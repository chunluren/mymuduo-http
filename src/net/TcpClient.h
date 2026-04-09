/**
 * @file TcpClient.h
 * @brief TCP 客户端
 *
 * TcpClient 是 TcpServer 的客户端镜像，基于 Reactor 架构实现。
 * - TcpServer: Acceptor + TcpConnection（被动接受连接）
 * - TcpClient: Connector + TcpConnection（主动发起连接）
 *
 * 连接建立后的 TcpConnection 与服务端完全相同，共用同一套读写/回调机制。
 *
 * @example 使用示例
 * @code
 * EventLoop loop;
 * InetAddress serverAddr("127.0.0.1", 8080);
 * TcpClient client(&loop, serverAddr, "MyClient");
 *
 * client.setConnectionCallback([](const TcpConnectionPtr& conn) {
 *     if (conn->connected()) {
 *         conn->send("Hello Server!");
 *     }
 * });
 *
 * client.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
 *     std::string msg = buf->retrieveAllAsString();
 *     std::cout << "Received: " << msg << std::endl;
 * });
 *
 * client.connect();
 * loop.loop();
 * @endcode
 */

#pragma once

#include "noncopyable.h"
#include "Callbacks.h"
#include "TcpConnection.h"

#include <mutex>
#include <atomic>
#include <string>
#include <memory>

class Connector;
class EventLoop;
class InetAddress;

class TcpClient : noncopyable
{
public:
    TcpClient(EventLoop* loop,
              const InetAddress& serverAddr,
              const std::string& nameArg);
    ~TcpClient();

    /// 发起连接
    void connect();

    /// 断开连接（优雅关闭）
    void disconnect();

    /// 停止（不再重连）
    void stop();

    /// 获取当前连接（可能为空）
    TcpConnectionPtr connection() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return connection_;
    }

    EventLoop* getLoop() const { return loop_; }
    const std::string& name() const { return name_; }

    /// 启用断开后自动重连
    void enableRetry() { retry_ = true; }
    bool retry() const { return retry_; }

    void setConnectionCallback(ConnectionCallback cb)
    { connectionCallback_ = std::move(cb); }

    void setMessageCallback(MessageCallback cb)
    { messageCallback_ = std::move(cb); }

    void setWriteCompleteCallback(WriteCompleteCallback cb)
    { writeCompleteCallback_ = std::move(cb); }

private:
    /// Connector 连接成功后的回调
    void newConnection(int sockfd);

    /// 连接关闭后的回调
    void removeConnection(const TcpConnectionPtr& conn);

    EventLoop* loop_;
    std::shared_ptr<Connector> connector_;      ///< 连接器（shared_ptr 因为 Connector 内部 shared_from_this）
    const std::string name_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;

    std::atomic<bool> retry_;                   ///< 是否自动重连
    std::atomic<bool> connect_;                 ///< 是否要连接

    int nextConnId_;                            ///< 连接 ID（非线程安全，仅在 loop 线程使用）
    mutable std::mutex mutex_;
    TcpConnectionPtr connection_;               ///< 当前连接
};
