#pragma once
#include "noncopyable.h"
#include<functional>
#include "Timestamp.h"
#include<memory>

//只需要指针或者引用的时候可以使用前置声明，要使用具体的类，则要引入头文件
class EventLoop;



/*
理解EventLoop和Channle、Poller的关系，在Reactor模型中，EventLoop和Channel、Poller对应Demultiplexer
Channel 类封装了socketfd和socketfd对应的事件（EPOLLIN、EPOLLOUT等） 
绑定了Poller和回调函数
*/
class Channel : noncopyable{ 
public:
       /**
     * @brief 事件回调函数类型，无参数无返回值。
     *
     * 用于表示通用的事件处理回调函数，例如可读、可写事件的处理。
     */
    using EventCallback = std::function<void()>;
    
    /**
     * @brief 读事件回调函数类型，包含时间戳参数。
     *
     * 用于处理带有时间戳信息的读事件，记录事件触发的具体时间。
     */
    using ReadEventCallback = std::function<void(Timestamp)>;
    
    /**
     * @brief 构造一个新的Channel对象，绑定到指定的事件循环和文件描述符。
     *
     * @param loop 指向EventLoop对象的指针，负责管理该Channel的事件循环
     * @param fd 文件描述符，用于监听I/O事件
     */
    Channel(EventLoop* loop, int fd);
    
    /**
     * @brief 析构Channel对象，释放相关资源并从事件循环中移除注册。
     *
     * 注意：确保在销毁前已停止监听，避免悬空指针问题。
     */
    ~Channel();
    
    /**
     * @brief 处理已发生的事件，根据事件类型调用相应的回调函数。
     *
     * @param receiveTime 事件触发的时间戳，用于记录事件发生时间
     * @return void 无返回值
     */
    void handleEvent(Timestamp receiveTime);
    
    /**
     * @brief 设置监听的事件类型，如读事件、写事件等。
     *
     * @param events 事件类型，可以是多个事件类型组合，如 kReadEvent | kWriteEvent
     * @return void 无返回值
     */
    void setReadCallback(ReadEventCallback cb){readCallback_ = std::move(cb);}
    void setWriteCallback(EventCallback cb){writeCallback_ = std::move(cb);}
    void setCloseCallback(EventCallback cb){closeCallback_ = std::move(cb);}
    void setErrorCallback(EventCallback cb){errorCallback_ = std::move(cb);}
    //防止channel被手动remove掉，channel还在执行回调操作
    void tie(const std::shared_ptr<void>&);

    int fd() const{return fd_;}
    int events() const{return events_;}
    void set_revents(int revt){revents_ = revt;}

    //设置fd上channel监听的事件类型
    void enableReading(){events_ |= kReadEvent; update(); }
    void disableReading(){events_ &= ~kReadEvent; update();}
    void enableWriting(){events_ |= kWriteEvent; update();}
    void disableWriting(){events_ &= ~kWriteEvent; update();}
    void disableAll(){events_ = kNoneEvent; update();}

    //设置fd当前监听事件
    bool isNoneEvent() const { return events_ == kNoneEvent; }
    bool isWriting() const { return events_ & kWriteEvent; }
    bool isReading() const { return events_ & kReadEvent; }

    int index(){return index_;}
    void set_index(int idx){index_ = idx;}

    //one loop one thread
    EventLoop* ownerLoop(){return loop_;}
    void remove();
private:

    void update();
    void handleEventWithGuard(Timestamp receiveTime);

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;
    EventLoop *loop_; //事件循环
    const int fd_;    //fd, Poller监听的对象
    int events_;      //注册fd感兴趣的事件
    int revents_;     //poller返回的具体发生的事件
    int index_;

    std::weak_ptr<void> tie_;
    bool tied_;
    ///回调操作,因为channel能够监听事件，知道最终发生的事件revents，所以channel执行相应的回调
    ReadEventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback errorCallback_;
    EventCallback closeCallback_;

};