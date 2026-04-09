/**
 * @file RpcClient.h
 * @brief JSON RPC 客户端
 *
 * 本文件定义了 RpcClient 类，实现 JSON-RPC 2.0 客户端。
 * 支持同步和异步调用。
 *
 * @example 使用示例
 * @code
 * RpcClient client("127.0.0.1", 8080);
 *
 * // 同步调用
 * json params = {1, 2};
 * json result = client.call("add", params);
 * std::cout << "Result: " << result << std::endl;
 *
 * // 异步调用
 * auto future = client.asyncCall("add", params);
 * // ... 做其他事情 ...
 * json result = future.get();
 * @endcode
 */

#pragma once

#include <string>
#include <unordered_map>
#include <atomic>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <future>

using json = nlohmann::json;

/**
 * @class RpcClient
 * @brief JSON RPC 客户端
 *
 * 提供 JSON-RPC 2.0 客户端功能:
 * - 同步调用 (call)
 * - 异步调用 (asyncCall)
 *
 * 使用 HTTP 作为传输协议
 */
class RpcClient {
public:
    /**
     * @brief 构造 RPC 客户端
     * @param host 服务器主机名或 IP
     * @param port 服务器端口
     */
    RpcClient(const std::string& host, int port)
        : host_(host), port_(port), nextId_(1)
    {}

    /**
     * @brief 同步调用 RPC 方法
     * @param method 方法名称
     * @param params 参数 (可选)
     * @return 调用结果，失败时返回 {"error", "错误信息"}
     *
     * @example
     * @code
     * json result = client.call("add", {1, 2});
     * if (result.contains("error")) {
     *     std::cerr << "Error: " << result["error"] << std::endl;
     * } else {
     *     std::cout << "Result: " << result << std::endl;
     * }
     * @endcode
     */
    json call(const std::string& method, const json& params = json()) {
        int sock = connect();
        if (sock < 0) {
            return {{"error", "connection failed"}};
        }

        // 构造请求
        json request;
        request["jsonrpc"] = "2.0";
        request["method"] = method;
        request["params"] = params;
        request["id"] = nextId_++;

        // 发送 HTTP 请求
        std::string body = request.dump();
        std::string httpReq = "POST /rpc HTTP/1.1\r\n"
                             "Host: " + host_ + "\r\n"
                             "Content-Type: application/json\r\n"
                             "Content-Length: " + std::to_string(body.size()) + "\r\n"
                             "\r\n" + body;

        send(sock, httpReq.c_str(), httpReq.size(), 0);

        // 接收响应
        char buf[4096];
        std::string response;
        ssize_t n;
        while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[n] = '\0';
            response += buf;
        }

        close(sock);

        // 解析 HTTP 响应
        size_t bodyStart = response.find("\r\n\r\n");
        if (bodyStart == std::string::npos) {
            return {{"error", "invalid response"}};
        }

        std::string respBody = response.substr(bodyStart + 4);

        json resp;
        try {
            resp = json::parse(respBody);
        } catch (const json::parse_error& e) {
            return {{"error", "JSON parse error"}};
        }

        if (resp.contains("error")) {
            return resp["error"];
        }

        return resp["result"];
    }

    /**
     * @brief 异步调用 RPC 方法
     * @param method 方法名称
     * @param params 参数 (可选)
     * @return future<json>，可通过 get() 获取结果
     *
     * @example
     * @code
     * auto future = client.asyncCall("add", {1, 2});
     * // ... 做其他事情 ...
     * json result = future.get();
     * @endcode
     */
    std::future<json> asyncCall(const std::string& method, const json& params = json()) {
        return std::async(std::launch::async, [this, method, params]() {
            return call(method, params);
        });
    }

private:
    std::string host_;
    int port_;
    std::atomic<int> nextId_;

    /**
     * @brief 建立 TCP 连接
     * @return socket fd，-1 表示失败
     */
    int connect() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;

        // 使用 getaddrinfo 替代废弃的 gethostbyname（线程安全）
        addrinfo hints = {};
        hints.ai_family = AF_INET;  // IPv4
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* result = nullptr;
        int ret = getaddrinfo(host_.c_str(),
                              std::to_string(port_).c_str(),
                              &hints, &result);
        if (ret != 0 || !result) {
            close(sock);
            return -1;
        }

        // 使用第一个地址
        if (::connect(sock, result->ai_addr, result->ai_addrlen) < 0) {
            freeaddrinfo(result);
            close(sock);
            return -1;
        }

        freeaddrinfo(result);
        return sock;
    }
};

