#include"Channel.h"
#include"EventLoop.h"
#include"logger.h"

#include<sys/epoll.h>
const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI;
const int Channel::kWriteEvent = EPOLLOUT;
Channel::Channel(EventLoop* loop, int fd)
    :loop_(loop), fd_(fd), events_(0), revents_(0), index_(-1), tied_(false)
{
}

Channel::~Channel()
{
}

// tie什么时候调用？
void Channel::tie(const std::shared_ptr<void>& obj)
{
    tie_ = obj;
    tied_ = true;
}

//当改变channel代表的fd的events事件后，update负责在poller里面更改fd对应的时间（epoll_CTL）
//Evevtloop => poller ChannelList
void Channel::update()
{
    //通过poller给channel设置相应的epoll_event
    //add code
    loop_->updateChannel(this);
}
//在channel所属的eventloop中，把当前的channel删除掉
void Channel::remove()
{
    loop_->removeChannel(this);
}

//fd得到poller通知后，处理事件
void Channel::handleEvent(Timestamp receiveTime)
{
    if(tied_)
    {
        std::shared_ptr<void> guard = tie_.lock();
        if(guard)
        {
            handleEventWithGuard(receiveTime);
        }
    }
    else
    {
        handleEventWithGuard(receiveTime);
    }
}

void Channel::handleEventWithGuard(Timestamp receiveTime)
{
    LOG_INFO("Channel::handleEvent fd=%d revents=%d", fd_, revents_);
    if((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN))
    {
        if(closeCallback_) closeCallback_();
    }
    if(revents_ & EPOLLERR)
    {
        if(errorCallback_) errorCallback_();
    }
    if(revents_ & (EPOLLIN | EPOLLPRI))
    {
        if(readCallback_) readCallback_(receiveTime);
    }
    if(revents_ & EPOLLOUT)
    {
        if(writeCallback_) writeCallback_();
    } 
}