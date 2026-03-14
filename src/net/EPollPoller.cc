#include "EPollPoller.h"
#include "logger.h"
#include "Channel.h"

#include <errno.h>
#include <unistd.h>
#include <strings.h>  

//channel未添加到poller中
const int kNew = -1;
//channel已添加到poller中
const int kAdded = 1;
//channel从poller中删除
const int kDeleted = 2;

/**
 * @brief 构造EPollPoller对象，初始化epoll实例并设置事件监听集合
 * 
 * @param loop 关联的事件循环对象指针，用于驱动IO事件处理
 * 
 * @note 初始化流程：
 * 1. 调用基类Poller构造函数
 * 2. 创建带有EPOLL_CLOEXEC标志的epoll文件描述符
 * 3. 初始化事件数组容量为kInitEventListSize（默认16）
 * 4. 失败时记录致命日志并终止程序
 */
EPollPoller::EPollPoller(EventLoop* loop)
    :Poller(loop),
    epollfd_(::epoll_create1(EPOLL_CLOEXEC)),  // 创建epoll实例，EPOLL_CLOEXEC标志保证exec时自动关闭fd
    events_(kInitEventListSize)  // 预分配事件数组空间
{
    if(epollfd_ < 0)
    {
        LOG_FATAL("epoll_create error:%d\n", errno);  // 创建失败时记录错误信息并触发致命日志
    }
}

EPollPoller::~EPollPoller()
{
    ::close(epollfd_);
}

/**
 * @brief 等待并处理IO事件
 * 
 * 该函数使用epoll_wait等待IO事件的发生，并将活跃的通道填充到activeChannels列表中。
 * 如果事件数量达到events_容器的容量，则会自动扩容。
 * 
 * @param timeoutMs 等待超时时间，单位为毫秒
 * @param activeChannels 用于存储活跃通道的列表指针
 * @return Timestamp 返回当前时间戳
 */
Timestamp EPollPoller::poll(int timeoutMs, ChannelList* activeChannels)
{
    LOG_DEBUG("func=%s => pollerfd=%d\n", __FUNCTION__, epollfd_);
    Timestamp now(Timestamp::now());
    int saveErrno = errno;
    
    // 调用epoll_wait等待IO事件
    int numEvents = ::epoll_wait(epollfd_, &*events_.begin(), static_cast<int>(events_.size()), timeoutMs);
    if(numEvents > 0)
    {
        LOG_INFO("%d events happened\n", numEvents);
        // 处理活跃通道
        fillActiveChannels(numEvents, activeChannels);
        // 如果事件数量等于当前容量，则扩容events_容器
        if(static_cast<size_t>(numEvents) == events_.size())
        {
            events_.resize(events_.size() * 2);
        }
    }
    else if(numEvents == 0)
    {
        LOG_DEBUG("%s timeout!\n", __FUNCTION__);
    }
    else
    {
        // 处理epoll_wait错误情况
        if(errno != EINTR)
        {
            errno = saveErrno;
            LOG_ERROR("epoll_wait error:%d\n", errno);
        }
    }
    return now;     
}

/**
 * updateChannel()
 * 
 * 该函数负责将Channel的状态同步到epoll实例中：
 * - 当Channel为新创建(kNew)或已删除(kDeleted)状态时，将其添加到epoll监听
 * - 当Channel已存在且事件发生改变时，更新其在epoll中的注册状态
 * - 当Channel不再关注任何事件时，从epoll中移除
 */
