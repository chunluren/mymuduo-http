/**
 * @file test_thread_pool.cpp
 * @brief ThreadPool 单测：基础提交、亲和保序、背压、关闭 drain
 */
#include "util/ThreadPool.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#define TEST(name) static void name()
#define RUN_TEST(name) do { std::cerr << "[run] " #name << "\n"; name(); std::cerr << "[ok]  " #name << "\n"; } while (0)

TEST(basic_submit) {
    ThreadPool pool(4);
    pool.start();
    std::atomic<int> count{0};
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        bool ok = pool.submit([&]() { count.fetch_add(1); });
        assert(ok);
    }
    pool.stop();  // dtor 也会做，但显式 stop 后 count 应该 == N
    assert(count.load() == N);
}

TEST(affinity_serializes_per_key) {
    // 同 key 任务一定落同一 worker → 该 key 内部观察到的执行计数严格递增
    ThreadPool pool(4);
    pool.start();

    constexpr int kKeys = 8;
    constexpr int kPerKey = 200;

    std::mutex mu;
    std::unordered_map<uint64_t, int> lastSeen;
    std::atomic<bool> outOfOrder{false};

    for (int seq = 0; seq < kPerKey; ++seq) {
        for (uint64_t k = 0; k < kKeys; ++k) {
            int mySeq = seq;
            pool.submitAffinity(k, [k, mySeq, &mu, &lastSeen, &outOfOrder]() {
                // 慢一点放大乱序窗口
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                std::lock_guard<std::mutex> lk(mu);
                auto it = lastSeen.find(k);
                if (it != lastSeen.end() && mySeq <= it->second) {
                    outOfOrder.store(true);
                }
                lastSeen[k] = mySeq;
            });
        }
    }
    pool.stop();
    assert(!outOfOrder.load());
    assert(lastSeen.size() == kKeys);
    for (auto& [_, v] : lastSeen) assert(v == kPerKey - 1);
}

TEST(backpressure_drops_when_full) {
    // 队列上限 4 → 超过返回 false 且 droppedTasks() 自增
    ThreadPool pool(/*workers=*/1, /*maxQueueDepth=*/4);
    pool.start();

    // 第一个任务长跑，把 worker 占住，让队列堆积到上限
    pool.submitAffinity(0, []() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    });
    // 给 worker 时间把 task1 从队列里取走，避免它仍占着一个槽位影响计数
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    int accepted = 0, rejected = 0;
    for (int i = 0; i < 20; ++i) {
        if (pool.submitAffinity(0, [](){})) accepted++; else rejected++;
    }
    // 队列上限 4 → 应当恰好 4 个被接受，其余被拒绝
    assert(accepted == 4);
    assert(rejected == 16);
    assert(pool.droppedTasks() == 16);
    pool.stop();
}

TEST(stop_drains_pending_tasks) {
    // dtor / stop 应该让已经在队列里的任务跑完
    ThreadPool pool(2);
    pool.start();
    std::atomic<int> count{0};
    for (int i = 0; i < 50; ++i) {
        pool.submit([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            count.fetch_add(1);
        });
    }
    pool.stop();
    assert(count.load() == 50);
}

TEST(exception_in_task_does_not_kill_worker) {
    ThreadPool pool(1);
    pool.start();
    std::atomic<int> okCount{0};

    pool.submit([]() { throw std::runtime_error("boom"); });
    pool.submit([&]() { okCount.fetch_add(1); });
    pool.submit([&]() { okCount.fetch_add(1); });
    pool.stop();

    assert(okCount.load() == 2);
}

int main() {
    RUN_TEST(basic_submit);
    RUN_TEST(affinity_serializes_per_key);
    RUN_TEST(backpressure_drops_when_full);
    RUN_TEST(stop_drains_pending_tasks);
    RUN_TEST(exception_in_task_does_not_kill_worker);
    std::cerr << "ALL OK\n";
    return 0;
}
