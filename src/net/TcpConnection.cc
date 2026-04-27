#include "TcpConnection.h"
#include "logger.h"
#include "Socket.h"
#include "Channel.h"
#include "EventLoop.h"

#include<unistd.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <sys/uio.h>             /* writev */
#include <strings.h>
#include <netinet/tcp.h>

static EventLoop* CheckLoopNotNull(EventLoop *loop)
{
    if(loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d TcpConnection Loop is null!\n", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}

TcpConnection::TcpConnection(EventLoop *loop,
                             const std::string& nameArg,
                             int sockfd,
                             const InetAddress& localAddr,
                             const InetAddress& peerAddr)
    : loop_(CheckLoopNotNull(loop)),
      name_(nameArg),
      state_(kConnecting),
      reading_(true),
      socket_(std::make_unique<Socket>(sockfd)),
      channel_(std::make_unique<Channel>(loop, sockfd)),
      localAddr_(localAddr),
      peerAddr_(peerAddr),
      highWaterMark_(64*1024*1024)
{
    // 绑定回调函数,给channel设置相应的回调函数
    channel_->setReadCallback(
        std::bind(&TcpConnection::handleRead, this, std::placeholders::_1)
    );
    channel_->setWriteCallback(
        std::bind(&TcpConnection::handleWrite, this)
    );
    channel_->setCloseCallback(
        std::bind(&TcpConnection::handleClose, this)
    );
    channel_->setErrorCallback(
        std::bind(&TcpConnection::handleError, this)
    );
    LOG_DEBUG("TcpConnection::ctor[%s] at fd=%d\n", name_.c_str(), sockfd);
    socket_->setKeepAlive(true);
}

TcpConnection::~TcpConnection()
{
    LOG_DEBUG("TcpConnection::dtor[%s] at fd=%d state=%d\n", name_.c_str(), channel_->fd(), (int)state_);
}

void TcpConnection::handleRead(Timestamp receiveTime)
{ 
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    if(n > 0)
    {
        // 触发读回调函数
        messageCallback_(shared_from_this(), &inputBuffer_, receiveTime); 
    }
    else if(n == 0)
    {
        handleClose();
    }
    else
    {
        errno = savedErrno;
        LOG_ERROR("TcpConnection::handleRead");
        handleError();
    }
}

void TcpConnection::connectEstablished()
{
    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading(); 

    connectionCallback_(shared_from_this());
}

void TcpConnection::connectDestroyed()
{
    if(state_ == kConnected)
    {
        setState(kDisconnected);
        channel_->disableAll();
        connectionCallback_(shared_from_this());
    }
    channel_->remove();
}


void TcpConnection::handleWrite()
{
    if(channel_->isWriting())
    {
        int savedErrno = 0;
        ssize_t n = outputBuffer_.writeFd(channel_->fd(), &savedErrno);
        if(n > 0)
        {
            outputBuffer_.retrieve(n);
            if(outputBuffer_.readableBytes() == 0)
            {
                channel_->disableWriting();
                if(writeCompleteCallback_)
                {
                    // 唤醒loop_对应的线程,执行回调
                    loop_->queueInLoop(
                        std::bind(writeCompleteCallback_, shared_from_this())
                    );
                }
                if(state_ == kDisconnecting)
                {
                    shutdownInLoop();
                }
            }
        }
        else
        {
            LOG_ERROR("TcpConnection::handleWrite");
        }
    }
    else
    {
        LOG_ERROR("TcpConnection fd=%d is down, no more writing\n", channel_->fd());
    }
}

void TcpConnection::handleClose()
{
    LOG_DEBUG("TcpConnection::handleClose fd=%d state=%d \n", channel_->fd(), (int)state_);
    setState(kDisconnected); 
    channel_->disableAll();

    TcpConnectionPtr connPtr(shared_from_this());
    connectionCallback_(connPtr);   // 触发连接关闭回调函数
    closeCallback_(connPtr);    // 触发closecallback
}

void TcpConnection::handleError()
{
    int optval;
    socklen_t optlen = sizeof optval;
    int err = 0;
    if(::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
    {
        err = errno;
    }
    else
    {
        err = optval;
    }
    LOG_ERROR("TcpConnection::handleError name=%s - SO_ERROR=%d\n", name_.c_str(), err);
}


void TcpConnection::send(const std::string& message)
{
    if(state_ == kConnected)
    {
        if(loop_->isInLoopThread())
        {
            sendInLoop(message.c_str(), message.size());
        }
        else
        {
            // 按值捕获 message 和 shared_from_this()，避免悬空指针
            loop_->runInLoop(
                [self = shared_from_this(), msg = message]() {
                    self->sendInLoop(msg.c_str(), msg.size());
                }
            );
        }
    }
}

/**
 * @brief 发送原始数据（零拷贝优化版）
 * @param data 数据指针
 * @param len 数据长度
 *
 * 避免构造 std::string 的开销。同线程直接发送，
 * 跨线程时需要拷贝一次（因为原始指针可能在发送前失效）。
 */
void TcpConnection::send(const void* data, size_t len)
{
    if(state_ == kConnected)
    {
        if(loop_->isInLoopThread())
        {
            sendInLoop(data, len);
        }
        else
        {
            /// 跨线程发送：原始指针可能在 runInLoop 执行前被释放，
            /// 必须拷贝到 std::string 中以保证数据生命周期安全
            std::string msg(static_cast<const char*>(data), len);
            loop_->runInLoop(
                [self = shared_from_this(), msg = std::move(msg)]() {
                    self->sendInLoop(msg.c_str(), msg.size());
                }
            );
        }
    }
}

/**
 * @brief 发送数据（移动语义优化版）
 * @param message 待发送的消息（右值引用，避免不必要的拷贝）
 *
 * 接受右值引用，在跨线程场景下通过 std::move 将数据转移到
 * IO 线程的回调闭包中，减少一次深拷贝。
 * 同线程则直接使用原始数据发送，无需额外拷贝。
 */
void TcpConnection::send(std::string&& message)
{
    if(state_ == kConnected)
    {
        if(loop_->isInLoopThread())
        {
            sendInLoop(message.c_str(), message.size());
        }
        else
        {
            /// 利用 std::move 将 message 移动到闭包中，
            /// 避免跨线程时的深拷贝开销
            loop_->runInLoop(
                [self = shared_from_this(), msg = std::move(message)]() {
                    self->sendInLoop(msg.c_str(), msg.size());
                }
            );
        }
    }
}

void TcpConnection::shutdown()
{
    if(state_ == kConnected)
    {
        setState(kDisconnecting);
        // 捕获 shared_from_this()，避免 UAF
        loop_->runInLoop(
            [self = shared_from_this()]() {
                self->shutdownInLoop();
            }
        );
    }
}

void TcpConnection::shutdownInLoop()
{
    if(!channel_->isWriting())  //表示数据已经发送完毕
    {
        socket_->shutdownWrite();   //关闭写端
    }
}

void TcpConnection::setTcpNoDelay(bool on)
{
    socket_->setTcpNoDelay(on);
}

void TcpConnection::setKeepAlive(bool on)
{
    socket_->setKeepAlive(on);
}

void TcpConnection::setKeepAliveParams(int idleSec, int intvlSec, int probeCount)
{
    socket_->setKeepAliveParams(idleSec, intvlSec, probeCount);
}

// writev 多缓冲区发送 — 见 TcpConnection.h IoSlice 注释
// 实现要点：
//   1. 跨线程调用：拼成一个 string runInLoop（退化路径，但保正确性）
//   2. 同线程：
//      a. outputBuffer 已有数据 → 全部 append 到 outputBuffer，不要 writev（防交错）
//      b. outputBuffer 为空 → 一次 writev；写不完的尾巴 append 到 outputBuffer
void TcpConnection::sendIov(const IoSlice* slices, int count)
{
    if (count <= 0 || state_ != kConnected) return;

    if (!loop_->isInLoopThread())
    {
        // 跨线程：拼成一份 string 转入 IO 线程（正确性优先）
        size_t total = 0;
        for (int i = 0; i < count; ++i) total += slices[i].len;
        std::string buf;
        buf.reserve(total);
        for (int i = 0; i < count; ++i)
            buf.append(static_cast<const char*>(slices[i].data), slices[i].len);
        loop_->runInLoop(
            [self = shared_from_this(), msg = std::move(buf)]() {
                self->sendInLoop(msg.c_str(), msg.size());
            });
        return;
    }

    if (state_ == kDisconnected) {
        LOG_ERROR("disconnected, give up writing");
        return;
    }

    // outputBuffer 已经有积压：必须先排队，不能用 writev 抢前
    if (channel_->isWriting() || outputBuffer_.readableBytes() > 0)
    {
        size_t added = 0;
        size_t oldLen = outputBuffer_.readableBytes();
        for (int i = 0; i < count; ++i) {
            outputBuffer_.append(static_cast<const char*>(slices[i].data), slices[i].len);
            added += slices[i].len;
        }
        if (oldLen < highWaterMark_ && oldLen + added >= highWaterMark_ && highWaterMarkCallback_)
        {
            loop_->queueInLoop(
                std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + added));
        }
        if (!channel_->isWriting()) channel_->enableWriting();
        return;
    }

    // outputBuffer 空 → 直接 writev
    constexpr int kMaxIov = 16;  // IOV_MAX >= 1024，但 HTTP 一般 2-4 段
    iovec iov[kMaxIov];
    int n = std::min(count, kMaxIov);
    size_t total = 0;
    for (int i = 0; i < n; ++i) {
        iov[i].iov_base = const_cast<void*>(slices[i].data);
        iov[i].iov_len  = slices[i].len;
        total += slices[i].len;
    }

    ssize_t nwrote = ::writev(channel_->fd(), iov, n);
    bool faultError = false;
    size_t remaining = total;

    if (nwrote >= 0) {
        remaining = total - static_cast<size_t>(nwrote);
        if (remaining == 0 && writeCompleteCallback_) {
            loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
        }
    } else {
        nwrote = 0;
        if (errno != EWOULDBLOCK) {
            LOG_ERROR("TcpConnection::sendIov writev");
            if (errno == EPIPE || errno == ECONNRESET) faultError = true;
        }
    }

    // 处理 iov 没塞完的（既包括 nwrote<total 也包括 count>kMaxIov 的尾段）
    if (!faultError && remaining > 0) {
        size_t skip = static_cast<size_t>(nwrote);
        for (int i = 0; i < count && remaining > 0; ++i) {
            size_t segLen = slices[i].len;
            if (skip >= segLen) { skip -= segLen; continue; }
            const char* p = static_cast<const char*>(slices[i].data) + skip;
            outputBuffer_.append(p, segLen - skip);
            skip = 0;
        }
        size_t curLen = outputBuffer_.readableBytes();
        if (curLen >= highWaterMark_ && highWaterMarkCallback_) {
            loop_->queueInLoop(std::bind(highWaterMarkCallback_, shared_from_this(), curLen));
        }
        if (!channel_->isWriting()) channel_->enableWriting();
    }
}

// 发送数据 应用写的快而内核发送数据慢，需要把待发送数据写入缓冲区，设置水位回调
void TcpConnection::sendInLoop(const void *data, size_t len)
{ 
    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;
    //之前调用过shutdown，不能再发送数据了
    if(state_ == kDisconnected)
    {
        LOG_ERROR("disconnected, give up writing");
        return;
    }
    //表示channel_第一次写数据，而且缓冲区没有待发送数据
    if(!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
        nwrote = ::write(channel_->fd(), data, len); 
        if(nwrote >= 0)
        {
            remaining = len - nwrote;
            if(remaining == 0 && writeCompleteCallback_)
            {
                //数据全部发送完成，不用继续注册EPOLLOUT事件
                loop_->queueInLoop(
                    std::bind(writeCompleteCallback_, shared_from_this())
                );
            }
        }
        else
        {
            nwrote = 0;
            if(errno != EWOULDBLOCK)
            {
                LOG_ERROR("TcpConnection::sendInLoop");
                if(errno == EPIPE || errno == ECONNRESET)
                {
                    faultError = true;
                }
            }
        }
    }
    //没有发送完数据,剩余数据需要放入缓冲区中，然后继续注册EPOLLOUT事件，poller会再次调用该函数
    if(!faultError && remaining > 0)
    {
        // 发送缓冲区剩余的待发送数据
        size_t oldLen = outputBuffer_.readableBytes();
        if(oldLen + remaining >= highWaterMark_
            && oldLen < highWaterMark_
            && highWaterMarkCallback_)
        {
            loop_->queueInLoop(
                std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining)
            );
        } 
        outputBuffer_.append((char*)data + nwrote, remaining);
        if(!channel_->isWriting())
        {
            channel_->enableWriting();  //这里注册channel的写事件，poller注册fd发生epoll_wait时，会调用channel的回调函数
        }
    }
}