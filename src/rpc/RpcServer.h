/**
 * @file RpcServer.h
 * @brief JSON RPC 服务端
 *
 * 本文件定义了 RpcServer 类，基于 HttpServer 实现 JSON-RPC 2.0 服务端。
 *
 * JSON-RPC 2.0 协议:
 * - 请求格式: {"jsonrpc":"2.0","method":"方法名","params":参数,"id":1}
 * - 响应格式: {"jsonrpc":"2.0","result":结果,"id":1}
 * - 错误格式: {"jsonrpc":"2.0","error":{"code":-32601,"message":"Method not found"},"id":1}
 *
 * @example 使用示例
 * @code
 * EventLoop loop;
 * InetAddress addr(8080);
 * RpcServer server(&loop, addr);
 *
 * // 注册 RPC 方法
 * server.registerMethod("add", [](const json& params) -> json {
 *     int a = params[0].get<int>();
 *     int b = params[1].get<int>();
 *     return a + b;
 * });
 *
 * server.setThreadNum(4);
 * server.start();
 * loop.loop();
 * @endcode
 */

#pragma once

#include "http/HttpServer.h"
#include <unordered_map>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/// RPC 方法类型: 接收 json 参数，返回 json 结果
using RpcMethod = std::function<json(const json&)>;

/**
 * @class RpcServer
 * @brief JSON RPC 服务端
 *
 * 基于 HttpServer 实现，监听 /rpc 端点处理 RPC 调用
 */
class RpcServer {
public:
    /**
     * @brief 构造 RPC 服务器
     * @param loop 事件循环
     * @param addr 监听地址
     */
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

    /**
     * @brief 注册 RPC 方法
     * @param name 方法名称
     * @param method 方法处理函数
     *
     * @example
     * @code
     * server.registerMethod("getUser", [](const json& params) {
     *     int userId = params["id"].get<int>();
     *     return {{"name", "Alice"}, {"age", 25}};
     * });
     * @endcode
     */
    void registerMethod(const std::string& name, RpcMethod method) {
        methods_[name] = method;
    }

    /**
     * @brief 批量注册服务方法
     * @param service 服务对象指针
     *
     * 服务类需要实现 registerMethods(RpcServer*) 方法
     */
    template<typename T>
    void registerService(T* service) {
        service->registerMethods(this);
    }

private:
    HttpServer httpServer_;
    std::unordered_map<std::string, RpcMethod> methods_;

    /**
     * @brief 处理 RPC 调用
     * @param req HTTP 请求
     * @param resp HTTP 响应
     */
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

    /// 构造成功响应
    std::string makeResponse(int id, const json& result) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["result"] = result;
        return resp.dump();
    }

    /// 构造错误响应
    std::string makeError(int id, int code, const std::string& message) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["error"]["code"] = code;
        resp["error"]["message"] = message;
        return resp.dump();
    }
};