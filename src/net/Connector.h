/**
 * @file Connector.h
 * @brief 客户端连接器
 *
 * Connector 负责发起非阻塞 TCP 连接，是 Acceptor 的客户端镜像。
 * - Acceptor: 被动 accept 连接（服务端）
 * - Connector: 主动 connect 连接（客户端）
 *
 * 支持连接失败后的自动重试（指数退避）。
 */

#pragma once

#include "noncopyable.h"
#include "InetAddress.h"

#include <functional>
#include <memory>
#include <atomic>

class Channel;
class EventLoop;

class Connector : noncopyable, public std::enable_shared_from_this<Connector>
{
public:
    using NewConnectionCallback = std::function<void(int sockfd)>;

    Connector(EventLoop* loop, const InetAddress& serverAddr);
    ~Connector();

    void setNewConnectionCallback(const NewConnectionCallback& cb)
    { newConnectionCallback_ = cb; }

    const InetAddress& serverAddress() const { return serverAddr_; }

    /// 发起连接（可在任意线程调用）
    void start();

    /// 重新连接（必须在 loop 线程调用）
    void restart();

    /// 停止连接（可在任意线程调用）
    void stop();

private:
    enum States { kDisconnected, kConnecting, kConnected };

    void setState(States s) { state_ = s; }

    void startInLoop();
    void stopInLoop();

    void connect();
    void connecting(int sockfd);

    void handleWrite();
    void handleError();

    void retry(int sockfd);
    int removeAndResetChannel();
    void resetChannel();

    EventLoop* loop_;
    InetAddress serverAddr_;
    std::atomic<bool> connect_;
    States state_;
    std::unique_ptr<Channel> channel_;
    NewConnectionCallback newConnectionCallback_;
    int retryDelayMs_;

    static const int kMaxRetryDelayMs = 30 * 1000;   // 最大重试延迟 30 秒
    static const int kInitRetryDelayMs = 500;          // 初始重试延迟 500ms
};
