/**
 * @file TimerQueue.h
 * @brief 时间轮定时器队列
 *
 * 本文件定义了 TimerQueue 类，使用时间轮算法管理大量定时器。
 * 时间轮是一种高效的定时器管理数据结构，支持 O(1) 的插入和删除。
 *
 * 时间轮原理:
 * @code
 *                     [桶0]
 *            +-----> [桶1]
 *           /        [桶2]
 *          /         ...
 *         /          [桶N-1]
 *        /
 *   指针 --+---> 当前桶 (currentBucket_)
 *          \
 *           \
 *            +---> 时间轮转动 (tick)
 * @endcode
 *
 * 每个桶存放一个定时器链表，时间轮每次转动 (tick)，
 * 检查当前桶中的定时器是否到期。
 *
 * 优点:
 * - O(1) 插入
 * - O(1) 删除 (使用哈希表辅助)
 * - 适合管理大量定时器
 *
 * @example 使用示例
 * @code
 * TimerQueue timerQueue(60, 1000);  // 60 个桶，每桶 1 秒
 *
 * // 添加一次性定时器，5 秒后执行
 * int64_t timerId = timerQueue.addTimer([]() {
 *     std::cout << "Timer fired!" << std::endl;
 * }, 5000);
 *
 * // 添加周期性定时器，每 1 秒执行一次
 * int64_t periodicId = timerQueue.addTimer([]() {
 *     std::cout << "Periodic!" << std::endl;
 * }, 1000, 1000);
 *
 * // 在主循环中定期调用 tick
 * while (running) {
 *     timerQueue.tick();
 *     std::this_thread::sleep_for(std::chrono::milliseconds(100));
 * }
 *
 * // 取消定时器
 * timerQueue.cancelTimer(timerId);
 * @endcode
 */

#pragma once

#include "Timer.h"
#include <vector>
#include <list>
#include <memory>
#include <mutex>
#include <functional>
#include <unordered_map>

/**
 * @class TimerQueue
 * @brief 时间轮定时器队列
 *
 * 使用时间轮算法管理定时器，支持:
 * - 高效的定时器添加和删除
 * - 一次性定时器
 * - 周期性定时器
 * - 定时器取消
 *
 * 线程安全:
 * - 所有公共方法都是线程安全的
 * - 使用 mutex 保护内部数据结构
 *
 * 时间轮参数:
 * - buckets: 桶的数量，决定时间轮的大小
 * - tickMs: 每个 tick 的时间间隔 (毫秒)
 */
class TimerQueue {
public:
    /**
     * @brief 构造时间轮定时器队列
     * @param buckets 桶的数量 (默认 60)
     * @param tickMs 每个 tick 的时间间隔，毫秒 (默认 1000ms)
     *
     * 时间轮的覆盖范围 = buckets * tickMs
     * 例如: 60 桶 * 1000ms = 60 秒
     *
     * 定时器的延迟时间不能超过这个范围，否则会回绕
     */
    explicit TimerQueue(size_t buckets = 60, int tickMs = 1000)
        : buckets_(buckets)
        , tickMs_(tickMs)
        , currentBucket_(0)
    {
        wheel_.resize(buckets);
    }

    /**
     * @brief 添加定时器
     * @param cb 回调函数
     * @param delayMs 延迟时间 (毫秒)
     * @param intervalMs 重复间隔 (毫秒)，0 表示一次性定时器
     * @return 定时器 ID，用于取消定时器
     *
     * @example
     * @code
     * // 一次性定时器，5 秒后执行
     * int64_t id = timerQueue.addTimer(callback, 5000);
     *
     * // 周期性定时器，立即执行，之后每 1 秒执行一次
     * int64_t id = timerQueue.addTimer(callback, 0, 1000);
     * @endcode
     */
    int64_t addTimer(TimerCallback cb, int delayMs, int intervalMs = 0) {
        std::lock_guard<std::mutex> lock(mutex_);

        int64_t now = Timer::now();
        int64_t expiration = now + delayMs;

        auto timer = std::make_shared<Timer>(cb, expiration, intervalMs);
        int64_t timerId = timer->id();

        // 计算放入哪个桶
        // 向上取整: ticks = ceil(delayMs / tickMs_)
        size_t ticks = (delayMs + tickMs_ - 1) / tickMs_;
        if (ticks == 0) ticks = 1;  // 至少放入下一个桶

        size_t bucket = (currentBucket_ + ticks) % buckets_;

        wheel_[bucket].push_back(timer);
        timers_[timerId] = timer;

        return timerId;
    }

    /**
     * @brief 取消定时器
     * @param timerId 定时器 ID
     *
     * 从定时器表中移除定时器，并将其标记为已取消
     */
    void cancelTimer(int64_t timerId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = timers_.find(timerId);
        if (it != timers_.end()) {
            it->second->cancel();
            timers_.erase(it);
        }
    }

    /**
     * @brief 时间轮推进 (tick)
     *
     * 此方法应该定期调用，频率与 tickMs_ 匹配。
     *
     * 处理流程:
     * 1. 获取当前桶中的所有定时器
     * 2. 检查每个定时器是否到期
     * 3. 执行到期的定时器回调
     * 4. 对于周期性定时器，重新计算位置
     * 5. 移动到下一个桶
     *
     * @note 回调在锁外执行，避免死锁
     */
    void tick() {
        // 收集待执行的定时器
        std::vector<std::pair<std::shared_ptr<Timer>, bool>> expiredTimers;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto& bucket = wheel_[currentBucket_];
            int64_t now = Timer::now();

            for (auto it = bucket.begin(); it != bucket.end(); ) {
                auto& timer = *it;

                if (timer->expiration() <= now && !timer->isCancelled()) {
                    // 收集待执行回调
                    bool repeat = timer->repeat();
                    expiredTimers.push_back({timer, repeat});

                    if (repeat) {
                        // 周期性定时器，计算新位置
                        timer->restart(now);
                        size_t ticks = (timer->interval() + tickMs_ - 1) / tickMs_;
                        size_t newBucket = (currentBucket_ + ticks) % buckets_;
                        wheel_[newBucket].push_back(timer);
                    } else {
                        // 一次性定时器，从表中移除
                        timers_.erase(timer->id());
                    }

                    it = bucket.erase(it);
                } else if (timer->isCancelled()) {
                    // 已取消的定时器，移除
                    timers_.erase(timer->id());
                    it = bucket.erase(it);
                } else {
                    ++it;
                }
            }

            // 移动到下一个桶
            currentBucket_ = (currentBucket_ + 1) % buckets_;
        }

        // 锁外执行回调，避免死锁
        // 如果回调中调用 addTimer 或 cancelTimer，会导致死锁
        for (auto& [timer, repeat] : expiredTimers) {
            timer->run();
        }
    }

    /**
     * @brief 获取下一次 tick 的超时时间
     * @return 超时时间 (毫秒)
     */
    int getNextTimeout() const { return tickMs_; }

    /**
     * @brief 获取定时器数量
     * @return 当前活跃的定时器数量
     */
    size_t timerCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return timers_.size();
    }

private:
    size_t buckets_;      ///< 桶的数量
    int tickMs_;          ///< 每个 tick 的时间间隔 (毫秒)
    size_t currentBucket_; ///< 当前桶索引

    std::vector<std::list<std::shared_ptr<Timer>>> wheel_;  ///< 时间轮
    std::unordered_map<int64_t, std::shared_ptr<Timer>> timers_;  ///< 定时器 ID -> 定时器
    mutable std::mutex mutex_;  ///< 保护内部数据结构
};