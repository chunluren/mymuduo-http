// ConnectionPool.h - TCP 连接池
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

// 连接封装
class Connection {
public:
    using Ptr = std::shared_ptr<Connection>;
    
    Connection(int fd, const std::string& host, int port)
        : fd_(fd), host_(host), port_(port), lastUsed_(0)
    {}
    
    ~Connection() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    
    int fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int64_t lastUsed() const { return lastUsed_; }
    
    void markUsed() {
        lastUsed_ = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    
    // 发送数据
    ssize_t send(const void* buf, size_t len) {
        return ::send(fd_, buf, len, MSG_NOSIGNAL);
    }
    
    // 接收数据
    ssize_t recv(void* buf, size_t len) {
        return ::recv(fd_, buf, len, 0);
    }
    
    // 设置非阻塞
    void setNonBlocking() {
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }

private:
    int fd_;
    std::string host_;
    int port_;
    int64_t lastUsed_;
};

// 连接池
class ConnectionPool {
public:
    using CreateCallback = std::function<int(const std::string&, int)>;
    
    ConnectionPool(const std::string& host, int port,
                   size_t minSize = 5, size_t maxSize = 20)
        : host_(host)
        , port_(port)
        , minSize_(minSize)
        , maxSize_(maxSize)
        , totalCreated_(0)
        , running_(true)
    {
        // 预创建连接
        for (size_t i = 0; i < minSize; ++i) {
            int fd = createConnection(host, port);
            if (fd >= 0) {
                pool_.push(std::make_shared<Connection>(fd, host, port));
                totalCreated_++;
            }
        }
    }
    
    ~ConnectionPool() {
        running_ = false;
        while (!pool_.empty()) {
            pool_.pop();
        }
    }
    
    // 获取连接
    Connection::Ptr acquire(int timeoutMs = 5000) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // 等待可用连接
        if (cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return !pool_.empty() || totalCreated_ < maxSize_; })) {
            
            if (!pool_.empty()) {
                auto conn = pool_.front();
                pool_.pop();
                conn->markUsed();
                return conn;
            }
            
            // 创建新连接
            if (totalCreated_ < maxSize_) {
                int fd = createConnection(host_, port_);
                if (fd >= 0) {
                    totalCreated_++;
                    return std::make_shared<Connection>(fd, host_, port_);
                }
            }
        }
        
        return nullptr;
    }
    
    // 归还连接
    void release(Connection::Ptr conn) {
        if (!conn || !conn->valid()) {
            totalCreated_--;
            return;
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (running_ && pool_.size() < maxSize_) {
            pool_.push(conn);
        } else {
            totalCreated_--;
        }
        
        cv_.notify_one();
    }
    
    // 健康检查（清理空闲连接）
    void healthCheck() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        std::queue<Connection::Ptr> valid;
        
        while (!pool_.empty()) {
            auto conn = pool_.front();
            pool_.pop();
            
            // 清理超过 60 秒未使用的连接，但保留 minSize 个
            if (now - conn->lastUsed() > 60 && valid.size() >= minSize_) {
                totalCreated_--;
            } else {
                valid.push(conn);
            }
        }
        
        pool_ = std::move(valid);
    }
    
    size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pool_.size();
    }
    
    size_t totalCreated() const { return totalCreated_; }

private:
    int createConnection(const std::string& host, int port) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        
        if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
        
        return fd;
    }
    
    std::string host_;
    int port_;
    size_t minSize_;
    size_t maxSize_;
    std::atomic<size_t> totalCreated_;
    std::atomic<bool> running_;
    
    std::queue<Connection::Ptr> pool_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};