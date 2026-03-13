// Timer.h - 定时器核心
#pragma once

#include <functional>
#include <chrono>
#include <atomic>

using TimerCallback = std::function<void()>;

class Timer {
public:
    Timer(TimerCallback cb, int64_t when, int64_t interval = 0)
        : callback_(std::move(cb))
        , expiration_(when)
        , interval_(interval)
        , repeat_(interval > 0)
        , cancelled_(false)
        , id_(nextId_++)
    {}
    
    void run() const {
        if (!cancelled_ && callback_) {
            callback_();
        }
    }
    
    void cancel() { cancelled_ = true; }
    
    int64_t expiration() const { return expiration_; }
    bool repeat() const { return repeat_; }
    int64_t id() const { return id_; }
    int64_t interval() const { return interval_; }
    bool isCancelled() const { return cancelled_; }
    
    void restart(int64_t now) {
        if (repeat_) {
            expiration_ = now + interval_;
        }
    }
    
    static int64_t now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

private:
    TimerCallback callback_;
    int64_t expiration_;  // 到期时间 (ms)
    int64_t interval_;    // 间隔 (ms), 0 表示一次性
    bool repeat_;
    std::atomic<bool> cancelled_;
    int64_t id_;
    
    static std::atomic<int64_t> nextId_;
};