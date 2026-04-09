/**
 * @file Connector.cc
 * @brief 客户端连接器实现
 *
 * 核心流程:
 * 1. 创建非阻塞 socket
 * 2. 调用 ::connect()，返回 EINPROGRESS
 * 3. 注册 EPOLLOUT，等待连接完成
 * 4. EPOLLOUT 触发后 getsockopt(SO_ERROR) 检查结果
 * 5. 连接失败则 runAfter 重试（指数退避）
 */

#include "Connector.h"
#include "Channel.h"
#include "EventLoop.h"
#include "logger.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int createNonblockingSocket()
{
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd < 0)
    {
        LOG_FATAL("Connector::createNonblockingSocket error:%d\n", errno);
    }
    return sockfd;
}

Connector::Connector(EventLoop* loop, const InetAddress& serverAddr)
    : loop_(loop)
    , serverAddr_(serverAddr)
    , connect_(false)
    , state_(kDisconnected)
    , retryDelayMs_(kInitRetryDelayMs)
{
}

Connector::~Connector()
{
    LOG_DEBUG("Connector::~Connector\n");
}

void Connector::start()
{
    connect_ = true;
    loop_->runInLoop(std::bind(&Connector::startInLoop, this));
}

void Connector::restart()
{
    setState(kDisconnected);
    retryDelayMs_ = kInitRetryDelayMs;
    connect_ = true;
    startInLoop();
}

void Connector::stop()
{
    connect_ = false;
    loop_->queueInLoop(std::bind(&Connector::stopInLoop, this));
}

void Connector::startInLoop()
{
    if (connect_)
    {
        connect();
    }
    else
    {
        LOG_DEBUG("Connector::startInLoop: do not connect\n");
    }
}

void Connector::stopInLoop()
{
    if (state_ == kConnecting)
    {
        setState(kDisconnected);
        int sockfd = removeAndResetChannel();
        ::close(sockfd);
    }
}

void Connector::connect()
{
    int sockfd = createNonblockingSocket();
    const sockaddr_in* addr = serverAddr_.getSockAddr();
    int ret = ::connect(sockfd, (const sockaddr*)addr, sizeof(*addr));

    int savedErrno = (ret == 0) ? 0 : errno;

    switch (savedErrno)
    {
    case 0:
    case EINPROGRESS:   // 非阻塞 connect 正在进行
    case EINTR:
    case EISCONN:       // 已经连接
        connecting(sockfd);
        break;

    case EAGAIN:
    case EADDRINUSE:
    case EADDRNOTAVAIL:
    case ECONNREFUSED:
    case ENETUNREACH:
        retry(sockfd);
        break;

    case EACCES:
    case EPERM:
    case EAFNOSUPPORT:
    case EALREADY:
    case EBADF:
    case EFAULT:
    case ENOTSOCK:
        LOG_ERROR("Connector::connect error: %d\n", savedErrno);
        ::close(sockfd);
        break;

    default:
        LOG_ERROR("Connector::connect unexpected error: %d\n", savedErrno);
        ::close(sockfd);
        break;
    }
}

void Connector::connecting(int sockfd)
{
    setState(kConnecting);
    channel_.reset(new Channel(loop_, sockfd));

    channel_->setWriteCallback(std::bind(&Connector::handleWrite, this));
    channel_->setErrorCallback(std::bind(&Connector::handleError, this));

    // 注册写事件，等待连接完成
    channel_->enableWriting();
}

void Connector::handleWrite()
{
    if (state_ == kConnecting)
    {
        // 从 epoll 移除 Channel，后续由 TcpConnection 接管
        int sockfd = removeAndResetChannel();

        // 检查 socket 错误
        int optval;
        socklen_t optlen = sizeof(optval);
        if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
        {
            optval = errno;
        }

        if (optval)
        {
            // 连接失败
            LOG_ERROR("Connector::handleWrite SO_ERROR = %d\n", optval);
            retry(sockfd);
        }
        else
        {
            // 连接成功
            setState(kConnected);
            if (connect_)
            {
                // 重置重试延迟
                retryDelayMs_ = kInitRetryDelayMs;

                if (newConnectionCallback_)
                {
                    newConnectionCallback_(sockfd);
                }
            }
            else
            {
                ::close(sockfd);
            }
        }
    }
}

void Connector::handleError()
{
    LOG_ERROR("Connector::handleError state=%d\n", state_);
    if (state_ == kConnecting)
    {
        int sockfd = removeAndResetChannel();

        int optval;
        socklen_t optlen = sizeof(optval);
        if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
        {
            optval = errno;
        }
        LOG_ERROR("Connector::handleError SO_ERROR = %d\n", optval);

        retry(sockfd);
    }
}

void Connector::retry(int sockfd)
{
    ::close(sockfd);
    setState(kDisconnected);

    if (connect_)
    {
        LOG_INFO("Connector::retry connecting to %s in %d ms\n",
                 serverAddr_.toIpPort().c_str(), retryDelayMs_);

        // 使用 EventLoop 的定时器实现延迟重连
        loop_->runAfter(retryDelayMs_ / 1000.0,
                        std::bind(&Connector::startInLoop, shared_from_this()));

        // 指数退避，上限 30 秒
        retryDelayMs_ = std::min(retryDelayMs_ * 2, kMaxRetryDelayMs);
    }
}

int Connector::removeAndResetChannel()
{
    channel_->disableAll();
    channel_->remove();
    int sockfd = channel_->fd();

    // 不能在 Channel::handleEvent 中直接 reset channel
    // 需要延迟到下一轮
    loop_->queueInLoop(std::bind(&Connector::resetChannel, this));
    return sockfd;
}

void Connector::resetChannel()
{
    channel_.reset();
}
