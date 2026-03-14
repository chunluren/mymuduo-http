/**
 * @file Channel.cc
 * @brief 事件通道实现
 *
 * 本文件实现了 Channel 类的所有方法，包括:
 * - 事件的启用/禁用
 * - 事件的处理和分发
 * - tie 机制
 */

#include "Channel.h"
#include "EventLoop.h"
#include "logger.h"

#include <sys/epoll.h>

/// 无事件
const int Channel::kNoneEvent = 0;
/// 读事件: 普通数据可读 + 带外数据可读
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI;
/// 写事件
const int Channel::kWriteEvent = EPOLLOUT;

/**
 * @brief Channel 构造函数实现
 * @param loop 所属的 EventLoop
 * @param fd 文件描述符
 */
Channel::Channel(EventLoop* loop, int fd)
    : loop_(loop)
    , fd_(fd)
    , events_(0)
    , revents_(0)
    , index_(-1)  // -1 表示未添加到 Poller
    , tied_(false)
{
}

/**
 * @brief Channel 析构函数
 */
Channel::~Channel()
{
}

/**
 * @brief 绑定对象生命周期
 * @param obj 共享指针
 *
 * tie 在什么时候调用？
 * 在 TcpConnection::connectEstablished() 中调用，
 * 用于绑定 TcpConnection 对象的生命周期。
 *
 * 这样当 Channel 处理事件时，会先检查 TcpConnection 是否存活，
 * 避免使用悬空指针。
 */
void Channel::tie(const std::shared_ptr<void>& obj)
{
    tie_ = obj;
    tied_ = true;
}

/**
 * @brief 更新 Channel 在 Poller 中的注册
 *
 * 当改变 channel 代表的 fd 的 events 事件后，
 * update 负责在 poller 里面更改 fd 对应的事件 (epoll_CTL)
 *
 * 调用链: Channel::update() -> EventLoop::updateChannel() -> Poller::updateChannel()
 */
void Channel::update()
{
    // 通过 poller 给 channel 设置相应的 epoll_event
    loop_->updateChannel(this);
}

/**
 * @brief 从 EventLoop 中移除当前 Channel
 *
 * 在 channel 所属的 eventloop 中，把当前的 channel 删除掉
 */
void Channel::remove()
{
    loop_->removeChannel(this);
}

/**
 * @brief 处理已发生的事件
 * @param receiveTime 事件触发时间
 *
 * 如果设置了 tie_，会先检查绑定的对象是否存活。
 * 只有当对象存活时，才调用事件处理函数。
 *
 * 这是为了防止: channel 被手动 remove 掉后，channel 还在执行回调操作
 */
void Channel::handleEvent(Timestamp receiveTime)
{
    if (tied_)
    {
        // 检查绑定的对象是否存活
        std::shared_ptr<void> guard = tie_.lock();
        if (guard)
        {
            // 对象存活，处理事件
            handleEventWithGuard(receiveTime);
        }
        // 对象已销毁，不处理事件
    }
    else
    {
        // 没有绑定对象，直接处理事件
        handleEventWithGuard(receiveTime);
    }
}

/**
 * @brief 实际处理事件
 * @param receiveTime 事件触发时间
 *
 * 根据实际发生的事件 (revents_)，调用相应的回调函数。
 *
 * 事件处理顺序:
 * 1. EPOLLHUP 且没有 EPOLLIN: 调用关闭回调
 * 2. EPOLLERR: 调用错误回调
 * 3. EPOLLIN | EPOLLPRI: 调用读回调
 * 4. EPOLLOUT: 调用写回调
 *
 * @note EPOLLHUP 表示挂起，通常表示对端关闭连接
 * @note EPOLLPRI 表示带外数据可读
 */
void Channel::handleEventWithGuard(Timestamp receiveTime)
{
    LOG_INFO("Channel::handleEvent fd=%d revents=%d", fd_, revents_);

    // 1. 处理 EPOLLHUP 事件 (挂起/对端关闭)
    // 如果 EPOLLHUP 且没有 EPOLLIN，说明连接被关闭
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN))
    {
        if (closeCallback_) closeCallback_();
    }

    // 2. 处理 EPOLLERR 事件 (错误)
    if (revents_ & EPOLLERR)
    {
        if (errorCallback_) errorCallback_();
    }

    // 3. 处理 EPOLLIN | EPOLLPRI 事件 (可读)
    // EPOLLIN: 普通数据可读
    // EPOLLPRI: 带外数据可读
    if (revents_ & (EPOLLIN | EPOLLPRI))
    {
        if (readCallback_) readCallback_(receiveTime);
    }

    // 4. 处理 EPOLLOUT 事件 (可写)
    if (revents_ & EPOLLOUT)
    {
        if (writeCallback_) writeCallback_();
    }
}