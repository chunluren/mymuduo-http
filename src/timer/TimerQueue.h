// TimerQueue.h - 时间轮定时器队列
#pragma once

#include "Timer.h"
#include <vector>
#include <list>
#include <memory>
#include <mutex>

// 时间轮实现 - O(1) 插入/删除
class TimerQueue {
public:
    explicit TimerQueue(size_t buckets = 60, int tickMs = 1000)
        : buckets_(buckets)
        , tickMs_(tickMs)
        , currentBucket_(0)
        , nextTimerId_(0)
    {
        wheel_.resize(buckets);
    }
    
    // 添加定时器，返回 timerId
    int64_t addTimer(TimerCallback cb, int delayMs, int intervalMs = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        int64_t now = Timer::now();
        int64_t expiration = now + delayMs;
        
        auto timer = std::make_shared<Timer>(cb, expiration, intervalMs);
        int64_t timerId = timer->id();
        
        // 计算放入哪个桶
        size_t bucket = (currentBucket_ + delayMs / tickMs_) % buckets_;
        wheel_[bucket].push_back(timer);
        timers_[timerId] = timer;
        
        return timerId;
    }
    
    // 取消定时器
    void cancelTimer(int64_t timerId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = timers_.find(timerId);
        if (it != timers_.end()) {
            it->second->cancel();
            timers_.erase(it);
        }
    }
    
    // 时间轮推进（每个 tick 调用）
    void tick() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto& bucket = wheel_[currentBucket_];
        int64_t now = Timer::now();
        
        for (auto it = bucket.begin(); it != bucket.end(); ) {
            auto& timer = *it;
            
            if (timer->expiration() <= now) {
                // 执行回调
                timer->run();
                
                if (timer->repeat()) {
                    // 重复定时器，重新加入
                    timer->restart(now);
                    size_t newBucket = (currentBucket_ + timer->expiration() % tickMs_) % buckets_;
                    wheel_[newBucket].push_back(timer);
                } else {
                    timers_.erase(timer->id());
                }
                
                it = bucket.erase(it);
            } else {
                ++it;
            }
        }
        
        currentBucket_ = (currentBucket_ + 1) % buckets_;
    }
    
    // 获取最近的超时时间 (ms)
    int getNextTimeout() const {
        return tickMs_;  // 简化：固定 tick 间隔
    }
    
    size_t timerCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return timers_.size();
    }

private:
    size_t buckets_;
    int tickMs_;
    size_t currentBucket_;
    int64_t nextTimerId_;
    
    std::vector<std::list<std::shared_ptr<Timer>>> wheel_;
    std::unordered_map<int64_t, std::shared_ptr<Timer>> timers_;
    mutable std::mutex mutex_;
};