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

class RpcClientPb {
public:
    RpcClientPb(const std::string& host, int port)
        : host_(host), port_(port), nextId_(1), connected_(false)
    {}
    
    ~RpcClientPb() {
        disconnect();
    }
    
    // 连接
    bool connect() {
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) return false;
        
        hostent* host = gethostbyname(host_.c_str());
        if (!host) {
            close(sock_);
            return false;
        }
        
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr = *((in_addr*)host->h_addr);
        
        if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock_);
            return false;
        }
        
        connected_ = true;
        return true;
    }
    
    void disconnect() {
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
        if (!connected_ && !connect()) {
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
    
    // 异步调用
    template<typename T1, typename T2>
    std::future<bool> asyncCall(const std::string& service, const std::string& method,
                                 const T1& request, T2& response) {
        return std::async(std::launch::async, [this, service, method, &request, &response]() {
            return call<T1, T2>(service, method, request, response);
        });
    }

private:
    std::string host_;
    int port_;
    int sock_ = -1;
    int64_t nextId_;
    bool connected_;
};