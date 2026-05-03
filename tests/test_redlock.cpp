/**
 * @file test_redlock.cpp
 * @brief Phase 6.2 Redlock 单元 + 并发测试
 *
 * 前置：Redis @ 127.0.0.1:6379
 * 跳过：SKIP_REDLOCK_TEST=1
 */
#include "src/pool/RedisPool.h"
#include "src/util/Redlock.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#define TEST(name) static void name()
#define RUN_TEST(name) do { std::cerr << "[run] " #name "\n"; name(); std::cerr << "[ok]  " #name "\n"; } while (0)

static std::shared_ptr<RedisPool> mkPool() {
    RedisPoolConfig cfg;
    cfg.host = "127.0.0.1"; cfg.port = 6379;
    cfg.minSize = 5; cfg.maxSize = 50;
    return std::make_shared<RedisPool>(cfg);
}

TEST(redlock_basic_acquire_release) {
    auto pool = mkPool();
    Redlock lock(pool);
    auto t = lock.acquire("test:redlock:basic", 5000);
    assert(!t.empty());
    assert(lock.isHeldByMe("test:redlock:basic", t));
    assert(lock.release("test:redlock:basic", t));
    assert(!lock.isHeldByMe("test:redlock:basic", t));
}

TEST(redlock_token_mismatch_release_fails) {
    auto pool = mkPool();
    Redlock lock(pool);
    auto t = lock.acquire("test:redlock:token", 5000);
    assert(!t.empty());
    assert(!lock.release("test:redlock:token", "wrong-token")); // 别人 token 释放不掉
    assert(lock.release("test:redlock:token", t));               // 自己的 token 行
}

TEST(redlock_second_acquire_blocks_until_release) {
    auto pool = mkPool();
    Redlock lock(pool);
    auto t1 = lock.acquire("test:redlock:concurrent", 5000);
    assert(!t1.empty());
    // 第二个尝试 maxWait=0 → 立刻 fail
    auto t2 = lock.acquire("test:redlock:concurrent", 5000, 100, 0);
    assert(t2.empty());
    lock.release("test:redlock:concurrent", t1);
    // 释放后能拿
    auto t3 = lock.acquire("test:redlock:concurrent", 5000);
    assert(!t3.empty());
    lock.release("test:redlock:concurrent", t3);
}

TEST(redlock_100_concurrent_only_one_wins) {
    auto pool = mkPool();
    Redlock lock(pool);
    constexpr int N = 100;
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&]() {
            auto t = lock.acquire("test:redlock:race", 1000, 50, 0);
            if (!t.empty()) {
                winners.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                lock.release("test:redlock:race", t);
            }
        });
    }
    for (auto& th : threads) th.join();
    // 100 个线程同时 acquire 一个 key + 立刻 fail (maxWait=0) → 应该只有 1 个赢
    std::cerr << "  winners = " << winners.load() << " / 100\n";
    assert(winners.load() == 1);
}

TEST(redlock_ttl_expires_lets_next_caller_in) {
    auto pool = mkPool();
    Redlock lock(pool);
    auto t1 = lock.acquire("test:redlock:ttl", 200);   // 200ms TTL
    assert(!t1.empty());
    // 200ms 内别人拿不到
    auto t2 = lock.acquire("test:redlock:ttl", 5000, 100, 0);
    assert(t2.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    // TTL 过了别人能拿
    auto t3 = lock.acquire("test:redlock:ttl", 5000);
    assert(!t3.empty());
    // 我用旧 token release 应该返 false（不是我的了）
    assert(!lock.release("test:redlock:ttl", t1));
    // 当前持有者用 t3 能 release
    assert(lock.release("test:redlock:ttl", t3));
}

TEST(redlock_guard_raii) {
    auto pool = mkPool();
    Redlock lock(pool);
    {
        RedlockGuard g(lock, "test:redlock:guard", 5000);
        assert(g.held());
        // 在 guard 作用域内，别人拿不到
        auto t2 = lock.acquire("test:redlock:guard", 5000, 100, 0);
        assert(t2.empty());
    }   // guard 析构 → release
    // 现在能拿
    auto t = lock.acquire("test:redlock:guard", 5000);
    assert(!t.empty());
    lock.release("test:redlock:guard", t);
}

int main() {
    if (std::getenv("SKIP_REDLOCK_TEST")) { std::cerr << "SKIP\n"; return 0; }
    RUN_TEST(redlock_basic_acquire_release);
    RUN_TEST(redlock_token_mismatch_release_fails);
    RUN_TEST(redlock_second_acquire_blocks_until_release);
    RUN_TEST(redlock_100_concurrent_only_one_wins);
    RUN_TEST(redlock_ttl_expires_lets_next_caller_in);
    RUN_TEST(redlock_guard_raii);
    std::cerr << "ALL OK\n";
    return 0;
}
