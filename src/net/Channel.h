/**
 * @file Channel.h
 * @brief 事件通道类
 *
 * 本文件定义了 Channel 类，它是 Reactor 模式中的关键组件。
 * Channel 封装了文件描述符 (fd) 和该 fd 上感兴趣的事件，
 * 以及事件发生时的回调函数。
 *
 * 设计理念:
 * Channel 类封装了 socketfd 和 socketfd 对应的事件 (EPOLLIN、EPOLLOUT 等)
 * 绑定了 Poller 和回调函数。
 *
 * Reactor 模型中的关系:
 * @code
 *           EventLoop
 *              |
 *         +----+----+
 *         |         |
 *     Channel     Poller
 *    (事件处理器)  (事件多路分发器)
 * @endcode
 *
 * @example 基本使用
 * @code
 * EventLoop loop;
 * int sockfd = ...;  // 创建 socket
 * Channel channel(&loop, sockfd);
 *
 * channel.setReadCallback([](Timestamp) {
 *     // 处理读事件
 * });
 * channel.enableReading();  // 启用读事件监听
 * @endcode
 */

#pragma once

#include "noncopyable.h"
#include <functional>
#include "Timestamp.h"
#include <memory>

// 只需要指针或者引用的时候可以使用前置声明
// 要使用具体的类，则需要引入头文件
class EventLoop;

/**
 * @class Channel
 * @brief 事件通道类，封装 fd 和事件回调
 *
 * Channel 是可读、可写等事件的分发器，它:
 * - 绑定一个文件描述符 (fd)
 * - 记录该 fd 感兴趣的事件 (events_)
 * - 记录 Poller 返回的实际发生的事件 (revents_)
 * - 提供事件处理回调函数
 *
 * 线程安全:
 * Channel 对象只在创建它的 EventLoop 线程中被访问，
 * 因此不需要加锁。
 *
 * @note Channel 不可拷贝 (继承自 noncopyable)
 */
class Channel : noncopyable
{
public:
    /**
     * @brief 事件回调函数类型，无参数无返回值
     *
     * 用于表示通用的事件处理回调函数，例如可写、关闭、错误事件的处理
     */
    using EventCallback = std::function<void()>;

    /**
     * @brief 读事件回调函数类型，包含时间戳参数
     *
     * 用于处理带有时间戳信息的读事件，记录事件触发的具体时间
     */
    using ReadEventCallback = std::function<void(Timestamp)>;

    /**
     * @brief 构造一个新的 Channel 对象
     * @param loop 指向 EventLoop 对象的指针，负责管理该 Channel 的事件循环
     * @param fd 文件描述符，用于监听 I/O 事件
     *
     * @example
     * @code
     * EventLoop loop;
     * int sockfd = socket(AF_INET, SOCK_STREAM, 0);
     * Channel channel(&loop, sockfd);
     * @endcode
     */
    Channel(EventLoop* loop, int fd);

    /**
     * @brief 析构 Channel 对象
     *
     * 注意: 确保在销毁前已停止监听，避免悬空指针问题。
     * 通常需要先调用 disableAll() 和 remove()
     */
    ~Channel();

    /**
     * @brief 处理已发生的事件
     * @param receiveTime 事件触发的时间戳，用于记录事件发生时间
     *
     * 根据 revents_ 调用相应的回调函数:
     * - EPOLLIN: 调用 readCallback_
     * - EPOLLOUT: 调用 writeCallback_
     * - EPOLLHUP: 调用 closeCallback_
     * - EPOLLERR: 调用 errorCallback_
     *
     * 如果设置了 tie_，会先检查对象是否存活再调用回调
     */
    void handleEvent(Timestamp receiveTime);

    /**
     * @brief 设置读事件回调函数
     * @param cb 回调函数
     *
     * @example
     * @code
     * channel.setReadCallback([](Timestamp time) {
     *     // 处理读事件
     * });
     * @endcode
     */
    void setReadCallback(ReadEventCallback cb) { readCallback_ = std::move(cb); }

