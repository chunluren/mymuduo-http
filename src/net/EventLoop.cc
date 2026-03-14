/**
 * @file EventLoop.cc
 * @brief 事件循环实现
 *
 * 本文件实现了 EventLoop 类的所有方法，包括:
 * - 事件循环的核心逻辑
 * - wakeup 机制
 * - 跨线程任务调度
 */

#include "EventLoop.h"
#include "logger.h"
#include "Poller.h"
#include "Channel.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <memory>

/// 线程局部变量，用于实现 One Loop Per Thread
/// 每个线程最多只能有一个 EventLoop
__thread EventLoop *t_loopInThisThread = nullptr;

/// 定义默认的 Poller IO 复用接口的超时时间 (毫秒)
const int kPollTimeMs = 10;

/**
 * @brief 创建 wakeup fd
 * @return 新创建的 eventfd
 *
 * eventfd 是 Linux 特有的系统调用，用于线程间通知。
 * 相比 pipe，它更轻量，只需要一个文件描述符。
 *
 * @note 使用 EFD_NONBLOCK (非阻塞) 和 EFD_CLOEXEC (exec 时关闭)
 */
int createEventfd()
{
    // 创建 eventfd，非阻塞模式，exec 时自动关闭
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0)
    {
        LOG_FATAL("eventfd error:%d \n", errno);
    }
    return evtfd;
}

/**
 * @brief EventLoop 构造函数实现
 *
 * 初始化流程:
 * 1. 设置 looping_ = false, quit_ = false
 * 2. 记录当前线程 ID
 * 3. 创建默认 Poller (epoll)
 * 4. 创建 wakeup fd 和 wakeup Channel
 * 5. 检查 One Loop Per Thread 约束
 */
EventLoop::EventLoop()
    : looping_(false)
    , quit_(false)
    , threadId_(CurrentThread::tid())
    , poller_(Poller::newDefaultPoller(this))
    , wakeupFd_(createEventfd())
    , wakeupChannel_(new Channel(this, wakeupFd_))
    , callingPendingFunctors_(false)
{
    LOG_DEBUG("EventLoop created %p in thread %d \n", this, threadId_);

    // 检查当前线程是否已经有 EventLoop
    // 实现 One Loop Per Thread 约束
    if (t_loopInThisThread)
    {
        // 当前线程已存在 EventLoop，这是严重错误
        LOG_FATAL("Another EventLoop %p exists in this thread %d \n", t_loopInThisThread, threadId_);
    }
    else
    {
        // 将当前 EventLoop 保存到线程局部变量
        t_loopInThisThread = this;
    }

    // 设置 wakeup Channel 的读事件回调
    // 当 wakeup fd 可读时，调用 handleRead
    wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleRead, this));

    // 启用 wakeup Channel 的读事件监听
    // 这样当其他线程写入 wakeup fd 时，当前 EventLoop 会被唤醒
    wakeupChannel_->enableReading();
}

/**
 * @brief EventLoop 析构函数实现
 *
 * 清理流程:
 * 1. 禁用 wakeup Channel 的所有事件
 * 2. 从 Poller 中移除 wakeup Channel
 * 3. 关闭 wakeup fd
 * 4. 清除线程局部变量
 */
EventLoop::~EventLoop()
{
    wakeupChannel_->disableAll();    // 禁用所有事件
    wakeupChannel_->remove();         // 从 Poller 移除
    ::close(wakeupFd_);              // 关闭 fd
    t_loopInThisThread = nullptr;    // 清除线程局部变量
}

/**
 * @brief 开启事件循环
 *
 * 事件循环的核心逻辑:
 * 1. 设置 looping_ = true
 * 2. 循环执行:
 *    a. 清空活跃 Channel 列表
 *    b. 调用 Poller::poll() 等待事件 (超时 kPollTimeMs)
 *    c. 处理所有活跃 Channel 的事件
 *    d. 执行待处理的回调函数
 * 3. 直到 quit_ 为 true 才退出
 *
 * @note Poller 监听两类 fd:
 * - client 的 fd: 客户端连接
 * - wakeup fd: 用于唤醒 EventLoop
 */
void EventLoop::loop()
{
    looping_ = true;
    quit_ = false;

    LOG_INFO("EventLoop %p start looping \n", this);

    while (!quit_)
    {
        activeChannels_.clear();

        // 调用 Poller 获取活跃的 Channel
        // Poller 会监听 wakeup fd 和所有注册的 client fd
        pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);

        // 处理所有活跃 Channel 的事件
        for (Channel *channel : activeChannels_)
        {
            // Poller 监听到哪些 channel 发生事件了
            // 然后上报给 EventLoop，通知 channel 处理相应的事件
            channel->handleEvent(pollReturnTime_);
        }

        // 执行当前 EventLoop 事件循环需要处理的回调操作
        /**
         * IO 线程模型示例:
         *
         *              mainLoop (accept 线程)
         *                   |
         *                   v
         *              新连接 fd -> channel
         *                   |
         *                   v
         *              轮询选择 subloop
         *                   |
         *                   v
         *    mainLoop 注册回调 cb 到 subloop
         *                   |
         *                   v
         *    wakeup subloop -> 执行 doPendingFunctors -> 执行 cb
         *
         * 这里实现了 mainLoop 到 subloop 的跨线程任务调度
         */
        doPendingFunctors();
    }

    LOG_INFO("EventLoop %p stop looping. \n", this);
    looping_ = false;
}

