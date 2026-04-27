#include "Socket.h"
#include "logger.h"
#include "InetAddress.h"

#include<unistd.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <strings.h>
#include <netinet/tcp.h>
#include <fcntl.h>

Socket::~Socket()
{
    close(sockfd_);
}

void Socket::bindAddress(const InetAddress &localaddr)
{
    if (0 != ::bind(sockfd_, (sockaddr*)localaddr.getSockAddr(), sizeof(sockaddr_in)))
    {
        LOG_FATAL("bind sockfd:%d fail \n", sockfd_);
    }
}

void Socket::listen()
{
    if (0 != ::listen(sockfd_, 1024))
    {
        LOG_FATAL("listen sockfd:%d fail \n", sockfd_);
    }
}

int Socket::accept(InetAddress *peeraddr)
{
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    ::bzero(&addr, sizeof(addr));
    int connfd = ::accept(sockfd_, (sockaddr*)&addr, &len);
    if (connfd >= 0)
    {
        peeraddr->setSockAddr(addr);
        // 设置非阻塞
        int flags = ::fcntl(connfd, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(connfd, F_SETFL, flags | O_NONBLOCK);
        }
    }
    return connfd;
}

void Socket::shutdownWrite()
{
    if (::shutdown(sockfd_, SHUT_WR) < 0)
    {
        LOG_ERROR("shutdownWrite error");
    }
}

void Socket::setTcpNoDelay(bool on)
{
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) < 0)
    {
        LOG_ERROR("setsockopt TCP_NODELAY failed, fd=%d", sockfd_);
    }
}

void Socket::setReuseAddr(bool on)
{
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
    {
        LOG_ERROR("setsockopt SO_REUSEADDR failed, fd=%d", sockfd_);
    }
}

void Socket::setReusePort(bool on)
{
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) < 0)
    {
        LOG_ERROR("setsockopt SO_REUSEPORT failed, fd=%d", sockfd_);
    }
}

void Socket::setKeepAlive(bool on)
{
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0)
    {
        LOG_ERROR("setsockopt SO_KEEPALIVE failed, fd=%d", sockfd_);
    }
}

void Socket::setKeepAliveParams(int idleSec, int intvlSec, int probeCount)
{
    // 调收紧探测参数：内核默认 7200/75/9 = 半连接撑两小时，对长连接 IM/WS 太松
    if (::setsockopt(sockfd_, IPPROTO_TCP, TCP_KEEPIDLE, &idleSec, sizeof(idleSec)) < 0)
        LOG_ERROR("setsockopt TCP_KEEPIDLE failed, fd=%d", sockfd_);
    if (::setsockopt(sockfd_, IPPROTO_TCP, TCP_KEEPINTVL, &intvlSec, sizeof(intvlSec)) < 0)
        LOG_ERROR("setsockopt TCP_KEEPINTVL failed, fd=%d", sockfd_);
    if (::setsockopt(sockfd_, IPPROTO_TCP, TCP_KEEPCNT, &probeCount, sizeof(probeCount)) < 0)
        LOG_ERROR("setsockopt TCP_KEEPCNT failed, fd=%d", sockfd_);
}