    /**
     * @brief 设置写事件回调函数
     * @param cb 回调函数
     */
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }

    /**
     * @brief 设置关闭事件回调函数
     * @param cb 回调函数
     */
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }

    /**
     * @brief 设置错误事件回调函数
     * @param cb 回调函数
     */
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    /**
     * @brief 绑定对象生命周期，防止 channel 被手动 remove 后还在执行回调
     * @param obj 共享指针，通常是 TcpConnection
     *
     * tie 的作用:
     * 当 TcpConnection 对象被销毁时，其对应的 Channel 可能还在处理事件。
     * 通过 tie() 绑定 TcpConnection 的 shared_ptr，
     * handleEvent() 会先检查对象是否存活，避免使用悬空指针。
     *
     * @example
     * @code
     * // 在 TcpConnection 中
     * channel_->tie(shared_from_this());
     * @endcode
     */
    void tie(const std::shared_ptr<void>&);

    /**
     * @brief 获取文件描述符
     * @return 文件描述符
     */
    int fd() const { return fd_; }

    /**
     * @brief 获取感兴趣的事件
     * @return 事件位掩码
     */
    int events() const { return events_; }

    /**
     * @brief 设置实际发生的事件 (由 Poller 调用)
     * @param revt 事件位掩码
     */
    void set_revents(int revt) { revents_ = revt; }

    /**
     * @brief 启用读事件监听
     *
     * 设置 EPOLLIN | EPOLLPRI 事件，并更新到 Poller
     */
    void enableReading() { events_ |= kReadEvent; update(); }

    /**
     * @brief 禁用读事件监听
     *
     * 清除 EPOLLIN | EPOLLPRI 事件，并更新到 Poller
     */
    void disableReading() { events_ &= ~kReadEvent; update(); }

    /**
     * @brief 启用写事件监听
     *
     * 设置 EPOLLOUT 事件，并更新到 Poller
     */
    void enableWriting() { events_ |= kWriteEvent; update(); }

    /**
     * @brief 禁用写事件监听
     *
     * 清除 EPOLLOUT 事件，并更新到 Poller
     */
    void disableWriting() { events_ &= ~kWriteEvent; update(); }

    /**
     * @brief 禁用所有事件监听
     *
     * 清除所有事件，并更新到 Poller
     */
    void disableAll() { events_ = kNoneEvent; update(); }

    /**
     * @brief 检查是否没有监听任何事件
     * @return true 如果没有监听任何事件
     */
    bool isNoneEvent() const { return events_ == kNoneEvent; }

    /**
     * @brief 检查是否监听写事件
     * @return true 如果监听写事件
     */
    bool isWriting() const { return events_ & kWriteEvent; }

    /**
     * @brief 检查是否监听读事件
     * @return true 如果监听读事件
     */
    bool isReading() const { return events_ & kReadEvent; }

    /**
     * @brief 获取 Channel 在 Poller 中的索引
     * @return 索引值
     *
     * 用于 Poller 内部管理 Channel
     */
    int index() { return index_; }

    /**
     * @brief 设置 Channel 在 Poller 中的索引
     * @param idx 索引值
     */
    void set_index(int idx) { index_ = idx; }

    /**
     * @brief 获取所属的 EventLoop
     * @return EventLoop 指针
     */
    EventLoop* ownerLoop() { return loop_; }

    /**
     * @brief 从 EventLoop 中移除当前 Channel
     *
     * 调用 EventLoop::removeChannel() 从 Poller 中移除
     */
    void remove();

private:
    /**
     * @brief 更新 Channel 在 Poller 中的注册
     *
     * 当改变 channel 代表的 fd 的 events 事件后，
     * update 负责在 poller 里面更改 fd 对应的事件 (epoll_ctl)
     */
    void update();

    /**
     * @brief 实际处理事件 (带保护)
     * @param receiveTime 事件触发时间
     *
     * 根据 revents_ 调用相应的回调函数
     */
    void handleEventWithGuard(Timestamp receiveTime);

    /// 无事件
    static const int kNoneEvent;
    /// 读事件 (EPOLLIN | EPOLLPRI)
    static const int kReadEvent;
    /// 写事件 (EPOLLOUT)
    static const int kWriteEvent;

    EventLoop *loop_;   ///< 事件循环
    const int fd_;      ///< fd，Poller 监听的对象
    int events_;        ///< 注册 fd 感兴趣的事件
    int revents_;       ///< poller 返回的具体发生的事件
    int index_;         ///< 在 Poller 中的索引 (用于 EPOLL_CTL_ADD/MOD/DEL)

    std::weak_ptr<void> tie_;  ///< 绑定的对象 (弱引用)
    bool tied_;                 ///< 是否设置了 tie

    /// 回调操作
    /// 因为 Channel 能够监听事件，知道最终发生的事件 revents，
    /// 所以 Channel 可以执行相应的回调
    ReadEventCallback readCallback_;   ///< 读事件回调
    EventCallback writeCallback_;      ///< 写事件回调
    EventCallback errorCallback_;      ///< 错误事件回调
    EventCallback closeCallback_;      ///< 关闭事件回调
};