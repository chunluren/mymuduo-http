/**
 * @file TcpServer.cc
 * @brief TCP 服务器实现
 *
 * 本文件实现了 TcpServer 类的所有方法，包括:
 * - 服务器初始化和启动
 * - 新连接的接受和分发
 * - 连接的移除和清理
 */

#include "TcpServer.h"
#include "logger.h"
#include "TcpConnection.h"

#include <strings.h>
#include <functional>

/**
 * @brief 检查 EventLoop 是否为空
 *
 * 这是一个辅助函数，用于在构造 TcpServer 时验证 loop 参数的有效性。
 * 如果 loop 为空，会触发 LOG_FATAL 导致程序终止。
 *
 * @param loop 要检查的 EventLoop 指针
 * @return 非空的 EventLoop 指针
 *
 * @note 这是一个内部辅助函数，不应被外部调用
 */
EventLoop* CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        // loop 为空是严重错误，直接终止程序
        LOG_FATAL("%s:%s:%d mainLoop is null!\n", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}


/**
 * @brief TcpServer 构造函数实现
 *
 * 初始化流程:
 * 1. 验证 loop 参数非空
 * 2. 初始化成员变量
 * 3. 创建 Acceptor (用于接受新连接)
 * 4. 创建 EventLoopThreadPool (I/O 线程池)
 * 5. 设置 Acceptor 的新连接回调
 */
TcpServer::TcpServer(EventLoop *loop,
              const InetAddress &listenAddr,
              const std::string &nameArg,
              Option option)
              : loop_(CheckLoopNotNull(loop))       // 确保 loop 非空
              , ipPort_(listenAddr.toIpPort())      // 保存监听地址字符串
              , name_(nameArg)                       // 保存服务器名称
              , acceptor_(std::make_unique<Acceptor>(loop, listenAddr, option == kReusePort))  // 创建 Acceptor
              , threadPool_(std::make_unique<EventLoopThreadPool>(loop, name_))  // 创建线程池
              , connectionCallback_()                // 初始化回调为空
              , messageCallback_()
              , nextConnId_(1)                       // 连接 ID 从 1 开始
{
    // 设置 Acceptor 的新连接回调
    // 当有新用户连接时，Acceptor 会执行该回调
    // 使用 bind 将 newConnection 方法绑定到当前 TcpServer 对象
    acceptor_->setNewConnectionCallback(std::bind(&TcpServer::newConnection, this,
                                        std::placeholders::_1, std::placeholders::_2));
}

/**
 * @brief TcpServer 析构函数实现
 *
 * 析构流程:
 * 1. 遍历所有连接
 * 2. 对每个连接，在对应的 subLoop 中调用 connectDestroyed
 * 3. 释放连接资源
 *
 * @note 连接必须在对应的 subLoop 线程中销毁，以保证线程安全
 */
TcpServer::~TcpServer()
{
    // 遍历所有连接，安全地移除它们
    for (auto &item : connections_)
    {
        TcpConnectionPtr conn(item.second);  // 获取连接的 shared_ptr
        item.second.reset();                  // 释放 map 中的引用
        // 在连接所属的 loop 线程中销毁连接
        conn->getLoop()->runInLoop(
            std::bind(&TcpConnection::connectDestroyed, conn)
        );
    }
}


/**
 * @brief 设置 I/O 线程数量
 * @param numThreads 线程数量
 *
 * 直接委托给线程池的 setThreadNum 方法
 */
void TcpServer::setThreadNum(int numThreads)
{
    threadPool_->setThreadNum(numThreads);
}

/**
 * @brief 启动服务器
 *
 * 启动流程:
 * 1. 使用原子操作确保只启动一次
 * 2. 启动 I/O 线程池
 * 3. 在 baseLoop 中开始监听
 *
 * @note 多次调用 start() 是安全的，只有第一次调用有效
 */
void TcpServer::start()
{
    // 原子操作: 如果 started_ 为 0，则设为 1 并返回 0
    // 这样可以确保 start() 只执行一次
    if (started_++ == 0)
    {
        // 启动线程池
        threadPool_->start(threadInitCallback_);
        // 在 baseLoop 中开始监听
        // bind 将 Acceptor::listen 绑定到 acceptor_ 对象
        loop_->runInLoop(std::bind(&Acceptor::listen, acceptor_.get()));
    }
}

