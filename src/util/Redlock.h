/**
 * @file Redlock.h
 * @brief Phase 6.2 Redis 分布式锁（Redlock 简化版 — 单 Redis 实例足够）
 *
 * 协议（与官方 Redlock 兼容子集）：
 *   acquire:  SET key token NX PX ttl_ms
 *             成功 → 返回 token；失败 → 返回 ""
 *   release:  Lua: if redis.call("GET", KEYS[1]) == ARGV[1]
 *                   then return redis.call("DEL", KEYS[1])
 *                   else return 0 end
 *             token 校验防"我刚释放，别人立刻拿到，我又 DEL 了别人的"竞争。
 *
 * 用法：
 *   Redlock lock(redisPool);
 *   auto token = lock.acquire("group:42:members", 5000);
 *   if (token.empty()) return; // 拿锁失败
 *   try {
 *       // 临界区
 *   } catch (...) {}
 *   lock.release("group:42:members", token);
 *
 * 已知边界（参考 Martin Kleppmann 论文）：
 *   - GC pause + 时钟漂移可能让某进程"以为持锁"实际已过期 → 关键路径
 *     必须 + DB UNIQUE 兜底。本类只做"普通并发防御"。
 *   - 单 Redis 实例时，主挂掉 + sentinel failover 期间持锁信息可能丢
 *     （AOF/RDB 间隔窗口）。多 Redis 实例 multi-instance 模式可缓解。
 */
#pragma once

#include "pool/RedisPool.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <thread>

class Redlock {
public:
    explicit Redlock(std::shared_ptr<RedisPool> pool) : pool_(std::move(pool)) {}

    /// 加锁；成功返回 token，失败返回 ""
    /// @param key       锁 key（建议带业务 prefix，如 "lock:group:42"）
    /// @param ttlMs     锁过期时间（5000ms 默认；调用方必须在此窗口内完成）
    /// @param retryMs   抢不到时多久重试一次（默认 100ms）
    /// @param maxWaitMs 最多等多久（0 = 不重试，立刻 fail；默认 0）
    std::string acquire(const std::string& key, int ttlMs = 5000,
                        int retryMs = 100, int maxWaitMs = 0) {
        std::string token = generateToken();
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(maxWaitMs);
        while (true) {
            if (trySet(key, token, ttlMs)) return token;
            if (maxWaitMs == 0 || std::chrono::steady_clock::now() >= deadline) {
                return {};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(retryMs));
        }
    }

    /// 释放锁。返回 true 表示我持有的锁被成功释放；false 表示锁已不归我（过期 / 被别人拿了）
    bool release(const std::string& key, const std::string& token) {
        if (token.empty()) return false;
        auto conn = pool_->acquire(2000);
        if (!conn || !conn->valid()) return false;
        // Lua 脚本保证 GET + DEL 原子，防止 token 校验通过后被别的 client 抢占的窗口
        const char* lua =
            "if redis.call('GET', KEYS[1]) == ARGV[1] "
            "then return redis.call('DEL', KEYS[1]) "
            "else return 0 end";
        auto r = conn->command("EVAL %s 1 %s %s", lua, key.c_str(), token.c_str());
        bool ok = r.raw && r.raw->type == REDIS_REPLY_INTEGER && r.raw->integer == 1;
        pool_->release(std::move(conn));
        return ok;
    }

    /// 锁是否仍由我持有（不会改 ttl，纯查询）
    bool isHeldByMe(const std::string& key, const std::string& token) {
        if (token.empty()) return false;
        auto conn = pool_->acquire(2000);
        if (!conn || !conn->valid()) return false;
        std::string current = conn->get(key);
        pool_->release(std::move(conn));
        return current == token;
    }

private:
    bool trySet(const std::string& key, const std::string& token, int ttlMs) {
        auto conn = pool_->acquire(2000);
        if (!conn || !conn->valid()) return false;
        // SET key value NX PX ttl_ms — 原子：只在 key 不存在时设置 + 自带 TTL
        auto r = conn->command("SET %s %s NX PX %d",
                               key.c_str(), token.c_str(), ttlMs);
        // 成功返回 status string "OK"，失败返回 nil
        bool ok = r.raw && r.raw->type == REDIS_REPLY_STATUS &&
                  std::string(r.raw->str, r.raw->len) == "OK";
        pool_->release(std::move(conn));
        return ok;
    }

    /// 生成随机 token（PID + 时间戳 + 计数器，跨进程唯一）
    static std::string generateToken() {
        static std::atomic<uint64_t> counter{0};
        uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t c = counter.fetch_add(1, std::memory_order_relaxed);
        thread_local std::mt19937_64 rng(std::random_device{}());
        uint64_t r = rng();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%llx-%llx-%llx",
                      (unsigned long long)now,
                      (unsigned long long)c,
                      (unsigned long long)r);
        return buf;
    }

    std::shared_ptr<RedisPool> pool_;
};

/**
 * @class RedlockGuard
 * @brief RAII：构造时拿锁，析构时释放
 *
 *   {
 *       RedlockGuard g(lock, "group:42:members", 5000);
 *       if (!g.held()) return;  // 没拿到
 *       // 临界区
 *   }  // 析构自动 release
 */
class RedlockGuard {
public:
    RedlockGuard(Redlock& lock, std::string key, int ttlMs = 5000,
                 int retryMs = 100, int maxWaitMs = 0)
        : lock_(lock), key_(std::move(key))
    {
        token_ = lock_.acquire(key_, ttlMs, retryMs, maxWaitMs);
    }
    ~RedlockGuard() { if (!token_.empty()) lock_.release(key_, token_); }
    RedlockGuard(const RedlockGuard&) = delete;
    RedlockGuard& operator=(const RedlockGuard&) = delete;

    bool held() const { return !token_.empty(); }
    const std::string& token() const { return token_; }

private:
    Redlock& lock_;
    std::string key_;
    std::string token_;
};
