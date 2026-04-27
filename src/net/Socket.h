#pragma once

#include "noncopyable.h"

class InetAddress;

class Socket : noncopyable { 
public:
    explicit Socket(int sockfd)
    : sockfd_(sockfd)
    {
     
    }

    ~Socket();

    int fd() const
    {
        return sockfd_;
    }
    void bindAddress(const InetAddress &localaddr);
    void listen();
    int accept(InetAddress *peeraddr);
    void shutdownWrite();
    void setTcpNoDelay(bool on);
    void setReuseAddr(bool on);
    void setReusePort(bool on);
    void setKeepAlive(bool on);
    /// 调收紧 keepalive 探测：默认 60s 空闲后开探，10s 一次，3 次失败断
    void setKeepAliveParams(int idleSec, int intvlSec, int probeCount);
private:
    const int sockfd_;
};