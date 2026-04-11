#pragma once
#include "Poller.h"
#include<sys/epoll.h>

class Channel;
/**
 * EPollPoller类：基于epoll的I/O多路复用事件收集器，继承自Poller基类
 * 实现Linux平台下高效的事件监听与分发
 */
class EPollPoller : public Poller 
{ 
public:
    /**
     * 构造函数：初始化epoll实例
     * @param loop 关联的事件循环对象
     */
    explicit EPollPoller(EventLoop* loop);

    /**
     * 析构函数：释放epoll文件描述符及所有关联资源
     */
    virtual ~EPollPoller() override;

    /**
     * 等待并收集发生的事件
     * @param timeoutMs 超时时间（毫秒），-1表示无限等待
     * @param activeChannels 输出参数，存储检测到事件的通道列表
     * @return 返回最近发生事件的时间戳
     */
    virtual Timestamp poll(int timeoutMs, ChannelList* activeChannels) override;

    /**
     * 从监听集合中移除指定通道
     * @param channel 待移除的通道指针
     * @note 移除操作会触发EPOLL_CTL_DEL并清理内部映射
     */
    virtual void removeChannel(Channel* channel) override;

    /**
     * 更新通道的事件监听状态
     * @param channel 需要更新的通道指针
     * @note 根据通道状态决定执行ADD/MOD/DEL操作，可能引发异常
     */
    virtual void updateChannel(Channel* channel) override;

private:
    // 初始事件列表容量常量，用于优化内存分配
    static const int kInitEventListSize = 128;

    /**
     * 填充活动通道列表
     * @param numEvents 已发生的事件数量
     * @param activeChannels 目标填充的活动通道列表
     * @note 将events_数组中的就绪事件转换为对应的Channel对象
     */
    void fillActiveChannels(int numEvents, ChannelList* activeChannels) const;

    /**
     * 执行epoll_ctl操作
     * @param operation epoll_ctl的操作类型（ADD/MOD/DEL）
     * @param channel 关联的通道对象
     * @note 该函数会直接操作内核epoll实例
     */
    void update(int operation, Channel* channel);
    // epoll专用文件描述符，用于标识内核事件表
    int epollfd_;

    
    // epoll事件数组类型别名，使用vector管理连续内存
    using EventList = std::vector<epoll_event>;
    // 事件数组，存储就绪事件，初始容量由kInitEventListSize指定
    EventList events_;
};