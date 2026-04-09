#include"EventLoopThreadPool.h"
#include"EventLoopThread.h"

#include<memory>
EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, const std::string& nameArg)
    : baseLoop_(baseLoop)
    , name_(nameArg)
    , started_(false)
    , numThreads_(0)
    , next_(0)
{
    if(name_.empty())
    {
        char buf[32];
        snprintf(buf, sizeof buf, "%d", getpid());
        name_ = "EventLoopThreadPoll-";
    }
}
EventLoopThreadPool::~EventLoopThreadPool(){};

void EventLoopThreadPool::start(const ThreadInitCallback& cb)
{ 
    started_ = true;
    for(int i = 0; i < numThreads_; ++i)
    {
        char buf[name_.size() + 32];
        snprintf(buf, sizeof buf, "%s%d", name_.c_str(), i);
        std::unique_ptr<EventLoopThread> t(new EventLoopThread(cb, buf));
        loops_.push_back(t->startLoop());   // 底层创建线程，绑定一个新的EventLoop，并返回该EventLoop的地址
        threads_.push_back(std::move(t));
    }
    if(numThreads_ == 0 && cb)
    {
        cb(baseLoop_);
    }

}

EventLoop* EventLoopThreadPool::getNextLoop()
{ 
    EventLoop* loop = baseLoop_;
    if(!loops_.empty()) //说明有loop，轮询获取
    {
        loop = loops_[next_];
        ++next_;
        if(next_ >= loops_.size())
        {
            next_ = 0;
        }
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops()
{
    if(numThreads_ == 0)
    {
        return std::vector<EventLoop*>(1, baseLoop_);
    }
    else
    {
        return loops_;
    }
}
