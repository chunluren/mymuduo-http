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
     * @brief Check if a request is allowed for the given key
     * @param key Identifier for rate limiting (e.g., client IP)
     * @return true if request is allowed, false if rate limited
     */
    bool allow(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = nowMs();
        auto it = buckets_.find(key);

        if (it == buckets_.end()) {
            // First request for this key: start with full bucket, consume 1
            Bucket bucket;
            bucket.tokens = static_cast<double>(burst_) - 1.0;
            bucket.lastTime = now;
            buckets_[key] = bucket;
            return true;
        }

        Bucket& bucket = it->second;

        // Refill tokens based on elapsed time
        double elapsed = static_cast<double>(now - bucket.lastTime) / 1000.0;
        bucket.tokens = std::min(static_cast<double>(burst_),
                                  bucket.tokens + elapsed * rate_);
        bucket.lastTime = now;

        // Try to consume 1 token
        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            return true;
        }

        return false;
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
     * @brief Check if a request is allowed for the given key
     * @param key Identifier for rate limiting (e.g., client IP)
     * @return true if request is allowed, false if rate limited
     */
    bool allow(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = nowMs();
        auto& window = windows_[key];

        // Clean expired entries
        int64_t cutoff = now - windowMs_;
        while (!window.empty() && window.front() <= cutoff) {
            window.pop_front();
        }

        // Check if within limit
        if (static_cast<int>(window.size()) < maxRequests_) {
            window.push_back(now);
            return true;
        }

        return false;
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
