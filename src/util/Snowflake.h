/**
 * @file Snowflake.h
 * @brief 64-bit 分布式全局唯一 ID 生成器（Twitter Snowflake 算法变体）
 *
 * 设计:
 * @verbatim
 *   | 1 bit symbol | 41 bit timestamp (ms) | 10 bit worker_id | 12 bit sequence |
 *   |      0       |   since custom epoch  |    0 ~ 1023      |    0 ~ 4095     |
 * @endverbatim
 *
 * - 符号位恒 0，保证生成的 int64_t 为正数
 * - 41 bit 毫秒时间戳，基于自定义 epoch（2026-01-01），可用约 69 年
 * - 10 bit worker_id，支持 1024 个实例（从 MUDUO_IM_WORKER_ID 环境变量注入）
 * - 12 bit sequence，同一毫秒内最多生成 4096 个 ID，耗尽后 spin 到下一毫秒
 *
 * 时钟回拨处理:
 * - 回拨 <= 5 ms：sleep 等待直至 catch up
 * - 回拨  > 5 ms：抛异常（上层负责告警 + 下线该实例）
 *
 * 线程安全: 内部 std::mutex 保护 lastTimestamp_ 与 sequence_
 *
 * @example 使用示例
 * @code
 * Snowflake::instance().init(0);  // worker_id = 0
 * int64_t msgId = Snowflake::instance().nextId();
 *
 * // 反解时间戳（调试用）
 * int64_t ts = Snowflake::extractTimestamp(msgId);
 * @endcode
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace mymuduo {

class Snowflake {
public:
    /// 自定义 epoch：2026-01-01 00:00:00 UTC（毫秒）
    static constexpr int64_t kEpoch = 1735689600000LL;

    static constexpr int64_t kWorkerIdBits = 10;
    static constexpr int64_t kSequenceBits = 12;

    static constexpr int64_t kMaxWorkerId = (1LL << kWorkerIdBits) - 1;  // 1023
    static constexpr int64_t kMaxSequence = (1LL << kSequenceBits) - 1;  // 4095

    static constexpr int64_t kWorkerIdShift = kSequenceBits;          // 12
    static constexpr int64_t kTimestampShift = kSequenceBits + kWorkerIdBits; // 22

    /// 允许的时钟回拨阈值：回拨 <= 此值时等待，> 此值时抛异常
    static constexpr int64_t kMaxClockRollbackMs = 5;

    /// 全局单例访问
    static Snowflake& instance() {
        static Snowflake s;
        return s;
    }

    /**
     * @brief 初始化 worker_id
     * @param workerId 0 ~ 1023
     * @throw std::invalid_argument workerId 越界
     * @throw std::logic_error 已初始化后重复调用
     *
     * 必须在调用 nextId() 之前调用一次。线程不安全，应在启动阶段单线程完成。
     */
    void init(int64_t workerId) {
        if (workerId < 0 || workerId > kMaxWorkerId) {
            throw std::invalid_argument(
                "Snowflake workerId out of range [0," +
                std::to_string(kMaxWorkerId) + "]: " +
                std::to_string(workerId));
        }
        if (initialized_.exchange(true)) {
            throw std::logic_error("Snowflake already initialized");
        }
        workerId_ = workerId;
    }

    /**
     * @brief 生成下一个 ID
     * @return 64-bit 正整数 ID
     * @throw std::runtime_error 未 init、或时钟回拨超过 kMaxClockRollbackMs
     */
    int64_t nextId() {
        if (!initialized_.load(std::memory_order_acquire)) {
            throw std::runtime_error("Snowflake not initialized; call init() first");
        }

        std::lock_guard<std::mutex> lock(mutex_);
        int64_t now = currentMillis();

        if (now < lastTimestamp_) {
            int64_t offset = lastTimestamp_ - now;
            if (offset <= kMaxClockRollbackMs) {
                // 小幅回拨：等待直至 catch up
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(offset + 1));
                now = currentMillis();
                // 仍小于 lastTimestamp_（极罕见）则强制用 lastTimestamp_，sequence 递增
                if (now < lastTimestamp_) {
                    now = lastTimestamp_;
                }
            } else {
                // 大幅回拨：拒绝生成，让上层告警
                throw std::runtime_error(
                    "Clock rollback detected: " +
                    std::to_string(offset) + "ms > threshold " +
                    std::to_string(kMaxClockRollbackMs) + "ms");
            }
        }

        if (now == lastTimestamp_) {
            sequence_ = (sequence_ + 1) & kMaxSequence;
            if (sequence_ == 0) {
                // 当前毫秒 sequence 耗尽，spin 到下一毫秒
                while ((now = currentMillis()) <= lastTimestamp_) {
                    // busy wait，最坏 1ms
                }
            }
        } else {
            sequence_ = 0;
        }

        lastTimestamp_ = now;
        return ((now - kEpoch) << kTimestampShift)
             | (workerId_ << kWorkerIdShift)
             | sequence_;
    }

    /// 从 ID 反解出生成时的毫秒时间戳（UTC）
    static int64_t extractTimestamp(int64_t id) {
        return (id >> kTimestampShift) + kEpoch;
    }

    /// 从 ID 反解 worker_id
    static int64_t extractWorkerId(int64_t id) {
        return (id >> kWorkerIdShift) & kMaxWorkerId;
    }

    /// 从 ID 反解 sequence
    static int64_t extractSequence(int64_t id) {
        return id & kMaxSequence;
    }

    /// 当前 worker_id（若未 init 返回 -1）
    int64_t workerId() const {
        return initialized_.load() ? workerId_ : -1;
    }

    /// 从环境变量 MUDUO_IM_WORKER_ID 读取并 init。若未设置则默认 0
    void initFromEnv(const char* envName = "MUDUO_IM_WORKER_ID") {
        const char* v = std::getenv(envName);
        int64_t wid = 0;
        if (v != nullptr) {
            try {
                wid = std::stoll(v);
            } catch (const std::exception&) {
                throw std::invalid_argument(
                    std::string("Invalid ") + envName + "=" + v);
            }
        }
        init(wid);
    }

private:
    Snowflake() = default;
    Snowflake(const Snowflake&) = delete;
    Snowflake& operator=(const Snowflake&) = delete;

    static int64_t currentMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::atomic<bool> initialized_{false};
    int64_t workerId_ = 0;
    int64_t lastTimestamp_ = -1;
    int64_t sequence_ = 0;
    std::mutex mutex_;
};

}  // namespace mymuduo
