// RpcServerPb.h - Protobuf RPC 服务端
#pragma once

#include "../TcpServer.h"
#include "../Buffer.h"
#include "proto/rpc.pb.h"
#include <google/protobuf/message.h>
#include <unordered_map>
#include <functional>
#include <memory>

// Protobuf 方法类型
using PbMethod = std::function<void(const google::protobuf::Message&, google::protobuf::Message&)>;

// Protobuf RPC 服务端
class RpcServerPb {
public:
    RpcServerPb(EventLoop* loop, const InetAddress& addr)
        : server_(loop, addr, "RpcServerPb")
    {
        server_.setConnectionCallback([this](const TcpConnectionPtr& conn) {});
        server_.setMessageCallback([this](const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
            onMessage(conn, buf, time);
        });
        server_.setThreadNum(4);
    }
    
    void setThreadNum(int num) { server_.setThreadNum(num); }
    void start() { server_.start(); }
    
    // 注册方法
    // T1: 请求类型, T2: 响应类型
    template<typename T1, typename T2>
    void registerMethod(const std::string& serviceName, const std::string& methodName,
                        std::function<void(const T1&, T2&)> handler) {
        std::string key = serviceName + "." + methodName;
        methods_[key] = [handler](const google::protobuf::Message& req, google::protobuf::Message& resp) {
            handler(static_cast<const T1&>(req), static_cast<T2&>(resp));
        };
        requestCreators_[key] = []() -> std::unique_ptr<google::protobuf::Message> {
            return std::make_unique<T1>();
        };
        responseCreators_[key] = []() -> std::unique_ptr<google::protobuf::Message> {
            return std::make_unique<T2>();
        };
    }

private:
    TcpServer server_;
    std::unordered_map<std::string, PbMethod> methods_;
    std::unordered_map<std::string, std::function<std::unique_ptr<google::protobuf::Message>()>> requestCreators_;
    std::unordered_map<std::string, std::function<std::unique_ptr<google::protobuf::Message>()>> responseCreators_;
    
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
        // 读取消息长度 (4 bytes)
        if (buf->readableBytes() < 4) return;
        
        int32_t len = 0;
        memcpy(&len, buf->peek(), 4);
        len = ntohl(len);
        
        if (buf->readableBytes() < 4 + len) return;
        
        buf->retrieve(4);
        std::string data(buf->peek(), len);
        buf->retrieve(len);
        
        // 解析请求
        rpc::RpcRequest request;
        if (!request.ParseFromString(data)) {
            sendError(conn, 0, -32700, "Parse error");
            return;
        }
        
        // 查找方法
        std::string key = request.service() + "." + request.method();
        auto methodIt = methods_.find(key);
        auto reqIt = requestCreators_.find(key);
        auto respIt = responseCreators_.find(key);
        
        if (methodIt == methods_.end()) {
            sendError(conn, request.id(), -32601, "Method not found: " + key);
            return;
        }
        
        // 创建请求和响应对象
        auto reqMsg = reqIt->second();
        auto respMsg = respIt->second();
        
        // 解析参数
        if (!reqMsg->ParseFromString(request.params())) {
            sendError(conn, request.id(), -32602, "Invalid params");
            return;
        }
        
        // 调用方法
        try {
            methodIt->second(*reqMsg, *respMsg);
        } catch (const std::exception& e) {
            sendError(conn, request.id(), -32603, e.what());
            return;
        }
        
        // 发送响应
        sendResponse(conn, request.id(), *respMsg);
    }
    
    void sendResponse(const TcpConnectionPtr& conn, int64_t id, const google::protobuf::Message& result) {
        rpc::RpcResponse response;
        response.set_id(id);
        response.set_code(0);
        result.SerializeToString(response.mutable_result());
        
        std::string data;
        response.SerializeToString(&data);
        
        // 发送: 长度 + 数据
        int32_t len = htonl(data.size());
        conn->send(std::string((char*)&len, 4) + data);
    }
    
    void sendError(const TcpConnectionPtr& conn, int64_t id, int code, const std::string& message) {
        rpc::RpcResponse response;
        response.set_id(id);
        response.set_code(code);
        response.set_message(message);
        
        std::string data;
        response.SerializeToString(&data);
        
        int32_t len = htonl(data.size());
        conn->send(std::string((char*)&len, 4) + data);
    }
};