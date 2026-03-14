#include "EventLoopThread.h"
#include "CurrentThread.h"
#include "EventLoop.h"



EventLoopThread::EventLoopThread(const ThreadInitCallback& cb,
                    const std::string& name)
                    :loop_(NULL),
                    exiting_(false),
                    thread_(std::bind(&EventLoopThread::threadFunc, this), name),
                    mutex_(),
                    cond_(),
                    callback_(cb)
{
    CurrentThread::cacheTid();
}
EventLoopThread::~EventLoopThread()
{
    exiting_ = true;
    if (loop_ != NULL)
    {
        loop_->quit();
        thread_.join();
    }
}

EventLoop* EventLoopThread::startLoop()
{
    thread_.start();    // 启动线程

    EventLoop* loop = NULL;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (loop_ == NULL)
        {
            cond_.wait(lock);
        }
        loop = loop_;
    }
    return loop;
}

//这个方法是在单独的新线程中执行的
void EventLoopThread::threadFunc()
{
    EventLoop loop; // 创建一个EventLoop对象。和线程一一对应
    if (callback_)
    {
        callback_(&loop);
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();
    }
    loop.loop();    // 开始事件循环
    std::unique_lock<std::mutex> lock(mutex_);
    loop_ = NULL;
}