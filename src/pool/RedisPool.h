/**
 * @file RedisPool.h
 * @brief Redis 连接池
 *
 * 基于 hiredis 实现的线程安全 Redis 连接池。
 * 支持基本的 GET/SET/DEL、过期时间、列表操作等。
 *
 * @example 使用示例
 * @code
 * RedisPoolConfig config;
 * config.host = "127.0.0.1";
 * config.port = 6379;
 *
 * RedisPool pool(config);
 *
 * auto conn = pool.acquire();
 * if (conn && conn->valid()) {
 *     conn->set("key", "value", 300);  // TTL 300s
 *     auto val = conn->get("key");     // "value"
 *     pool.release(std::move(conn));
 * }
 * @endcode
 */

#pragma once

#include "util/Metrics.h"
#include <hiredis/hiredis.h>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <cstdarg>

/// Redis 连接池配置
struct RedisPoolConfig {
    std::string host = "127.0.0.1";     ///< Redis 主机地址（直连模式）
    int port = 6379;                     ///< Redis 端口（直连模式）
    std::string password;                ///< 认证密码（空则不认证）
    int db = 0;                          ///< 数据库编号
    size_t minSize = 5;                  ///< 最小连接数
    size_t maxSize = 20;                 ///< 最大连接数
    int idleTimeoutSec = 60;             ///< 空闲超时（秒）
    int connectTimeoutSec = 5;           ///< 连接超时（秒）

    /// Sentinel 模式（可选）：填了 sentinels 时 host/port 被忽略，
    /// 每次新建连接前先问 sentinel 拿当前主地址。Sentinel 自动 failover
    /// 后下一次 createConnection 会自然切到新主。
    /// 格式：host:port，e.g. "127.0.0.1:26379"
    std::vector<std::string> sentinels;
    std::string sentinelMaster = "im-master";  ///< sentinel 监控的主名
};

/**
 * @class RedisConnection
 * @brief 单个 Redis 连接封装
 */
class RedisConnection {
public:
    using Ptr = std::shared_ptr<RedisConnection>;

    /// RAII 包装 redisReply
    struct Reply {
        redisReply* raw;

        explicit Reply(redisReply* r) : raw(r) {}
        ~Reply() { if (raw) freeReplyObject(raw); }

        Reply(const Reply&) = delete;
        Reply& operator=(const Reply&) = delete;
        Reply(Reply&& other) noexcept : raw(other.raw) { other.raw = nullptr; }
        Reply& operator=(Reply&&) = delete;

        bool ok() const { return raw && raw->type != REDIS_REPLY_ERROR; }
        bool isNil() const { return !raw || raw->type == REDIS_REPLY_NIL; }

        std::string str() const {
            if (!raw || raw->type != REDIS_REPLY_STRING) return "";
            return std::string(raw->str, raw->len);
        }

        long long integer() const {
            if (!raw || raw->type != REDIS_REPLY_INTEGER) return 0;
            return raw->integer;
        }

        std::string error() const {
            if (!raw || raw->type != REDIS_REPLY_ERROR) return "";
            return std::string(raw->str, raw->len);
        }
    };

    explicit RedisConnection(redisContext* ctx)
        : ctx_(ctx), lastUsed_(nowSec()) {}

    ~RedisConnection() {
        if (ctx_) redisFree(ctx_);
    }

    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;

    bool valid() const { return ctx_ != nullptr && ctx_->err == 0; }

    bool ping() {
        if (!ctx_) return false;
        auto r = command("PING");
        return r.ok();
    }

    Reply command(const char* fmt, ...) {
        if (!ctx_) return Reply(nullptr);
        va_list ap;
        va_start(ap, fmt);
        auto* reply = static_cast<redisReply*>(redisvCommand(ctx_, fmt, ap));
        va_end(ap);
        return Reply(reply);
    }

    std::string get(const std::string& key) {
        auto r = command("GET %s", key.c_str());
        return r.str();
    }

