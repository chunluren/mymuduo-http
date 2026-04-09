/**
 * @file TcpClient.cc
 * @brief TCP 客户端实现
 *
 * TcpClient 封装了 Connector + TcpConnection:
 * - Connector 负责连接建立（非阻塞 connect + 重试）
 * - 连接成功后创建 TcpConnection，复用服务端同一套读写逻辑
 * - 连接断开后可选自动重连
 */

#include "TcpClient.h"
#include "Connector.h"
#include "EventLoop.h"
#include "logger.h"

#include <strings.h>
#include <functional>

TcpClient::TcpClient(EventLoop* loop,
                     const InetAddress& serverAddr,
                     const std::string& nameArg)
    : loop_(loop)
    , connector_(std::make_shared<Connector>(loop, serverAddr))
    , name_(nameArg)
    , retry_(false)
    , connect_(true)
    , nextConnId_(1)
{
    connector_->setNewConnectionCallback(
        std::bind(&TcpClient::newConnection, this, std::placeholders::_1));

    LOG_INFO("TcpClient::TcpClient [%s] - connector %s\n",
             name_.c_str(), serverAddr.toIpPort().c_str());
}

TcpClient::~TcpClient()
{
    LOG_INFO("TcpClient::~TcpClient [%s]\n", name_.c_str());

    TcpConnectionPtr conn;
    bool unique = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        unique = connection_.unique();
        conn = connection_;
    }

    if (conn)
    {
        // 设置一个关闭回调，让 TcpConnection 在关闭时自行清理
        CloseCallback cb = [loop = loop_](const TcpConnectionPtr& c) {
            loop->queueInLoop(
                std::bind(&TcpConnection::connectDestroyed, c));
        };
        loop_->runInLoop(
            std::bind(&TcpConnection::setCloseCallback, conn, cb));

        if (unique)
        {
            conn->shutdown();
        }
    }
    else
    {
        connector_->stop();
    }
}

void TcpClient::connect()
{
    LOG_INFO("TcpClient::connect [%s] connecting to %s\n",
             name_.c_str(), connector_->serverAddress().toIpPort().c_str());
    connect_ = true;
    connector_->start();
}

void TcpClient::disconnect()
{
    connect_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connection_)
        {
            connection_->shutdown();
        }
    }
}

void TcpClient::stop()
{
    connect_ = false;
    connector_->stop();
}

void TcpClient::newConnection(int sockfd)
{
    // 获取本地地址
    sockaddr_in local;
    ::bzero(&local, sizeof local);
    socklen_t addrLen = sizeof local;
    ::getsockname(sockfd, (sockaddr*)&local, &addrLen);
    InetAddress localAddr(local);

    // 获取对端地址
    sockaddr_in peer;
    ::bzero(&peer, sizeof peer);
    addrLen = sizeof peer;
    ::getpeername(sockfd, (sockaddr*)&peer, &addrLen);
    InetAddress peerAddr(peer);

    // 生成连接名称
    char buf[32];
    snprintf(buf, sizeof buf, ":%s#%d", peerAddr.toIpPort().c_str(), nextConnId_);
    ++nextConnId_;
    std::string connName = name_ + buf;

    LOG_INFO("TcpClient::newConnection [%s] - new connection [%s]\n",
             name_.c_str(), connName.c_str());

    // 创建 TcpConnection（和 TcpServer::newConnection 逻辑一致）
    auto conn = std::make_shared<TcpConnection>(
        loop_, connName, sockfd, localAddr, peerAddr);

    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    conn->setCloseCallback(
        std::bind(&TcpClient::removeConnection, this, std::placeholders::_1));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = conn;
    }

    conn->connectEstablished();
}

void TcpClient::removeConnection(const TcpConnectionPtr& conn)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_.reset();
    }

    loop_->queueInLoop(
        std::bind(&TcpConnection::connectDestroyed, conn));

    if (retry_ && connect_)
    {
        LOG_INFO("TcpClient::removeConnection [%s] reconnecting to %s\n",
                 name_.c_str(), connector_->serverAddress().toIpPort().c_str());
        connector_->restart();
    }
}
