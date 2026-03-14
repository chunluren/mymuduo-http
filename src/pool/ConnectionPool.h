/**
 * @file ConnectionPool.h
 * @brief TCP 连接池
 *
 * 本文件定义了 Connection 和 ConnectionPool 类，
 * 实现了数据库/服务连接的池化管理。
 *
 * 设计目标:
 * - 复用连接，减少创建/销毁开销
 * - 控制最大连接数，防止资源耗尽
 * - 自动健康检查，清理空闲连接
 * - 线程安全
 *
 * @example 使用示例
 * @code
 * // 创建连接池
 * ConnectionPool pool("127.0.0.1", 3306, 5, 20);  // 最小5个，最大20个
 *
 * // 获取连接
 * auto conn = pool.acquire(5000);  // 5秒超时
 * if (conn) {
 *     conn->send("SELECT * FROM users", 20);
 *     char buf[1024];
 *     conn->recv(buf, sizeof(buf));
 *
 *     // 归还连接
 *     pool.release(conn);
 * }
 *
 * // 健康检查 (定期调用)
 * pool.healthCheck();
 * @endcode
 */

#pragma once

#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>
#include <functional>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

/**
 * @class Connection
 * @brief 单个连接封装
 *
 * 封装一个 TCP 连接:
 * - 文件描述符
 * - 主机地址和端口
 * - 最后使用时间
 *
 * 提供 send/recv 等基本操作
 */
class Connection {
public:
    using Ptr = std::shared_ptr<Connection>;

    /**
     * @brief 构造连接
     * @param fd 文件描述符
     * @param host 主机地址
     * @param port 端口号
     */
    Connection(int fd, const std::string& host, int port)
        : fd_(fd), host_(host), port_(port), lastUsed_(0)
    {}

    /**
     * @brief 析构函数，自动关闭 fd
     */
    ~Connection() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    /**
     * @brief 获取文件描述符
     */
    int fd() const { return fd_; }

    /**
     * @brief 检查连接是否有效
     */
    bool valid() const { return fd_ >= 0; }

    /**
     * @brief 获取最后使用时间
     * @return Unix 时间戳 (秒)
     */
    int64_t lastUsed() const { return lastUsed_; }

    /**
     * @brief 标记为已使用
     *
     * 更新 lastUsed_ 为当前时间
     */
    void markUsed() {
        lastUsed_ = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    /**
     * @brief 发送数据
     * @param buf 数据缓冲区
     * @param len 数据长度
     * @return 实际发送的字节数，-1 表示错误
     */
    ssize_t send(const void* buf, size_t len) {
        return ::send(fd_, buf, len, MSG_NOSIGNAL);
    }

    /**
     * @brief 接收数据
     * @param buf 接收缓冲区
     * @param len 缓冲区大小
     * @return 实际接收的字节数，-1 表示错误
     */
    ssize_t recv(void* buf, size_t len) {
        return ::recv(fd_, buf, len, 0);
    }

    /**
     * @brief 设置非阻塞模式
     */
    void setNonBlocking() {
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }

private:
    int fd_;                ///< 文件描述符
    std::string host_;      ///< 主机地址
    int port_;              ///< 端口号
    int64_t lastUsed_;      ///< 最后使用时间 (秒)
};

/**
 * @class ConnectionPool
 * @brief 连接池 (线程安全版)
 *
 * 管理 Connection 对象的池:
 * - 预创建最小数量的连接
 * - 按需创建新连接，直到最大数量
 * - 自动清理空闲连接
 * - 线程安全
 *
 * 使用方法:
 * 1. 创建 ConnectionPool
 * 2. 调用 acquire() 获取连接
 * 3. 使用连接
 * 4. 调用 release() 归还连接
 * 5. 定期调用 healthCheck() 清理空闲连接
 */
class ConnectionPool {
public:
    /**
     * @brief 构造连接池
     * @param host 目标主机地址
     * @param port 目标端口
     * @param minSize 最小连接数 (预创建)
     * @param maxSize 最大连接数
     */
    ConnectionPool(const std::string& host, int port,
                   size_t minSize = 5, size_t maxSize = 20)
        : host_(host)
        , port_(port)
        , minSize_(minSize)
        , maxSize_(maxSize)
        , totalCreated_(0)
        , closed_(false)
    {
        // 预创建连接 (在锁外创建，避免阻塞)
        std::vector<Connection::Ptr> initialConns;
        for (size_t i = 0; i < minSize; ++i) {
            int fd = createConnection(host, port);
            if (fd >= 0) {
                initialConns.push_back(std::make_shared<Connection>(fd, host, port));
            }
        }

        // 一次性加入池中
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& conn : initialConns) {
            pool_.push(conn);
        }
        totalCreated_ = initialConns.size();
    }