    bool set(const std::string& key, const std::string& value, int ttlSec = 0) {
        Reply r = (ttlSec > 0)
            ? command("SET %s %s EX %d", key.c_str(), value.c_str(), ttlSec)
            : command("SET %s %s", key.c_str(), value.c_str());
        return r.ok();
    }

    bool del(const std::string& key) {
        auto r = command("DEL %s", key.c_str());
        return r.ok();
    }

    bool exists(const std::string& key) {
        auto r = command("EXISTS %s", key.c_str());
        return r.integer() > 0;
    }

    bool expire(const std::string& key, int seconds) {
        auto r = command("EXPIRE %s %d", key.c_str(), seconds);
        return r.ok();
    }

    long long incr(const std::string& key) {
        auto r = command("INCR %s", key.c_str());
        return r.integer();
    }

    long long lpush(const std::string& key, const std::string& value) {
        auto r = command("LPUSH %s %s", key.c_str(), value.c_str());
        return r.integer();
    }

    std::vector<std::string> lrange(const std::string& key, int start, int stop) {
        std::vector<std::string> result;
        auto r = command("LRANGE %s %d %d", key.c_str(), start, stop);
        if (r.raw && r.raw->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i < r.raw->elements; ++i) {
                if (r.raw->element[i]->type == REDIS_REPLY_STRING) {
                    result.emplace_back(r.raw->element[i]->str, r.raw->element[i]->len);
                }
            }
        }
        return result;
    }

    bool ltrim(const std::string& key, int start, int stop) {
        auto r = command("LTRIM %s %d %d", key.c_str(), start, stop);
        return r.ok();
    }

    int64_t lastUsed() const { return lastUsed_; }
    void markUsed() { lastUsed_ = nowSec(); }

private:
    static int64_t nowSec() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    redisContext* ctx_;
    int64_t lastUsed_;
};

/**
 * @class RedisPool
 * @brief Redis 连接池（线程安全版）
 */
class RedisPool {
public:
    explicit RedisPool(const RedisPoolConfig& config)
        : config_(config), totalCreated_(0), closed_(false)
    {
        for (size_t i = 0; i < config.minSize; ++i) {
            auto conn = createConnection();
            if (conn && conn->valid()) {
                std::lock_guard<std::mutex> lock(mutex_);
                pool_.push(std::move(conn));
                totalCreated_++;
            }
        }
    }

