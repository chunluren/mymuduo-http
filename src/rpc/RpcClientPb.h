// RpcClientPb.h - Protobuf RPC 客户端
#pragma once

#include "rpc.pb.h"
#include <google/protobuf/message.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <cerrno>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <future>
#include <atomic>

// 最大帧长度限制 (64MB)
constexpr int32_t RPC_MAX_FRAME_LENGTH = 64 * 1024 * 1024;

class RpcClientPb : public std::enable_shared_from_this<RpcClientPb> {
public:
    RpcClientPb(const std::string& host, int port)
        : host_(host), port_(port), sock_(-1), nextId_(1), connected_(false)
    {}
    
    ~RpcClientPb() {
        disconnect();
    }
    
    // 连接
    bool connect() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_) return true;

        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) return false;

        hostent* host = gethostbyname(host_.c_str());
        if (!host) {
            close(sock_);
            sock_ = -1;
            return false;
        }

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr = *((in_addr*)host->h_addr);

        if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock_);
            sock_ = -1;
            return false;
        }

        connected_ = true;
        return true;
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sock_ >= 0) {
            close(sock_);
            sock_ = -1;
        }
        connected_ = false;
    }
    
    // 同步调用
    template<typename T1, typename T2>
    bool call(const std::string& service, const std::string& method,
              const T1& request, T2& response) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_ && !connectInternal()) {
            return false;
        }

        // 构造请求
        rpc::RpcRequest req;
        req.set_service(service);
        req.set_method(method);
        req.set_id(nextId_++);
        request.SerializeToString(req.mutable_params());

        // 序列化
        std::string data;
        req.SerializeToString(&data);

        // 发送: 长度 + 数据
        int32_t len = htonl(data.size());
        if (!sendAll(&len, 4)) {
            connected_ = false;
            return false;
        }
        if (!sendAll(data.c_str(), data.size())) {
            connected_ = false;
            return false;
        }

        // 接收响应
        len = 0;
        if (!recvAll(&len, 4)) {
            connected_ = false;
            return false;
        }
        len = ntohl(len);

        // 校验帧长度，防止恶意数据
        if (len <= 0 || len > RPC_MAX_FRAME_LENGTH) {
            connected_ = false;
            return false;
        }

        std::string respData(len, '\0');
        if (!recvAll(&respData[0], len)) {
            connected_ = false;
            return false;
        }

        // 解析响应
        rpc::RpcResponse resp;
        if (!resp.ParseFromString(respData)) {
            return false;
        }

        if (resp.code() != 0) {
            return false;
        }

        return response.ParseFromString(resp.result());
    }
    
    // 异步调用 - 使用 shared_ptr 安全传递响应对象
    // 返回 future<bool> 表示调用是否成功
    template<typename T1, typename T2>
    std::future<bool> asyncCall(const std::string& service, const std::string& method,
                                 const T1& request, T2& response) {
        // 使用 shared_from_this() 延长对象生命周期
        auto self = shared_from_this();
        // 使用 shared_ptr 捕获 response，避免悬空引用
        auto responsePtr = std::shared_ptr<T2>(&response, [](T2*) {});  // 非所有权指针

        return std::async(std::launch::async, [self, service, method, request, responsePtr]() {
            return self->call<T1, T2>(service, method, request, *responsePtr);
        });
    }

private:
    // 完整发送所有数据（处理短写和 EINTR）
    bool sendAll(const void* buf, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = send(sock_, static_cast<const char*>(buf) + sent, len - sent, 0);
            if (n < 0) {
                // EINTR 表示被信号中断，需要重试
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) return false;
            sent += n;
        }
        return true;
    }

    // 完整接收所有数据（处理短读和 EINTR）
    bool recvAll(void* buf, size_t len) {
        size_t received = 0;
        while (received < len) {
            ssize_t n = recv(sock_, static_cast<char*>(buf) + received, len - received, 0);
            if (n < 0) {
                // EINTR 表示被信号中断，需要重试
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) return false;
            received += n;
        }
        return true;
    }

    // 内部连接方法（不加锁，由调用者保证线程安全）
    bool connectInternal() {
        if (connected_) return true;

        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) return false;

        hostent* host = gethostbyname(host_.c_str());
        if (!host) {
            close(sock_);
            sock_ = -1;
            return false;
        }

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr = *((in_addr*)host->h_addr);

        if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock_);
            sock_ = -1;
            return false;
        }

        connected_ = true;
        return true;
    }

    std::string host_;
    int port_;
    int sock_;
    std::atomic<int64_t> nextId_{1};
    bool connected_;
    mutable std::mutex mutex_;  // 保护 sock_, connected_ 等状态
};