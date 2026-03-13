// RpcServer.h - RPC 服务端
#pragma once

#include "../http/HttpServer.h"
#include <unordered_map>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// RPC 方法类型
using RpcMethod = std::function<json(const json&)>;

class RpcServer {
public:
    RpcServer(EventLoop* loop, const InetAddress& addr)
        : httpServer_(loop, addr, "RpcServer")
    {
        // 注册 RPC 端点
        httpServer_.POST("/rpc", [this](const HttpRequest& req, HttpResponse& resp) {
            handleRpcCall(req, resp);
        });
    }
    
    void setThreadNum(int num) { httpServer_.setThreadNum(num); }
    void start() { httpServer_.start(); }
    
    // 注册 RPC 方法
    void registerMethod(const std::string& name, RpcMethod method) {
        methods_[name] = method;
    }
    
    // 批量注册
    template<typename T>
    void registerService(T* service) {
        service->registerMethods(this);
    }

private:
    HttpServer httpServer_;
    std::unordered_map<std::string, RpcMethod> methods_;
    
    void handleRpcCall(const HttpRequest& req, HttpResponse& resp) {
        try {
            // 解析请求
            json request = json::parse(req.body);
            
            std::string method = request["method"];
            json params = request["params"];
            int id = request.value("id", 0);
            
            // 查找方法
            auto it = methods_.find(method);
            if (it == methods_.end()) {
                resp.json(makeError(id, -32601, "Method not found"));
                return;
            }
            
            // 调用方法
            json result = it->second(params);
            
            // 返回结果
            resp.json(makeResponse(id, result));
            
        } catch (const json::parse_error& e) {
            resp.json(makeError(0, -32700, "Parse error"));
        } catch (const std::exception& e) {
            resp.json(makeError(0, -32603, "Internal error"));
        }
    }
    
    std::string makeResponse(int id, const json& result) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["result"] = result;
        return resp.dump();
    }
    
    std::string makeError(int id, int code, const std::string& message) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["error"]["code"] = code;
        resp["error"]["message"] = message;
        return resp.dump();
    }
};