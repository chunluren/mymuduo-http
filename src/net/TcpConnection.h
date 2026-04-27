/**
 * @file TcpConnection.h
 * @brief TCP 连接类
 *
 * 本文件定义了 TcpConnection 类，表示一个 TCP 连接。
 * TcpConnection 是 muduo 网络库的核心类之一，负责:
 * - 连接的读写操作
 * - 连接状态管理
 * - 回调函数的管理和调用
 *
 * 生命周期管理:
 * TcpConnection 使用 shared_ptr 管理，确保在异步操作期间对象不会被销毁。
 * 通过 enable_shared_from_this 获取自身的 shared_ptr。
 *
 * @example 使用示例
 * @code
 * // TcpConnection 通常由 TcpServer 创建，用户通过回调使用
 * server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
 *     std::string msg = buf->retrieveAllAsString();
 *     conn->send("Echo: " + msg);
 * });
 * @endcode
 */

#pragma once

#include "noncopyable.h"
#include "InetAddress.h"
#include "Callbacks.h"
#include "Buffer.h"

#include <memory>
#include <atomic>
#include <memory>
#include <string>

class Channel;
class EventLoop;
class Socket;

/**
 * @class TcpConnection
 * @brief TCP 连接类，管理单个 TCP 连接
 *
 * TcpConnection 封装了一个 TCP 连接的所有信息:
 * - Socket: 封装 socket fd
 * - Channel: 用于事件监听
 * - 输入/输出缓冲区
 * - 各种回调函数
 *
 * 线程安全:
 * TcpConnection 只在其所属的 subLoop 线程中被访问。
 * 跨线程操作通过 runInLoop 实现。
 *
 * @note TcpConnection 不可拷贝 (继承自 noncopyable)
 * @note TcpConnection 使用 shared_ptr 管理生命周期
 */
class TcpConnection: noncopyable, public std::enable_shared_from_this<TcpConnection>
{
public:
    /**
     * @brief 构造函数
     * @param loop 所属的 EventLoop (subLoop)
     * @param nameArg 连接名称
     * @param sockfd socket 文件描述符
     * @param localAddr 本地地址
     * @param peerAddr 对端地址
     */
    TcpConnection(EventLoop* loop,
                  const std::string& nameArg,
                  int sockfd,
                  const InetAddress& localAddr,
                  const InetAddress& peerAddr);

    /**
     * @brief 析构函数
     *
     * 关闭 socket 并释放资源
     */
    ~TcpConnection();

    /**
     * @brief 获取所属的 EventLoop
     * @return EventLoop 指针
     */
    EventLoop* getLoop() const { return loop_; }

    /**
     * @brief 获取连接名称
     * @return 连接名称字符串
     */
    const std::string& name() const { return name_; }

    /**
     * @brief 获取本地地址
     * @return 本地地址
     */
    const InetAddress& localAddress() const { return localAddr_; }

    /**
     * @brief 获取对端地址
     * @return 对端地址
     */
    const InetAddress& peerAddress() const { return peerAddr_; }

    /**
     * @brief 检查连接是否已建立
     * @return true 如果连接已建立
     */
    bool connected() const { return state_ == kConnected; }

    /**
     * @brief 发送数据 (线程安全)
     * @param message 要发送的消息
     *
     * 如果在当前线程，直接发送
     * 否则，将发送操作转发到所属的 EventLoop 线程
     */
    void send(const std::string& message);

    /// 发送原始数据（避免构造 string）
    void send(const void* data, size_t len);

    /// 移动语义发送（避免跨线程拷贝）
    void send(std::string&& message);

    /**
     * @brief 关闭写端 (半关闭)
     *
     * 调用 shutdown() 后，连接变成半关闭状态:
     * - 可以继续读取数据
     * - 不能再写入数据
     */
    void shutdown();

    /**
     * @brief 关闭 Nagle 算法（降低小包响应延迟）
     *
     * 默认推荐 true：HTTP 响应/WS 小帧/IM ack 都是短消息，Nagle 会与对端的
     * 延迟 ACK 合谋出现 40ms 等待。开了 NODELAY 内核会立刻发包。
     */
    void setTcpNoDelay(bool on);

    /**
     * @brief 启用 SO_KEEPALIVE
     */
    void setKeepAlive(bool on);

