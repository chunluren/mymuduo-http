// RpcClientPb.h - Protobuf RPC 客户端
#pragma once

#include "rpc.pb.h"
#include <google/protobuf/message.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <future>
#include <atomic>

// 最大帧长度限制 (64MB)
constexpr int32_t RPC_MAX_FRAME_LENGTH = 64 * 1024 * 1024;

class RpcClientPb {
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
        if (send(sock_, &len, 4, 0) != 4) {
            connected_ = false;
            return false;
        }
        if (send(sock_, data.c_str(), data.size(), 0) != (ssize_t)data.size()) {
            connected_ = false;
            return false;
        }

        // 接收响应
        len = 0;
        if (recv(sock_, &len, 4, 0) != 4) {
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
        if (recv(sock_, &respData[0], len, 0) != len) {
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
    
    // 异步调用 - 按值捕获请求，避免悬空引用
    // 注意：调用者必须确保 response 的生命周期超过 future.get()
    template<typename T1, typename T2>
    std::future<bool> asyncCall(const std::string& service, const std::string& method,
                                 const T1& request, T2& response) {
        // 按值捕获请求（拷贝），避免悬空引用
        // 注意：response 仍为引用，调用者需确保生命周期
        return std::async(std::launch::async, [this, service, method, request, &response]() {
            return call<T1, T2>(service, method, request, response);
        });
    }

private:
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