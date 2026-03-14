/**
 * @file EventLoop.h
 * @brief 事件循环核心类
 *
 * 本文件定义了 EventLoop 类，是 muduo 网络库的核心组件。
 * EventLoop 实现了 Reactor 模式中的事件循环，负责:
 * - 监听和分发 I/O 事件
 * - 执行定时任务
 * - 跨线程任务调度
 *
 * 关键设计:
 * - One Loop Per Thread: 每个 EventLoop 绑定到一个线程
 * - 线程安全: 通过 wakeup 机制实现跨线程调用
 *
 * @example 基本使用
 * @code
 * EventLoop loop;
 * loop.loop();  // 阻塞，开始事件循环
 * @endcode
 *
 * @example 跨线程调用
 * @code
 * // 在其他线程中安全地执行任务
 * loop->runInLoop([]() {
 *     // 这个任务会在 loop 所在的线程中执行
 * });
 * @endcode
 */

#pragma once

#include <functional>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>

#include "noncopyable.h"
#include "Timestamp.h"
#include "CurrentThread.h"

class Channel;
class Poller;

/**
 * @class EventLoop
 * @brief 事件循环类，主要包含了两个大模块: Channel 和 Poller (epoll 的抽象)
 *
 * EventLoop 是 Reactor 模式的核心，实现了事件循环机制:
 * - Poller: 负责监听文件描述符上的 I/O 事件 (使用 epoll)
 * - Channel: 封装文件描述符和对应的事件回调
 *
 * 线程模型:
 * - 每个 EventLoop 对象只能在其创建的线程中使用
 * - 通过 isInLoopThread() 判断当前是否在 EventLoop 线程中
 * - 通过 runInLoop() 实现跨线程安全调用
 *
 * @note EventLoop 不可拷贝 (继承自 noncopyable)
 */
class EventLoop : noncopyable
{
public:
    /// 回调函数类型
    using Functor = std::function<void()>;

    /**
     * @brief 构造函数
     *
     * 初始化流程:
     * 1. 记录当前线程 ID
     * 2. 创建 Poller 对象
     * 3. 创建 wakeup fd 和 wakeup Channel
     * 4. 确保 One Loop Per Thread
     *
     * @note 如果当前线程已存在 EventLoop，会触发断言失败
     */
    EventLoop();

    /**
     * @brief 析构函数
     *
     * 清理流程:
     * 1. 禁用 wakeup Channel 的所有事件
     * 2. 移除 wakeup Channel
     * 3. 关闭 wakeup fd
     * 4. 清除线程局部变量
     */
    ~EventLoop();

    /**
     * @brief 开启事件循环
     *
     * 事件循环流程:
     * 1. 调用 Poller::poll() 等待事件
     * 2. 遍历活跃的 Channel，调用 handleEvent()
     * 3. 执行待处理的回调函数
     *
     * @note 此方法会阻塞，直到调用 quit()
     */
    void loop();

    /**
     * @brief 退出事件循环
     *
     * 设置 quit_ 标志，事件循环会在下一次迭代时退出。
     * 如果在非 EventLoop 线程中调用，会先唤醒 EventLoop 线程。
     *
     * @note 线程安全
     */
    void quit();

    /**
     * @brief 获取 Poller 返回的时间
     * @return Poller 返回发生事件的 channels 的时间点
     */
    Timestamp pollReturnTime() const { return pollReturnTime_; }

    /**
     * @brief 在当前 loop 中执行回调
     * @param cb 要执行的回调函数
     *
     * 如果当前在 EventLoop 线程中，直接执行 cb
     * 否则，将 cb 放入队列，并唤醒 EventLoop 线程
     *
     * @note 线程安全
     */
    void runInLoop(Functor cb);

    /**
     * @brief 把 cb 放入队列中，唤醒 loop 所在的线程，执行 cb
     * @param cb 要执行的回调函数
     *
     * 将回调函数加入待处理队列，并唤醒 EventLoop 线程。
     * 适用于不需要立即执行的场景。
     *
     * @note 线程安全
     */
    void queueInLoop(Functor cb);

    /**
     * @brief 唤醒 loop 所在的线程
     *
     * 通过向 wakeup fd 写入数据来唤醒 EventLoop 线程。
     * 这会使 Poller 从 poll() 中返回。
     *
     * @note 内部使用，用户通常不需要直接调用
     */
    void wakeup();

    /**
     * @brief 更新 Channel 在 Poller 中的注册
     * @param channel 要更新的 Channel
     *
     * 委托给 Poller::updateChannel()
     */
    void updateChannel(Channel *channel);

    /**
     * @brief 从 Poller 中移除 Channel
     * @param channel 要移除的 Channel
     *
     * 委托给 Poller::removeChannel()
     */
    void removeChannel(Channel *channel);

    /**
     * @brief 检查 Channel 是否在 Poller 中
     * @param channel 要检查的 Channel
     * @return 是否存在
     */
    bool hasChannel(Channel *channel);

    /**
     * @brief 判断 EventLoop 对象是否在自己的线程里面
     * @return 是否在创建线程中
     *
     * 通过比较当前线程 ID 和创建时的线程 ID 来判断
     */
    bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); }

private:
    /**
     * @brief 处理 wakeup 事件
     *
     * 当 wakeup fd 可读时调用，读取并丢弃数据。
     * 这是为了清除 wakeup 事件，防止重复触发。
     */
    void handleRead();

    /**
     * @brief 执行待处理的回调函数
     *
     * 遍历 pendingFunctors_ 并执行所有回调。
     * 使用 swap 技巧减少临界区时间。
     */
    void doPendingFunctors();

    /// Channel 列表类型
    using ChannelList = std::vector<Channel*>;

    std::atomic_bool looping_;  ///< 是否在事件循环中 (原子操作，通过 CAS 实现)
    std::atomic_bool quit_;     ///< 标识退出 loop 循环

    const pid_t threadId_;      ///< 记录当前 loop 所在线程的 id

    Timestamp pollReturnTime_;  ///< poller 返回发生事件的 channels 的时间点
    std::unique_ptr<Poller> poller_;  ///< Poller 对象 (epoll 的封装)

    int wakeupFd_;              ///< 主要作用: 当 mainLoop 获取一个新用户的 channel，
                                ///< 通过轮询算法选择一个 subloop，通过该成员唤醒 subloop 处理 channel
    std::unique_ptr<Channel> wakeupChannel_;  ///< wakeup fd 对应的 Channel

    ChannelList activeChannels_;  ///< Poller 返回的活跃 Channel 列表

    std::atomic_bool callingPendingFunctors_;  ///< 标识当前 loop 是否有需要执行的回调操作
    std::vector<Functor> pendingFunctors_;     ///< 存储 loop 需要执行的所有的回调操作
    std::mutex mutex_;  ///< 互斥锁，用来保护上面 vector 容器的线程安全操作
};