// ==================== Reactor 版 RPC 客户端 ====================

#include "net/TcpClient.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "net/Buffer.h"
#include "net/Callbacks.h"

#include <condition_variable>

/**
 * @class ReactorRpcClient
 * @brief 基于 Reactor 架构的 JSON-RPC 2.0 客户端
 *
 * 与 RpcClient 的区别:
 * - 使用 TcpClient 维持长连接（不需要每次建连）
 * - 非阻塞 I/O，基于 EventLoop
 * - 支持自动重连
 *
 * @example
 * @code
 * EventLoop loop;
 * ReactorRpcClient client(&loop, InetAddress("127.0.0.1", 8080), "RpcClient");
 * client.connect();
 *
 * // 同步调用（内部等待响应）
 * json result = client.call("add", {1, 2});
 *
 * // 异步调用
 * auto future = client.asyncCall("multiply", {3, 4});
 * json result2 = future.get();
 * @endcode
 */
class ReactorRpcClient {
public:
    ReactorRpcClient(EventLoop* loop, const InetAddress& serverAddr,
                     const std::string& name)
        : client_(loop, serverAddr, name)
        , nextId_(1)
    {
        client_.setConnectionCallback(
            [this](const TcpConnectionPtr& conn) {
                if (conn->connected()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    conn_ = conn;
                    connCv_.notify_all();
                } else {
                    std::lock_guard<std::mutex> lock(mutex_);
                    conn_.reset();
                }
            });

        client_.setMessageCallback(
            [this](const TcpConnectionPtr& /*conn*/, Buffer* buf, Timestamp /*time*/) {
                std::string data = buf->retrieveAllAsString();
                // 解析 HTTP 响应中的 JSON body
                size_t bodyStart = data.find("\r\n\r\n");
                if (bodyStart == std::string::npos) return;
                std::string respBody = data.substr(bodyStart + 4);

                try {
                    json resp = json::parse(respBody);
                    int id = resp.value("id", -1);
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = pendingCalls_.find(id);
                    if (it != pendingCalls_.end()) {
                        if (resp.contains("error")) {
                            it->second.set_value(resp["error"]);
                        } else {
                            it->second.set_value(resp["result"]);
                        }
                        pendingCalls_.erase(it);
                    }
                } catch (...) {
                    // JSON 解析失败，忽略
                }
            });

        client_.enableRetry();
    }

    void connect() { client_.connect(); }
    void disconnect() { client_.disconnect(); }

    /**
     * @brief 同步调用 RPC 方法
     * @param method 方法名
     * @param params 参数
     * @param timeoutMs 超时时间（毫秒），默认 5000
     * @return 调用结果
     */
    json call(const std::string& method, const json& params = json(),
              int timeoutMs = 5000) {
        auto future = asyncCall(method, params);
        if (future.wait_for(std::chrono::milliseconds(timeoutMs)) ==
            std::future_status::timeout) {
            return {{"error", "RPC call timeout"}};
        }
        return future.get();
    }

    /**
     * @brief 异步调用 RPC 方法
     * @param method 方法名
     * @param params 参数
     * @return future<json>
     */
    std::future<json> asyncCall(const std::string& method, const json& params = json()) {
        int id = nextId_++;
        std::promise<json> promise;
        auto future = promise.get_future();

        // 构造 JSON-RPC 请求
        json request;
        request["jsonrpc"] = "2.0";
        request["method"] = method;
        request["params"] = params;
        request["id"] = id;

        std::string body = request.dump();
        std::string httpReq = "POST /rpc HTTP/1.1\r\n"
                             "Content-Type: application/json\r\n"
                             "Content-Length: " + std::to_string(body.size()) + "\r\n"
                             "Connection: keep-alive\r\n"
                             "\r\n" + body;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingCalls_[id] = std::move(promise);
        }

        // 等待连接建立
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!conn_) {
                connCv_.wait_for(lock, std::chrono::milliseconds(3000),
                                [this] { return conn_ != nullptr; });
            }
            if (conn_) {
                conn_->send(httpReq);
            } else {
                auto it = pendingCalls_.find(id);
                if (it != pendingCalls_.end()) {
                    it->second.set_value({{"error", "not connected"}});
                    pendingCalls_.erase(it);
                }
            }
        }

        return future;
    }

private:
    TcpClient client_;
    std::atomic<int> nextId_;

    mutable std::mutex mutex_;
    std::condition_variable connCv_;
    TcpConnectionPtr conn_;
    std::unordered_map<int, std::promise<json>> pendingCalls_;
};