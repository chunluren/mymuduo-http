#include"Thread.h"
#include"CurrentThread.h"

#include<semaphore.h>

std::atomic_int Thread::numCreated_(0);

Thread::Thread(ThreadFunc func, const std::string &name)
    : started_(false)
    , joined_(false)
    , tid_(0)
    , func_(std::move(func))
    , name_(name)
{
    setDefaultName();
}
Thread::~Thread()
{
    if(started_ && !joined_)
    {
        thread_->detach();  // 线程析构时，自动调用
    }
}

void Thread::start()    //一个thread对象，记录的一个线程的详细信息
{
    started_ = true;
    sem_t sem;
    sem_init(&sem, false, 0);
    // 开启线程
    thread_ = std::make_shared<std::thread>([&](){
        //获取当前线程id
        tid_ = CurrentThread::tid();
        sem_post(&sem);
        // 开启一个新线程，专门执行该线程函数
        func_();    
    });

    //这里必须等待获取上面线程的tid
    sem_wait(&sem);

}
void Thread::join()
{
    joined_ = true;
    thread_->join();
}

void Thread::setDefaultName()
{
    int num = ++numCreated_;
    if(name_.empty())
    {
        char buf[32] = {0};
        snprintf(buf, sizeof buf, "Thread%d", num);
        name_ = buf;
    }
}