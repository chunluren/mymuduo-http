/**
 * @file CircuitBreaker.h
 * @brief 熔断器实现 -- 三态状态机（Closed / Open / HalfOpen）
 *
 * 设计思路（参考 Martin Fowler 的 CircuitBreaker 模式）:
 *
 *   状态转换:
 *   @verbatim
 *     ┌────────────────────────────────────────────┐
 *     │              连续成功 >= successThreshold   │
 *     │   ┌──────┐     ┌──────────┐     ┌────────┐ │
 *     └──>│Closed│────>│   Open   │────>│HalfOpen│─┘
 *         └──────┘     └──────────┘     └────────┘
 *          失败数>=       超时到期后        任何一次
 *        failureThreshold  自动转入        失败立即
 *                        HalfOpen         回到 Open
 *   @endverbatim
 *
 * - **Closed（关闭）**: 正常放行所有请求；当连续失败数达到阈值时转入 Open。
 * - **Open（打开）**: 拒绝所有请求（快速失败）；超时后自动转入 HalfOpen 进行探测。
 * - **HalfOpen（半开）**: 允许少量请求通过；若连续成功达到阈值则恢复 Closed，
 *   任何一次失败立即回退到 Open。
 *
 * 线程安全: 所有公共方法内部通过 std::mutex 保护，可在多线程环境中直接使用。
 *
 * @example 使用示例
 * @code
 * CircuitBreaker cb(5, 3, 30);  // 5次失败熔断, 3次成功恢复, 30秒超时
 * auto result = cb.execute([&]() {
 *     return httpClient.request("/api/data");
 * });
 * @endcode
 */

#pragma once

#include <mutex>
#include <chrono>
#include <functional>

/**
 * @class CircuitBreaker
 * @brief 熔断器 -- 保护下游服务免受雪崩影响的三态状态机
 *
 * 当下游连续失败超过阈值时自动熔断（Open），经过超时恢复期后进入
 * 半开状态（HalfOpen）进行探测，探测成功则恢复正常（Closed）。
 */
class CircuitBreaker {
public:
    /**
     * @brief 熔断器状态枚举
     *
     * - Closed   : 正常放行，失败计数中
     * - Open     : 熔断中，拒绝所有请求
     * - HalfOpen : 试探恢复，允许少量请求通过
     */
    enum State { Closed, Open, HalfOpen };

    /**
     * @brief 构造熔断器
     * @param failureThreshold 连续失败多少次后从 Closed 转入 Open
     * @param successThreshold 在 HalfOpen 状态下连续成功多少次后恢复 Closed
     * @param timeoutSec       Open 状态持续多少秒后自动转入 HalfOpen（探测恢复）
     */
    CircuitBreaker(int failureThreshold, int successThreshold, int timeoutSec)
        : failureThreshold_(failureThreshold)
        , successThreshold_(successThreshold)
        , timeoutMs_(timeoutSec * 1000)
        , state_(Closed)
        , failureCount_(0)
        , successCount_(0)
        , lastFailureTime_(0)
    {}

    /**
     * @brief 获取当前状态（带自动超时转换）
     * @return 当前熔断器状态
     *
     * 如果处于 Open 状态且超时已到期，会自动转入 HalfOpen。
     */
    State state() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == Open && nowMs() - lastFailureTime_ >= timeoutMs_) {
            state_ = HalfOpen;
            successCount_ = 0;
        }
        return state_;
    }

    /**
     * @brief 判断当前是否允许请求通过
     * @return true 允许放行，false 熔断中应快速失败
     *
     * - Closed   : 始终允许
     * - Open     : 超时到期则转 HalfOpen 并允许，否则拒绝
     * - HalfOpen : 始终允许（用于探测）
     */
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

    /**
     * @brief 记录一次成功调用
     *
     * - Closed   : 重置失败计数
     * - HalfOpen : 累加成功计数，达到阈值则恢复 Closed
     * - Open     : 无操作
     */
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

    /**
     * @brief 记录一次失败调用
     *
     * - Closed   : 累加失败计数，达到阈值则转入 Open
     * - HalfOpen : 立即回退到 Open（探测失败）
     * - Open     : 仅更新最后失败时间
     */
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

    /**
     * @brief 执行受熔断器保护的操作（模板方法）
     * @tparam Func 可调用对象类型
     * @param func 要执行的操作
     * @return 操作返回值；若熔断中或抛出异常则返回 ReturnType 的默认构造值
     *
     * 使用流程:
     * 1. 调用 allow() 检查是否放行
     * 2. 放行则执行 func()，成功时 recordSuccess()
     * 3. func() 抛出异常时 recordFailure() 并返回默认值
     * 4. 熔断中直接返回默认值（快速失败）
     */
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
    /**
     * @brief 获取当前时间戳（毫秒，基于 steady_clock）
     * @return 自 epoch 以来的毫秒数
     */
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    int failureThreshold_;    ///< 触发熔断的连续失败次数阈值
    int successThreshold_;    ///< HalfOpen 恢复到 Closed 所需的连续成功次数
    int64_t timeoutMs_;       ///< Open 状态超时时间（毫秒），超时后转入 HalfOpen
    State state_;             ///< 当前熔断器状态
    int failureCount_;        ///< Closed 状态下的连续失败计数
    int successCount_;        ///< HalfOpen 状态下的连续成功计数
    int64_t lastFailureTime_; ///< 最近一次失败的时间戳（毫秒）
    std::mutex mutex_;        ///< 保护所有状态变量的互斥锁
};