    ~RedisPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
        std::lock_guard<std::mutex> lock(mutex_);
        while (!pool_.empty()) pool_.pop();
    }

    RedisConnection::Ptr acquire(int timeoutMs = 5000) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return closed_ || !pool_.empty() || totalCreated_ < config_.maxSize; });
        if (closed_) return nullptr;

        if (!pool_.empty()) {
            auto conn = std::move(pool_.front());
            pool_.pop();
            if (conn->ping()) {
                conn->markUsed();
                return conn;
            }
            if (totalCreated_ > 0) totalCreated_--;
        }

        if (totalCreated_ < config_.maxSize) {
            totalCreated_++;
            lock.unlock();
            auto conn = createConnection();
            lock.lock();
            if (!conn || !conn->valid()) {
                totalCreated_--;
                return nullptr;
            }
            conn->markUsed();
            return conn;
        }
        return nullptr;
    }

    void release(RedisConnection::Ptr conn) {
        if (!conn) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || !conn->valid()) {
            if (totalCreated_ > 0) totalCreated_--;
            cv_.notify_one();
            return;
        }
        conn->markUsed();
        pool_.push(std::move(conn));
        cv_.notify_one();
    }

    void healthCheck() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::queue<RedisConnection::Ptr> valid;
        while (!pool_.empty()) {
            auto conn = std::move(pool_.front());
            pool_.pop();
            if (now - conn->lastUsed() > config_.idleTimeoutSec &&
                valid.size() >= config_.minSize) {
                if (totalCreated_ > 0) totalCreated_--;
            } else {
                valid.push(std::move(conn));
            }
        }
        pool_ = std::move(valid);
    }

    size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pool_.size();
    }

    size_t totalCreated() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return totalCreated_;
    }

    bool isClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    /// 跑一遍所有 sentinel 问 "当前主在哪"，第一个回答的就采用。
    /// 失败时返回 {"", 0}。
    static std::pair<std::string,int> resolveMasterViaSentinel(
            const std::vector<std::string>& sentinels,
            const std::string& masterName,
            int connectTimeoutSec) {
        struct timeval tv{ connectTimeoutSec, 0 };
        for (const auto& addr : sentinels) {
            auto colon = addr.find(':');
            if (colon == std::string::npos) continue;
            std::string h = addr.substr(0, colon);
            int p;
            try { p = std::stoi(addr.substr(colon + 1)); }
            catch (...) { continue; }

            redisContext* sctx = redisConnectWithTimeout(h.c_str(), p, tv);
            if (!sctx || sctx->err) {
                if (sctx) redisFree(sctx);
                continue;
            }
            auto* reply = static_cast<redisReply*>(redisCommand(
                sctx, "SENTINEL get-master-addr-by-name %s", masterName.c_str()));
            if (reply && reply->type == REDIS_REPLY_ARRAY && reply->elements == 2
                && reply->element[0]->type == REDIS_REPLY_STRING
                && reply->element[1]->type == REDIS_REPLY_STRING) {
                std::string mh(reply->element[0]->str, reply->element[0]->len);
                int mp = 0;
                try { mp = std::stoi(std::string(reply->element[1]->str,
                                                 reply->element[1]->len)); }
                catch (...) { mp = 0; }
                freeReplyObject(reply);
                redisFree(sctx);
                if (mp > 0) return {mh, mp};
            } else if (reply) {
                freeReplyObject(reply);
            }
            redisFree(sctx);
        }
        return {"", 0};
    }

    RedisConnection::Ptr createConnection() {
        struct timeval tv;
        tv.tv_sec = config_.connectTimeoutSec;
        tv.tv_usec = 0;

        // Sentinel 模式：每次新建连接都重新解析当前主。Sentinel 自动 failover
        // 后下一次 createConnection 自然切到新主，业务层无感。
        std::string targetHost = config_.host;
        int         targetPort = config_.port;
        if (!config_.sentinels.empty()) {
            auto resolved = resolveMasterViaSentinel(
                config_.sentinels, config_.sentinelMaster,
                config_.connectTimeoutSec);
            if (resolved.second > 0) {
                targetHost = resolved.first;
                targetPort = resolved.second;
                // Phase 5b.6: failover 检测 — master 地址变了就 ++counter
                std::string newMaster = targetHost + ":" + std::to_string(targetPort);
                std::lock_guard<std::mutex> lk(masterMu_);
                if (!lastMaster_.empty() && lastMaster_ != newMaster) {
                    Metrics::instance().increment("redis_sentinel_master_switches_total");
                }
                lastMaster_ = newMaster;
            }
            // 解析失败：退回到 host/port 兜底（最坏情况下还能连上老主）
        }

        redisContext* ctx = redisConnectWithTimeout(
            targetHost.c_str(), targetPort, tv);

        if (!ctx || ctx->err) {
            if (ctx) redisFree(ctx);
            return std::make_shared<RedisConnection>(nullptr);
        }

        if (!config_.password.empty()) {
            auto* reply = static_cast<redisReply*>(
                redisCommand(ctx, "AUTH %s", config_.password.c_str()));
            if (!reply || reply->type == REDIS_REPLY_ERROR) {
                if (reply) freeReplyObject(reply);
                redisFree(ctx);
                return std::make_shared<RedisConnection>(nullptr);
            }
            freeReplyObject(reply);
        }

        if (config_.db > 0) {
            auto* reply = static_cast<redisReply*>(
                redisCommand(ctx, "SELECT %d", config_.db));
            if (reply) freeReplyObject(reply);
        }

        return std::make_shared<RedisConnection>(ctx);
    }

    RedisPoolConfig config_;
    size_t totalCreated_;
    bool closed_;

    std::queue<RedisConnection::Ptr> pool_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // Phase 5b.6: sentinel failover 检测
    mutable std::mutex masterMu_;
    std::string         lastMaster_;
};
