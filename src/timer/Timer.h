/**
 * @file Timer.h
 * @brief 定时器对象定义
 *
 * 本文件定义了 Timer 类，表示一个定时任务。
 * Timer 可以是一次性的，也可以是周期性重复的。
 *
 * 特性:
 * - 支持一次性定时器
 * - 支持周期性定时器
 * - 支持取消操作
 * - 线程安全的状态管理
 *
 * @example 使用示例
 * @code
 * // 创建一次性定时器，5秒后执行
 * Timer timer([]() {
 *     std::cout << "Timer fired!" << std::endl;
 * }, Timer::now() + 5000);
 *
 * // 创建周期性定时器，每1秒执行一次
 * Timer periodicTimer([]() {
 *     std::cout << "Periodic timer!" << std::endl;
 * }, Timer::now() + 1000, 1000);
 *
 * // 执行定时器
 * timer.run();
 * @endcode
 */

#pragma once

#include <functional>
#include <chrono>
#include <atomic>

/// 定时器回调函数类型
using TimerCallback = std::function<void()>;

/**
 * @class Timer
 * @brief 定时器对象类
 *
 * Timer 封装了一个定时任务:
 * - callback_: 定时到期时执行的回调函数
 * - expiration_: 到期时间 (毫秒时间戳)
 * - interval_: 重复间隔 (毫秒，0 表示一次性)
 * - cancelled_: 取消标志
 *
 * 线程安全:
 * - cancelled_ 是原子变量，可以在任何线程安全地取消定时器
 * - 其他成员变量应在创建时设置，之后不应修改
 */
class Timer {
public:
    /**
     * @brief 构造定时器
     * @param cb 回调函数，定时到期时执行
     * @param when 到期时间 (毫秒时间戳)
     * @param interval 重复间隔 (毫秒)，0 表示一次性定时器
     *
     * @example
     * @code
     * // 一次性定时器，1秒后执行
     * Timer t([](){}, Timer::now() + 1000, 0);
     *
     * // 周期性定时器，立即开始，每100ms执行一次
     * Timer t([](){}, Timer::now(), 100);
     * @endcode
     */
    Timer(TimerCallback cb, int64_t when, int64_t interval = 0)
        : callback_(std::move(cb))
        , expiration_(when)
        , interval_(interval)
        , repeat_(interval > 0)
        , cancelled_(false)
        , id_(nextId_++)
    {}

    /**
     * @brief 执行定时器回调
     *
     * 如果定时器未被取消且回调函数有效，则执行回调。
     * 这是 const 方法，不会修改定时器状态。
     */
    void run() const {
        if (!cancelled_ && callback_) {
            callback_();
        }
    }

    /**
     * @brief 取消定时器
     *
     * 设置取消标志，下次调用 run() 时不会执行回调。
     * 这是线程安全的操作。
     */
    void cancel() { cancelled_ = true; }

    /**
     * @brief 获取到期时间
     * @return 到期时间 (毫秒时间戳)
     */
    int64_t expiration() const { return expiration_; }

    /**
     * @brief 是否为周期性定时器
     * @return true 如果是周期性定时器
     */
    bool repeat() const { return repeat_; }

    /**
     * @brief 获取定时器 ID
     * @return 定时器唯一 ID
     */
    int64_t id() const { return id_; }

    /**
     * @brief 获取重复间隔
     * @return 重复间隔 (毫秒)，0 表示一次性定时器
     */
    int64_t interval() const { return interval_; }

    /**
     * @brief 检查是否已取消
     * @return true 如果已取消
     */
    bool isCancelled() const { return cancelled_; }

    /**
     * @brief 重启定时器 (用于周期性定时器)
     * @param now 当前时间 (毫秒时间戳)
     *
     * 对于周期性定时器，设置新的到期时间 = now + interval_
     */
    void restart(int64_t now) {
        if (repeat_) {
            expiration_ = now + interval_;
        }
    }

    /**
     * @brief 获取当前时间
     * @return 当前时间 (毫秒时间戳)
     *
     * 使用 steady_clock 保证时间单调递增，不受系统时间调整影响
     */
    static int64_t now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

private:
    TimerCallback callback_;       ///< 回调函数
    int64_t expiration_;           ///< 到期时间 (ms)
    int64_t interval_;             ///< 间隔 (ms)，0 表示一次性
    bool repeat_;                  ///< 是否周期性
    std::atomic<bool> cancelled_;  ///< 取消标志 (原子变量)
    int64_t id_;                   ///< 定时器唯一 ID

    static std::atomic<int64_t> nextId_;  ///< 下一个 ID (静态)
};

/// 静态成员定义 (C++17 inline static)
inline std::atomic<int64_t> Timer::nextId_{0};