/**
 * @brief 有一个新客户端连接，Acceptor 会执行此回调
 *
 * 新连接处理流程:
 * 1. 从线程池获取一个 subLoop
 * 2. 生成唯一的连接名称
 * 3. 创建 TcpConnection 对象
 * 4. 设置各种回调函数
 * 5. 在 subLoop 中初始化连接
 *
 * @param sockfd 新连接的套接字描述符
 * @param peerAddr 对端地址
 */
void TcpServer::newConnection(int sockfd, const InetAddress &peerAddr)
{
    // 获取一个 subLoop，用于管理这个新连接的 I/O
    EventLoop *ioLoop = threadPool_->getNextLoop();

    // 生成唯一的连接名称
    // 格式: 服务器名-IP:Port#连接ID
    char buf[64] = {0};
    int connId = nextConnId_.fetch_add(1);  // 原子递增连接 ID
    snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), connId);
    std::string connName = name_ + buf;

    LOG_DEBUG("TcpServer::newConnection [%s] - new connection [%s] from %s\n",
            name_.c_str(), connName.c_str(), peerAddr.toIpPort().c_str());

    // 通过 sockfd 获取其绑定的本地 IP 地址和端口号
    sockaddr_in local;
    ::bzero(&local, sizeof local);
    socklen_t addrLen = sizeof local;
    if (::getsockname(sockfd, (sockaddr*)&local, &addrLen) < 0)
    {
        LOG_ERROR("sockets::getLocalAddr");
    }
    InetAddress localAddr(local);

    // 根据连接成功的 sockfd 创建 TcpConnection 对象
    // TcpConnection 构造函数会创建 Socket 和 Channel
    auto conn = std::make_shared<TcpConnection>(ioLoop, connName, sockfd, localAddr, peerAddr);

    // 默认开启 TCP_NODELAY（关 Nagle）+ SO_KEEPALIVE
    // - HTTP/WS/IM 都是短消息为主，Nagle 与对端 delayed-ACK 合谋会带来 40ms 抖动
    // - SO_KEEPALIVE 防"半连接"长期存活；具体探测间隔仍走默认 7200s（按需在调用方调）
    conn->setTcpNoDelay(true);
    conn->setKeepAlive(true);

    // 将连接保存到连接映射表中
    connections_[connName] = conn;

    // 设置连接的各种回调函数
    // 这些回调函数由用户在 start() 之前设置
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    // 设置连接关闭的回调
    // 当连接关闭时，需要从 TcpServer 的连接映射表中移除
    conn->setCloseCallback(
        std::bind(&TcpServer::removeConnection, this, std::placeholders::_1)
    );

    // 在 subLoop 所在的线程中调用 TcpConnection::connectEstablished
    // 这会启用 Channel 的事件监听，并调用用户的连接回调
    ioLoop->runInLoop(
        std::bind(&TcpConnection::connectEstablished, conn)
    );
}

/**
 * @brief 移除连接 (线程安全)
 * @param conn 要移除的连接
 *
 * 将移除操作转发到 baseLoop 线程中执行，
 * 保证对 connections_ 映射表的访问是线程安全的
 */
void TcpServer::removeConnection(const TcpConnectionPtr &conn)
{
    // 在 baseLoop 中执行实际的移除操作
    loop_->runInLoop(
        std::bind(&TcpServer::removeConnectionInLoop, this, conn)
    );
}

/**
 * @brief 在 baseLoop 中移除连接 (内部实现)
 * @param conn 要移除的连接
 *
 * 移除流程:
 * 1. 从连接映射表中移除
 * 2. 在连接所属的 subLoop 中销毁连接
 */
void TcpServer::removeConnectionInLoop(const TcpConnectionPtr &conn)
{
    LOG_DEBUG("TcpServer::removeConnectionInLoop [%s] - connection %s\n",
            name_.c_str(), conn->name().c_str());

    // 从映射表中移除连接
    connections_.erase(conn->name());

    // 获取连接所属的 subLoop
    EventLoop *ioLoop = conn->getLoop();

    // 在 subLoop 中销毁连接
    // 这会禁用 Channel 并调用用户的断开连接回调
    ioLoop->queueInLoop(
        std::bind(&TcpConnection::connectDestroyed, conn)
    );
}