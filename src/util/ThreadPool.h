/**
 * @file ThreadPool.h
 * @brief 业务 worker 线程池：把会阻塞 IO 线程的活 (DB / 外部 HTTP) 切走
 *
 * 设计要点（与 EventLoop 解耦，纯 std::thread）：
 *
 *  - N 个 worker，每个一条独立队列 + mutex + cv（不共享一条队列以避免单锁热点）
 *  - 提交两种语义：
 *      submit(task)              → round-robin 选 worker（适合无序业务）
 *      submitAffinity(key, task) → key 一致性 hash 到固定 worker，保证同 key 有序
 *  - 背压：每条队列长度 maxQueueDepth_（默认 10240），满则 submit 返回 false，
 *    上层应把客户端打回 429/503，而不是无限堆积
 *  - 关闭：dtor 设 running_=false → notify_all → join，已入队任务 drain 完毕再退出
 *
 * 使用：
 * @code
 *   ThreadPool pool(8);
 *   pool.start();
 *   pool.submitAffinity(uid, [=]{ doSlowDbWork(uid); });
 *   // ...
 *   // dtor 自动 stop+drain，或显式 pool.stop()
 * @endcode
 *
 * 跨线程取回结果：worker 线程不应持有 IO 资源（ws session、buffer 指针等）。
 * 标准模式：worker 跑完后用 EventLoop::runInLoop 把回调切回 IO 线程。
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ThreadPool {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t numThreads, size_t maxQueueDepth = 10240,
                        std::string name = "worker")
        : numThreads_(numThreads),
          maxQueueDepth_(maxQueueDepth),
          name_(std::move(name)),
          workers_(numThreads) {}

    ~ThreadPool() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        for (size_t i = 0; i < numThreads_; ++i) {
            workers_[i].thread = std::thread([this, i]() { workerLoop(i); });
        }
    }

    void stop() {
        if (!running_.exchange(false)) return;
        // 唤醒所有 worker
        for (auto& w : workers_) {
            std::lock_guard<std::mutex> lk(w.mu);
            w.cv.notify_all();
        }
        for (auto& w : workers_) {
            if (w.thread.joinable()) w.thread.join();
        }
    }

    /// 轮询派发；满则丢弃，返回 false
    bool submit(Task task) {
        if (!running_.load() || numThreads_ == 0) return false;
        size_t idx = nextRoundRobin_.fetch_add(1, std::memory_order_relaxed) % numThreads_;
        return enqueue(idx, std::move(task));
    }

    /// 亲和性派发：同一 key 总是落同一 worker，保证该 key 任务的相对顺序
    bool submitAffinity(uint64_t key, Task task) {
        if (!running_.load() || numThreads_ == 0) return false;
        size_t idx = static_cast<size_t>(key) % numThreads_;
        return enqueue(idx, std::move(task));
    }

    size_t size() const { return numThreads_; }

    /// 当前所有 worker 队列的总积压（监控用，非精确）
    size_t pendingTasks() const {
        size_t total = 0;
        for (auto& w : workers_) {
            std::lock_guard<std::mutex> lk(w.mu);
            total += w.queue.size();
        }
        return total;
    }

    /// 指定 worker 当前队列长度（监控用）
    size_t pendingTasks(size_t workerIdx) const {
        if (workerIdx >= numThreads_) return 0;
        std::lock_guard<std::mutex> lk(workers_[workerIdx].mu);
        return workers_[workerIdx].queue.size();
    }

    /// 队列上限（背压阈值）
    size_t maxQueueDepth() const { return maxQueueDepth_; }

    /// 累计被丢弃的任务数（监控用）
    uint64_t droppedTasks() const { return droppedTasks_.load(std::memory_order_relaxed); }

private:
    struct Worker {
        std::thread thread;
        // mutable 让 const 成员函数也能锁
        mutable std::mutex mu;
        std::condition_variable cv;
        std::deque<Task> queue;
    };

    bool enqueue(size_t idx, Task&& task) {
        Worker& w = workers_[idx];
        {
            std::lock_guard<std::mutex> lk(w.mu);
            if (w.queue.size() >= maxQueueDepth_) {
                droppedTasks_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            w.queue.emplace_back(std::move(task));
        }
        w.cv.notify_one();
        return true;
    }

    void workerLoop(size_t idx) {
        Worker& w = workers_[idx];
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lk(w.mu);
                w.cv.wait(lk, [&]() { return !w.queue.empty() || !running_.load(); });
                if (!running_.load() && w.queue.empty()) return;
                task = std::move(w.queue.front());
                w.queue.pop_front();
            }
            // 业务回调里抛异常会在 std::thread 里 std::terminate；
            // 这里包一层 try 防止单个任务杀死整个 worker
            try {
                task();
            } catch (const std::exception& e) {
                // 不依赖项目 logger，避免循环依赖；用 stderr 不会丢
                fprintf(stderr, "[ThreadPool %s] worker %zu task exception: %s\n",
                        name_.c_str(), idx, e.what());
            } catch (...) {
                fprintf(stderr, "[ThreadPool %s] worker %zu unknown exception\n",
                        name_.c_str(), idx);
            }
        }
    }

    const size_t numThreads_;
    const size_t maxQueueDepth_;
    const std::string name_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> nextRoundRobin_{0};
    std::atomic<uint64_t> droppedTasks_{0};
    mutable std::vector<Worker> workers_;
};