void EPollPoller::updateChannel(Channel* channel)
{
    const int index = channel->index();
    LOG_INFO("func=%s => fd=%d events=%d index=%d \n", __FUNCTION__, channel->fd(), channel->events(), index);

    /**
     * 处理新Channel或已删除的Channel
     * kNew: 首次添加的Channel
     * kDeleted: 之前被移除但仍在ChannelMap中的Channel
     */
    if(index == kNew || index == kDeleted)
    {
        /**
         * 对于新Channel，需要先将其加入ChannelMap管理
         * 通过fd作为key建立映射关系，便于后续事件分发
         */
        if(index == kNew)
        {
            int fd = channel->fd();
            channels_[fd] = channel;
        }

        /**
         * 统一设置为已添加状态，并执行epoll注册操作
         * EPOLL_CTL_ADD: 将fd添加到epoll实例的监听列表
         */
        channel->set_index(kAdded);
        update(EPOLL_CTL_ADD, channel);
    }
    else
    {
        /**
         * 处理已存在的Channel状态更新
         * EPOLL_CTL_MOD: 修改已注册fd的监听事件
         * EPOLL_CTL_DEL: 从epoll实例中移除fd
         */
        // int fd = channel->fd();
        if(channel->isNoneEvent())
        {
            /**
             * 当Channel不再关注任何事件时：
             * 1. 从epoll实例中删除该fd
             * 2. 将Channel状态标记为kDeleted
             * 保留Channel对象在ChannelMap中供后续复用
             */
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(kDeleted);
        }
        else
        {
            /**
             * 当Channel的监听事件发生变化时：
             * 通过EPOLL_CTL_MOD更新epoll实例中的事件注册
             */
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

/**
 * @brief 从epoll监听集合中移除指定的Channel
 * @param channel 要移除的Channel对象指针
 * 
 * 该函数负责将指定的Channel从epoll监听集合中移除，并更新Channel的状态。
 * 主要操作包括：从channels_映射表中删除该Channel、如果Channel已添加到epoll中
 * 则调用epoll_ctl删除操作、最后将Channel状态设置为新创建状态。
 */
void EPollPoller::removeChannel(Channel* channel)
{
    int fd = channel->fd();
    // 从channels_映射表中移除该文件描述符对应的Channel
    channels_.erase(fd);
    int index = channel->index();
    LOG_INFO("func=%s => fd=%d\n",__FUNCTION__ ,fd);
    
    // 如果该Channel已经添加到epoll中，则需要从epoll中删除
    if(index == kAdded)
    {
        update(EPOLL_CTL_DEL, channel);
    }
    
    // 将Channel状态设置为新创建状态
    channel->set_index(kNew);
}

/**
 * @brief 填充活跃的Channel列表
 * 
 * 将epoll_wait返回的事件数组转换为对应的Channel对象集合，
 * 并记录事件类型到Channel对象中
 * 
 * @param numEvents epoll_wait返回的事件数量
 * @param activeChannels 用于存储活跃Channel的输出参数
 * 
 * @return void 无返回值
 */
void EPollPoller::fillActiveChannels(int numEvents, ChannelList* activeChannels) const
{
    /**
     * 遍历所有epoll事件：
     * 1. 从事件结构体中提取关联的Channel对象
     * 2. 记录发生的事件类型到Channel的revents_成员
     * 3. 将该Channel添加到活跃通道列表中
     */
    for(int i = 0; i < numEvents; i++)
    {
        Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        activeChannels->push_back(channel);
    }
}

/*
 * 函数功能：更新epoll实例中的channel事件注册状态
 * 参数说明：
 *   operation - 操作类型（EPOLL_CTL_ADD/EPOLL_CTL_MOD/EPOLL_CTL_DEL）
 *   channel   - 需要操作的Channel对象指针
 * 返回值：void
 */
void EPollPoller::update(int operation, Channel* channel)
{
    epoll_event event;

    /*
     * 初始化epoll_event结构体：
     * 1. 清空结构体内容
     * 2. 设置事件类型和关联数据
     */
    int fd = channel->fd();
    bzero(&event, sizeof(event));
    event.events = channel->events();
    event.data.fd = fd;
    event.data.ptr = channel;
    
    /*
     * 执行epoll_ctl控制操作：
     * - 添加/修改/删除文件描述符的事件监控
     * - 出错时根据操作类型记录不同日志级别：
     *   - 删除操作失败记录ERROR级别日志
     *   - 其他操作失败记录FATAL级别日志
     */
    if(::epoll_ctl(epollfd_, operation, fd, &event) < 0)
    {
        if(operation == EPOLL_CTL_DEL)
        {
            LOG_ERROR("epoll_ctl del error:%d\n", errno);
        }
        else
        {
            LOG_FATAL("epoll_ctl add/mod error:%d\n", errno);
        }
    }
}