    /**
     * @brief 析构函数
     *
     * 安全关闭，唤醒所有等待的线程
     */
    ~ConnectionPool() {
        // 安全关闭
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();

        // 清空连接
        std::lock_guard<std::mutex> lock(mutex_);
        while (!pool_.empty()) {
            pool_.pop();
        }
    }

    /**
     * @brief 获取连接
     * @param timeoutMs 超时时间 (毫秒)
     * @return 连接指针，nullptr 表示失败
     *
     * 优先复用现有连接，否则创建新连接
     */
    Connection::Ptr acquire(int timeoutMs = 5000) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 等待可用连接或池未满
        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return closed_ || !pool_.empty() || totalCreated_ < maxSize_; });

        if (closed_) return nullptr;

        // 复用现有连接
        if (!pool_.empty()) {
            auto conn = pool_.front();
            pool_.pop();
            conn->markUsed();
            return conn;
        }

        // 创建新连接 (在锁外执行，避免阻塞)
        if (totalCreated_ < maxSize_) {
            totalCreated_++;  // 先占位
            lock.unlock();

            int fd = createConnection(host_, port_);

            lock.lock();
            if (fd < 0) {
                totalCreated_--;  // 创建失败，回退
                return nullptr;
            }

            return std::make_shared<Connection>(fd, host_, port_);
        }

        return nullptr;
    }

    /**
     * @brief 归还连接
     * @param conn 连接指针
     *
     * 无效连接会被丢弃
     */
    void release(Connection::Ptr conn) {
        if (!conn) return;

        if (!conn->valid()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (totalCreated_ > 0) totalCreated_--;
            cv_.notify_one();
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (closed_ || pool_.size() >= maxSize_) {
            // 池已关闭或已满，丢弃连接
            if (totalCreated_ > 0) totalCreated_--;
        } else {
            pool_.push(conn);
        }

        cv_.notify_one();
    }

    /**
     * @brief 健康检查 (清理空闲连接)
     *
     * 清理超过 60 秒未使用的连接，但保留 minSize 个
     */
    void healthCheck() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (closed_) return;

        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        std::queue<Connection::Ptr> valid;

        while (!pool_.empty()) {
            auto conn = pool_.front();
            pool_.pop();

            // 清理超过 60 秒未使用的连接，但保留 minSize 个
            if (now - conn->lastUsed() > 60 && valid.size() >= minSize_) {
                if (totalCreated_ > 0) totalCreated_--;
            } else {
                valid.push(conn);
            }
        }

        pool_ = std::move(valid);
    }

    /// 获取可用连接数
    size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pool_.size();
    }

    /// 获取已创建的总连接数
    size_t totalCreated() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return totalCreated_;
    }

    /// 检查池是否已关闭
    bool isClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    /**
     * @brief 创建新连接
     * @param host 主机地址
     * @param port 端口号
     * @return 文件描述符，-1 表示失败
     *
     * 使用非阻塞 connect + select 超时
     */
    int createConnection(const std::string& host, int port) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        // 设置非阻塞
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        // 非阻塞 connect
        if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
            if (errno != EINPROGRESS) {
                close(fd);
                return -1;
            }

            // 等待连接完成 (带超时)
            fd_set writefds;
            FD_ZERO(&writefds);
            FD_SET(fd, &writefds);

            struct timeval tv;
            tv.tv_sec = 5;
            tv.tv_usec = 0;

            int result = select(fd + 1, nullptr, &writefds, nullptr, &tv);
            if (result <= 0) {
                close(fd);
                return -1;
            }

            // 检查连接是否成功
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
            if (error != 0) {
                close(fd);
                return -1;
            }
        }

        // 恢复阻塞模式
        fcntl(fd, F_SETFL, flags);

        return fd;
    }

    std::string host_;      ///< 目标主机
    int port_;              ///< 目标端口
    size_t minSize_;        ///< 最小连接数
    size_t maxSize_;        ///< 最大连接数
    size_t totalCreated_;   ///< 已创建连接总数
    bool closed_;           ///< 是否已关闭

    std::queue<Connection::Ptr> pool_;  ///< 空闲连接队列
    mutable std::mutex mutex_;          ///< 保护队列
    std::condition_variable cv_;        ///< 等待条件
};