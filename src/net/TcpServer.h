/**
 * @file TcpServer.h
 * @brief TCP 服务器核心类
 *
 * 本文件定义了 TcpServer 类，是用户编写服务器程序的主要入口。
 * TcpServer 基于 Reactor 模式实现，支持:
 * - One Loop Per Thread 线程模型
 * - 自动管理连接的创建和销毁
 * - 可配置的 I/O 线程池
 *
 * @example 使用示例
 * @code
 * EventLoop loop;
 * InetAddress addr(8080);
 * TcpServer server(&loop, addr, "MyServer");
 *
 * // 设置回调函数
 * server.setConnectionCallback([](const TcpConnectionPtr& conn) {
 *     if (conn->connected()) {
 *         LOG_INFO << "New connection from " << conn->peerAddress().toIpPort();
 *     }
 * });
 *
 * server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
 *     // 处理消息
 *     std::string msg = buf->retrieveAllAsString();
 *     conn->send(msg);  // 回显
 * });
 *
 * server.setThreadNum(4);  // 4 个 I/O 线程
 * server.start();
 * loop.loop();
 * @endcode
 */

#pragma once

#include "EventLoop.h"
#include "Acceptor.h"
#include "InetAddress.h"
#include "noncopyable.h"
#include "EventLoopThreadPool.h"
#include "Callbacks.h"
#include "TcpConnection.h"
#include "Buffer.h"

#include <functional>
#include <string>
#include <memory>
#include <atomic>
#include <unordered_map>

/**
 * @class TcpServer
 * @brief 对外的服务器编程使用的类
 *
 * TcpServer 是 muduo 网络库对外的核心类，用户通过此类快速搭建 TCP 服务器。
 * 内部采用 mainReactor + subReactors 的架构:
 * - mainReactor (baseLoop_): 负责 accept 新连接
 * - subReactors (threadPool_): 负责已连接套接字的 I/O 处理
 *
 * 线程安全: 所有公共方法都是线程安全的
 */
class TcpServer : noncopyable
{
public:
    /**
     * @brief 线程初始化回调函数类型
     * @param EventLoop* 指向 I/O 线程的 EventLoop
     */
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    /**
     * @brief 端口复用选项枚举
     */
    enum Option
    {
        kNoReusePort,  ///< 不复用端口
        kReusePort,    ///< 复用端口 (SO_REUSEPORT)
    };

    /**
     * @brief 构造 TCP 服务器
     *
     * @param loop 事件循环 (baseLoop/mainLoop)，负责监听新连接
     * @param listenAddr 监听地址，包含 IP 和端口
     * @param nameArg 服务器名称，用于日志和调试
     * @param option 端口复用选项，默认不复用
     *
     * @note loop 必须非空，否则会触发断言失败
     *
     * @example
     * @code
     * EventLoop loop;
     * InetAddress addr(8080);
     * TcpServer server(&loop, addr, "MyServer");
     * @endcode
     */
    TcpServer(EventLoop *loop,
                const InetAddress &listenAddr,
                const std::string &nameArg,
                Option option = kNoReusePort);

    /**
     * @brief 析构函数
     *
     * 析构时会:
     * 1. 遍历所有连接，从对应的 subLoop 中移除
     * 2. 释放连接资源
     */
    ~TcpServer();

    /**
     * @brief 设置线程初始化回调
     * @param cb 回调函数，在每个 I/O 线程启动时调用
     *
     * @note 必须在 start() 之前调用
     */
    void setThreadInitcallback(const ThreadInitCallback &cb) { threadInitCallback_ = cb; }

    /**
     * @brief 设置新连接回调
     * @param cb 回调函数，当有新连接建立或断开时调用
     *
     * 回调参数 TcpConnectionPtr 表示连接对象:
     * - conn->connected() 返回 true 表示连接建立
     * - conn->connected() 返回 false 表示连接断开
     *
     * @note 必须在 start() 之前调用
     */
    void setConnectionCallback(const ConnectionCallback &cb) { connectionCallback_ = cb; }

    /**
     * @brief 设置消息回调
     * @param cb 回调函数，当连接上有数据可读时调用
     *
     * 回调参数:
     * - TcpConnectionPtr: 连接对象
     * - Buffer*: 输入缓冲区，包含接收到的数据
     * - Timestamp: 数据到达的时间戳
     *
     * @note 必须在 start() 之前调用
     */
    void setMessageCallback(const MessageCallback &cb) { messageCallback_ = cb; }

    /**
     * @brief 设置写完成回调
     * @param cb 回调函数，当数据发送完成时调用
     *
     * 当发送缓冲区中的数据全部写入内核发送缓冲区后触发，
     * 可用于实现流量控制或发送下一批数据。
     *
     * @note 必须在 start() 之前调用
     */
    void setWriteCompleteCallback(const WriteCompleteCallback &cb) { writeCompleteCallback_ = cb; }

    /**
     * @brief 设置底层 subLoop 的个数
     * @param numThreads I/O 线程数量
     *
     * - numThreads = 0: 所有 I/O 在 baseLoop 线程中处理
     * - numThreads > 0: 创建 numThreads 个 I/O 线程
     *
     * @note 必须在 start() 之前调用
     */
    void setThreadNum(int numThreads);

    /**
     * @brief 开启服务器监听
     *
     * 启动流程:
     * 1. 启动 I/O 线程池
     * 2. 开始监听端口
     *
     * @note 多次调用 start() 是安全的 (幂等)
     */
    void start();

private:
    /**
     * @brief 处理新连接 (内部回调)
     *
     * 当 Acceptor 接受新连接后调用此方法:
     * 1. 从线程池中选择一个 subLoop
     * 2. 创建 TcpConnection 对象
     * 3. 设置回调函数
     * 4. 在 subLoop 中初始化连接
     *
     * @param sockfd 新连接的套接字描述符
     * @param peerAddr 对端地址
     */
    void newConnection(int sockfd, const InetAddress &peerAddr);

    /**
     * @brief 移除连接 (线程安全)
     * @param conn 要移除的连接
     */
    void removeConnection(const TcpConnectionPtr &conn);

    /**
     * @brief 在 baseLoop 中移除连接 (内部实现)
     * @param conn 要移除的连接
     */
    void removeConnectionInLoop(const TcpConnectionPtr &conn);

    /// 连接映射表类型: 连接名 -> TcpConnectionPtr
    using ConnectionMap = std::unordered_map<std::string, TcpConnectionPtr>;

    EventLoop *loop_;  ///< baseLoop (mainLoop)，用户定义的 loop

    const std::string ipPort_;  ///< 监听的 IP:Port
    const std::string name_;    ///< 服务器名称

    std::unique_ptr<Acceptor> acceptor_;  ///< 运行在 mainLoop，任务就是监听新连接事件

    std::shared_ptr<EventLoopThreadPool> threadPool_;  ///< I/O 线程池 (one loop per thread)

    ConnectionCallback connectionCallback_;    ///< 有新连接时的回调
    MessageCallback messageCallback_;          ///< 有读写消息时的回调
    WriteCompleteCallback writeCompleteCallback_;  ///< 消息发送完成以后的回调

    ThreadInitCallback threadInitCallback_;  ///< loop 线程初始化的回调

    int nextConnId_;              ///< 下一个连接 ID (原子操作)
    std::atomic_int started_{0};  ///< 启动标志，防止重复启动

    ConnectionMap connections_;  ///< 保存所有的连接
};