    /// 调 keepalive 探测参数，见 Socket::setKeepAliveParams
    void setKeepAliveParams(int idleSec, int intvlSec, int probeCount);

    /**
     * @brief 在 EventLoop 线程中执行 shutdown
     *
     * shutdown() 的内部实现
     */
    void shutdownInLoop();

    /**
     * @brief 设置连接回调函数
     * @param cb 回调函数
     *
     * 当连接建立或断开时调用
     */
    void setConnectionCallback(const ConnectionCallback& cb)
    { connectionCallback_ = cb; }

    /**
     * @brief 设置消息回调函数
     * @param cb 回调函数
     *
     * 当连接上有数据可读时调用
     */
    void setMessageCallback(const MessageCallback& cb)
    { messageCallback_ = cb; }

    /**
     * @brief 设置写完成回调函数
     * @param cb 回调函数
     *
     * 当发送缓冲区中的数据全部发送完成时调用
     */
    void setWriteCompleteCallback(const WriteCompleteCallback& cb)
    { writeCompleteCallback_ = cb; }

    /**
     * @brief 设置高水位回调函数
     * @param cb 回调函数
     *
     * 当发送缓冲区数据量超过阈值时调用
     * 用于实现流量控制
     */
    void setHighWaterMarkCallback(const HighWaterMarkCallback& cb)
    { highWaterMarkCallback_ = cb; }

    /**
     * @brief 设置关闭回调函数
     * @param cb 回调函数
     *
     * 由 TcpServer 设置，用于从连接映射表中移除连接
     */
    void setCloseCallback(const CloseCallback& cb)
    { closeCallback_ = cb; }

    /**
     * @brief 连接建立 (由 TcpServer 调用)
     *
     * 在新连接建立时调用:
     * 1. 设置状态为 kConnected
     * 2. 启用 Channel 的读事件
     * 3. 调用用户的连接回调
     */
    void connectEstablished();

    /**
     * @brief 连接销毁 (由 TcpServer 调用)
     *
     * 在连接关闭时调用:
     * 1. 设置状态为 kDisconnected
     * 2. 禁用 Channel 的所有事件
     * 3. 调用用户的关闭回调
     */
    void connectDestroyed();

private:
    /// 连接状态枚举
    enum StateE {
        kDisconnected,   ///< 已断开
        kConnecting,     ///< 正在连接
        kConnected,      ///< 已连接
        kDisconnecting   ///< 正在断开
    };

    /**
     * @brief 设置连接状态
     * @param state 新状态
     */
    void setState(StateE state) { state_ = state; }

    /**
     * @brief 处理读事件
     * @param receiveTime 事件触发时间
     *
     * 从 socket 读取数据到 inputBuffer_，然后调用消息回调
     */
    void handleRead(Timestamp receiveTime);

    /**
     * @brief 处理写事件
     *
     * 将 outputBuffer_ 中的数据写入 socket
     */
    void handleWrite();

    /**
     * @brief 处理关闭事件
     *
     * 调用用户的关闭回 closeCallback_
     */
    void handleClose();

    /**
     * @brief 处理错误事件
     *
     * 记录错误日志
     */
    void handleError();

    /**
     * @brief 在 EventLoop 线程中发送数据
     * @param message 数据指针
     * @param len 数据长度
     */
    void sendInLoop(const void* message, size_t len);

    EventLoop* loop_;           ///< subLoop 所属线程
    const std::string name_;    ///< 连接名称
    std::atomic_int state_;     ///< 连接状态 (原子变量)
    bool reading_;              ///< 是否正在读取

    std::unique_ptr<Socket> socket_;   ///< Socket 封装
    std::unique_ptr<Channel> channel_; ///< Channel 封装

    const InetAddress localAddr_;  ///< 本地地址
    const InetAddress peerAddr_;   ///< 对端地址

    ConnectionCallback connectionCallback_;       ///< 连接回调
    MessageCallback messageCallback_;             ///< 消息回调
    CloseCallback closeCallback_;                 ///< 关闭回调
    WriteCompleteCallback writeCompleteCallback_; ///< 写完成回调
    HighWaterMarkCallback highWaterMarkCallback_; ///< 高水位回调

    size_t highWaterMark_;  ///< 高水位阈值

    Buffer inputBuffer_;   ///< 输入缓冲区
    Buffer outputBuffer_;  ///< 输出缓冲区
};