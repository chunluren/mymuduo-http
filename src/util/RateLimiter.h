/**
 * @file RateLimiter.h
 * @brief Rate limiter implementations (Token Bucket + Sliding Window)
 *
 * Header-only rate limiters for per-key request throttling.
 * Thread-safe with internal mutex.
 *
 * @example
 * @code
 * // Token bucket: 10 tokens/sec, burst capacity 10
 * TokenBucketLimiter limiter(10, 10);
 * if (limiter.allow(clientIp)) { handle(req); }
 *
 * // Sliding window: 100 requests per 60 seconds
 * SlidingWindowLimiter limiter(100, 60);
 * if (limiter.allow(clientIp)) { handle(req); }
 * @endcode
 */

#pragma once

#include <string>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <chrono>

/**
 * @class TokenBucketLimiter
 * @brief Token bucket rate limiter with per-key tracking
 *
 * Each key maintains its own bucket with a configurable refill rate
 * and burst capacity. Tokens are refilled based on elapsed time.
 */
class TokenBucketLimiter {
public:
    /**
     * @brief Construct a token bucket limiter
     * @param rate Tokens refilled per second
     * @param burst Maximum bucket capacity (also initial token count)
     */
    TokenBucketLimiter(double rate, int burst)
        : rate_(rate), burst_(burst) {}

    /**
     * @brief 判断指定 key 的请求是否被允许（令牌桶算法）
     * @param key 限流标识符（如客户端 IP 地址）
     * @return true 请求放行，false 请求被限流
     *
     * 算法逻辑:
     * 1. 首次请求: 创建满桶（burst 个令牌），消耗 1 个后立即放行
     * 2. 后续请求: 根据距上次访问的时间差，按 rate（令牌/秒）补充令牌，
     *    令牌总量不超过 burst 上限
     * 3. 若桶中令牌 >= 1.0，消耗 1 个令牌并放行；否则拒绝
     *
     * 特点: 允许短时间突发流量（burst），同时限制长期平均速率（rate）
     */
    bool allow(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = nowMs();
        auto it = buckets_.find(key);

        if (it == buckets_.end()) {
            /// 该 key 的首次请求: 初始化满桶，消耗 1 个令牌
            Bucket bucket;
            bucket.tokens = static_cast<double>(burst_) - 1.0;
            bucket.lastTime = now;
            buckets_[key] = bucket;
            return true;
        }

        Bucket& bucket = it->second;

        /// 按时间差补充令牌: elapsed（秒） * rate_（令牌/秒），上限为 burst_
        double elapsed = static_cast<double>(now - bucket.lastTime) / 1000.0;
        bucket.tokens = std::min(static_cast<double>(burst_),
                                  bucket.tokens + elapsed * rate_);
        bucket.lastTime = now;

        /// 尝试消耗 1 个令牌
        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            return true;
        }

        return false;  ///< 令牌不足，拒绝请求
    }

private:
    struct Bucket {
        double tokens;    ///< Current token count
        int64_t lastTime; ///< Last refill time in milliseconds
    };

    double rate_;    ///< Tokens per second
    int burst_;      ///< Maximum bucket capacity
    std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;

    /// Get current time in milliseconds since epoch
    static int64_t nowMs() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
    }
};

/**
 * @class SlidingWindowLimiter
 * @brief Sliding window rate limiter with per-key tracking
 *
 * Each key maintains a deque of request timestamps. Old entries
 * outside the window are cleaned on each call.
 */
class SlidingWindowLimiter {
public:
    /**
     * @brief Construct a sliding window limiter
     * @param maxRequests Maximum requests allowed within the window
     * @param windowSec Window duration in seconds
     */
    SlidingWindowLimiter(int maxRequests, int windowSec)
        : maxRequests_(maxRequests), windowMs_(windowSec * 1000) {}

    /**
     * @brief 判断指定 key 的请求是否被允许（滑动窗口算法）
     * @param key 限流标识符（如客户端 IP 地址）
     * @return true 请求放行，false 请求被限流
     *
     * 算法逻辑:
     * 1. 维护一个双端队列（deque），记录每次请求的时间戳
     * 2. 每次请求时，先清除窗口外（超过 windowMs_ 毫秒前）的过期记录
     * 3. 若窗口内的请求数 < maxRequests_，记录本次时间戳并放行
     * 4. 否则拒绝请求
     *
     * 特点: 精确统计滑动窗口内的请求数量，不存在令牌桶的突发问题，
     * 但内存开销与请求数成正比（每个请求占用一个时间戳记录）
     */
    bool allow(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = nowMs();
        auto& window = windows_[key];

        /// 清除超出时间窗口的过期记录（从队列头部移除）
        int64_t cutoff = now - windowMs_;
        while (!window.empty() && window.front() <= cutoff) {
            window.pop_front();
        }

        /// 窗口内请求数未超限，记录本次请求时间戳并放行
        if (static_cast<int>(window.size()) < maxRequests_) {
            window.push_back(now);
            return true;
        }

        return false;  ///< 窗口内请求数已达上限，拒绝请求
    }

private:
    int maxRequests_;   ///< Maximum requests per window
    int64_t windowMs_;  ///< Window duration in milliseconds
    std::mutex mutex_;
    std::unordered_map<std::string, std::deque<int64_t>> windows_;

    /// Get current time in milliseconds since epoch
    static int64_t nowMs() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
    }
};
