#pragma once

#include "noncopyable.h"
#include "Timestamp.h"

#include <vector>
#include <unordered_map>

class Channel;
class EventLoop;

class Poller : noncopyable {
public:
    using ChannelList = std::vector<Channel*>;
    Poller(EventLoop* loop);
    virtual ~Poller();
    //给所有IO复用保留统一接口
    virtual Timestamp poll(int timeoutMs, ChannelList* activeChannels) = 0;
    virtual void updateChannel(Channel* channel) = 0;
    virtual void removeChannel(Channel* channel) = 0;
    bool hasChannel(Channel* channel) const;

    //EventLoop可以通过这个接口，创建一个默认的Poller
    static Poller* newDefaultPoller(EventLoop* loop);
protected:
    //key:sockfd, value:sockfd对应的Channel
    using ChannelMap = std::unordered_map<int, Channel*>;
    ChannelMap channels_;
private:
    //定义poller所属的loop
    EventLoop* ownerLoop_;
};