/**
 * @brief 退出事件循环
 *
 * 退出事件循环有两种情况:
 * 1. 在 EventLoop 自己的线程中调用 quit():
 *    - 直接设置 quit_ = true
 *    - 下次循环迭代时退出
 *
 * 2. 在非 EventLoop 线程中调用 quit():
 *    - 设置 quit_ = true
 *    - 调用 wakeup() 唤醒 EventLoop 线程
 *    - 确保事件循环能够及时退出
 *
 * @example 典型场景
 * @code
 *              mainLoop
 *                  |
 *                  | quit()
 *                  v
 *              wakeup
 *                  |
 *    subLoop1   subLoop2   subLoop3
 *         |         |          |
 *         v         v          v
 *       退出      退出        退出
 * @endcode
 */
void EventLoop::quit()
{
    quit_ = true;

    // 如果是在其它线程中调用的 quit
    // 例如: 在一个 subloop (worker) 中，调用了 mainLoop (IO) 的 quit
    if (!isInLoopThread())
    {
        // 需要唤醒 EventLoop，让它检查 quit_ 标志并退出
        wakeup();
    }
}

/**
 * @brief 在当前 loop 中执行 cb
 * @param cb 要执行的回调函数
 *
 * 如果当前在 EventLoop 线程中，直接执行回调
 * 否则，调用 queueInLoop() 将回调加入队列
 */
void EventLoop::runInLoop(Functor cb)
{
    if (isInLoopThread())
    {
        // 在当前的 loop 线程中，直接执行 cb
        cb();
    }
    else
    {
        // 在非当前 loop 线程中执行 cb
        // 需要唤醒 loop 所在线程，执行 cb
        queueInLoop(cb);
    }
}

/**
 * @brief 把 cb 放入队列中，唤醒 loop 所在的线程，执行 cb
 * @param cb 要执行的回调函数
 *
 * 实现步骤:
 * 1. 加锁，将 cb 加入待处理队列
 * 2. 判断是否需要唤醒 EventLoop 线程:
 *    - 不在 EventLoop 线程中: 需要唤醒
 *    - 正在执行回调但有新回调: 需要唤醒 (防止回调被延迟)
 */
void EventLoop::queueInLoop(Functor cb)
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(cb);
    }

    // 唤醒相应的，需要执行上面回调操作的 loop 的线程了
    // || callingPendingFunctors_ 的意思是:
    // 当前 loop 正在执行回调，但是 loop 又有了新的回调
    // 这时也需要唤醒，否则新回调可能要等到下一次 poll 超时才能执行
    if (!isInLoopThread() || callingPendingFunctors_)
    {
        wakeup();  // 唤醒 loop 所在线程
    }
}

/**
 * @brief 处理 wakeup 事件
 *
 * 当 wakeup fd 可读时调用。
 * 读取并丢弃 8 字节数据，以清除事件。
 */
void EventLoop::handleRead()
{
    uint64_t one = 1;
    ssize_t n = read(wakeupFd_, &one, sizeof one);
    if (n != sizeof one)
    {
        LOG_ERROR("EventLoop::handleRead() reads %lu bytes instead of 8", n);
    }
}

/**
 * @brief 用来唤醒 loop 所在的线程
 *
 * 向 wakeup fd 写一个数据，wakeupChannel 就发生读事件，
 * 当前 loop 线程就会被唤醒，从 poll() 返回。
 */
void EventLoop::wakeup()
{
    uint64_t one = 1;
    ssize_t n = write(wakeupFd_, &one, sizeof one);
    if (n != sizeof one)
    {
        LOG_ERROR("EventLoop::wakeup() writes %lu bytes instead of 8 \n", n);
    }
}

/**
 * @brief 更新 Channel 在 Poller 中的注册
 * @param channel 要更新的 Channel
 *
 * 委托给 Poller::updateChannel()
 */
void EventLoop::updateChannel(Channel *channel)
{
    poller_->updateChannel(channel);
}

/**
 * @brief 从 Poller 中移除 Channel
 * @param channel 要移除的 Channel
 *
 * 委托给 Poller::removeChannel()
 */
void EventLoop::removeChannel(Channel *channel)
{
    poller_->removeChannel(channel);
}

/**
 * @brief 检查 Channel 是否在 Poller 中
 * @param channel 要检查的 Channel
 * @return 是否存在
 */
bool EventLoop::hasChannel(Channel *channel)
{
    return poller_->hasChannel(channel);
}

/**
 * @brief 执行待处理的回调函数
 *
 * 核心技巧: 使用 swap 减少临界区时间
 *
 * 执行步骤:
 * 1. 设置 callingPendingFunctors_ = true
 * 2. 加锁，交换 pendingFunctors_ 和本地 functors
 * 3. 解锁
 * 4. 遍历执行 functors 中的所有回调
 * 5. 设置 callingPendingFunctors_ = false
 *
 * swap 的好处:
 * - 减少临界区时间，只保护 swap 操作
 * - 其他线程可以继续往 pendingFunctors_ 添加回调
 * - 避免死锁 (回调中可能调用 queueInLoop)
 */
void EventLoop::doPendingFunctors()
{
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 关键: 使用 swap 而不是 copy
        // 这样 pendingFunctors_ 变为空，其他线程可以继续添加回调
        functors.swap(pendingFunctors_);
    }

    // 执行所有待处理的回调
    for (const Functor &functor : functors)
    {
        functor();  // 执行当前 loop 需要执行的回调操作
    }

    callingPendingFunctors_ = false;
}