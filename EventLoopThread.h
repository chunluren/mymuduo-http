#pragma once

#include "noncopyable.h"
#include "Thread.h"

#include <functional>
#include<mutex>
#include <condition_variable>

class EventLoop;

class EventLoopThread : noncopyable
{
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;
    // 构造函数
    EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback(),
                    const std::string& name = std::string());
    ~EventLoopThread();

    EventLoop* startLoop();
private:
    void threadFunc();
    EventLoop* loop_;   // 线程对应的EventLoop
    bool exiting_;  // 线程退出标志
    Thread thread_; // 线程实例
    std::mutex mutex_;
    std::condition_variable cond_;  // 条件变量
    ThreadInitCallback callback_;   // 线程初始化回调函数
};