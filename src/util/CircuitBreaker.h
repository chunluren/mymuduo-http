#pragma once

#include <mutex>
#include <chrono>
#include <functional>

class CircuitBreaker {
public:
    enum State { Closed, Open, HalfOpen };

    CircuitBreaker(int failureThreshold, int successThreshold, int timeoutSec)
        : failureThreshold_(failureThreshold)
        , successThreshold_(successThreshold)
        , timeoutMs_(timeoutSec * 1000)
        , state_(Closed)
        , failureCount_(0)
        , successCount_(0)
        , lastFailureTime_(0)
    {}

    State state() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == Open && nowMs() - lastFailureTime_ >= timeoutMs_) {
            state_ = HalfOpen;
            successCount_ = 0;
        }
        return state_;
    }

    bool allow() {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (state_) {
            case Closed: return true;
            case Open:
                if (nowMs() - lastFailureTime_ >= timeoutMs_) {
                    state_ = HalfOpen;
                    successCount_ = 0;
                    return true;
                }
                return false;
            case HalfOpen: return true;
        }
        return false;
    }

    void recordSuccess() {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (state_) {
            case Closed:
                failureCount_ = 0;
                break;
            case HalfOpen:
                successCount_++;
                if (successCount_ >= successThreshold_) {
                    state_ = Closed;
                    failureCount_ = 0;
                    successCount_ = 0;
                }
                break;
            case Open: break;
        }
    }

    void recordFailure() {
        std::lock_guard<std::mutex> lock(mutex_);
        lastFailureTime_ = nowMs();
        switch (state_) {
            case Closed:
                failureCount_++;
                if (failureCount_ >= failureThreshold_) {
                    state_ = Open;
                }
                break;
            case HalfOpen:
                state_ = Open;
                break;
            case Open: break;
        }
    }

    template<typename Func>
    auto execute(Func&& func) -> decltype(func()) {
        using ReturnType = decltype(func());
        if (!allow()) return ReturnType{};
        try {
            auto result = func();
            recordSuccess();
            return result;
        } catch (...) {
            recordFailure();
            return ReturnType{};
        }
    }

private:
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    int failureThreshold_;
    int successThreshold_;
    int64_t timeoutMs_;
    State state_;
    int failureCount_;
    int successCount_;
    int64_t lastFailureTime_;
    std::mutex mutex_;
};
