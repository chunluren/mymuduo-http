/**
 * @file HttpServer.h
 * @brief HTTP 服务器实现
 *
 * 本文件定义了 HttpServer 类，基于 TcpServer 实现了一个完整的 HTTP 服务器。
 * 支持:
 * - HTTP/1.1 协议
 * - GET、POST、PUT、DELETE 方法
 * - 路由注册和匹配
 * - 静态文件服务
 * - 中间件机制
 * - HTTP Keep-Alive
 * - HTTP Pipeline
 *
 * 实现说明：
 *   协议级逻辑（路由、请求解析、中间件、Gzip、静态文件）已抽取到 HttpCore，
 *   HttpServer 通过 **组合（composition）** 持有一个 HttpCore 实例，
 *   并把所有路由注册调用转发给它；本类自己只负责 TCP 传输层
 *   （TcpServer + onMessage 的粘包/流水线循环）。
 *   同一个 HttpCore 也被 HttpsServer 使用。
 *
 * @example 基本使用
 * @code
 * EventLoop loop;
 * InetAddress addr(8080);
 * HttpServer server(&loop, addr, "MyHttpServer");
 *
 * server.GET("/", [](const HttpRequest& req, HttpResponse& resp) {
 *     resp.setHtml("<h1>Hello World</h1>");
 * });
 *
 * server.serveStatic("/static", "/var/www/html");
 *
 * server.setThreadNum(4);
 * server.start();
 * loop.loop();
 * @endcode
 */

#pragma once

#include "HttpCore.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "net/TcpServer.h"
#include "net/EventLoop.h"
#include "net/Buffer.h"

#include <atomic>
#include <memory>
#include <string>

/**
 * @class HttpServer
 * @brief HTTP 服务器类
 *
 * 特点:
 * - 支持路由注册 (GET、POST、PUT、DELETE)
 * - 支持正则表达式路由匹配
 * - 支持静态文件服务
 * - 支持中间件
 * - 自动处理粘包和 HTTP Pipeline
 * - 请求体大小限制
 *
 * 线程模型:
 * - 使用 TcpServer，One Loop Per Thread 模型
 * - 可以配置多个 I/O 线程
 */
class HttpServer {
public:
    /// 最大请求体大小 (10MB)
    static constexpr size_t kMaxBodySize = HttpCore::kMaxBodySize;
    /// 最大请求行长度
    static constexpr size_t kMaxRequestLine = HttpCore::kMaxRequestLine;

    /**
     * @brief 构造 HTTP 服务器
     * @param loop 事件循环
     * @param addr 监听地址
     * @param name 服务器名称
     */
    HttpServer(EventLoop* loop, const InetAddress& addr,
               const std::string& name = "HttpServer")
        : server_(loop, addr, name)
        , loop_(loop)
        , started_(false)
        , idleTimeoutSec_(60.0)
    {
        server_.setConnectionCallback([this](const TcpConnectionPtr& conn) {
            onConnection(conn);
        });
        server_.setMessageCallback(
            [this](const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
                onMessage(conn, buf, time);
            });
        server_.setThreadNum(4);
    }

    /// 设置 I/O 线程数量
    void setThreadNum(int num) { server_.setThreadNum(num); }

    /// 设置空闲连接超时时间（秒，0 表示禁用）
    void setIdleTimeout(double seconds) { idleTimeoutSec_ = seconds; }

    /// 启动服务器（启动后不允许再注册路由）
    void start() {
        started_.store(true);
        server_.start();
    }

    /// 优雅关闭：停止接受新连接，timeoutSec 后强制退出事件循环
    void shutdown(double timeoutSec = 5.0) {
        started_.store(false);
        loop_->runAfter(timeoutSec, [this]() { loop_->quit(); });
    }

    // ==================== 路由注册（转发到 HttpCore） ====================

    void GET(const std::string& path, HttpHandler handler) {
        if (started_.load()) return;
        core_.GET(path, std::move(handler));
    }
    void POST(const std::string& path, HttpHandler handler) {
        if (started_.load()) return;
        core_.POST(path, std::move(handler));
    }
    void PUT(const std::string& path, HttpHandler handler) {
        if (started_.load()) return;
        core_.PUT(path, std::move(handler));
    }
    void DELETE(const std::string& path, HttpHandler handler) {
        if (started_.load()) return;
        core_.DELETE(path, std::move(handler));
    }

    void serveStatic(const std::string& urlPrefix, const std::string& dir) {
        if (started_.load()) return;
        core_.serveStatic(urlPrefix, dir);
    }

    void use(HttpHandler middleware) {
        if (started_.load()) return;
        core_.use(std::move(middleware));
    }

    void useRateLimit(int maxRequestsPerSec) {
        if (started_.load()) return;
        core_.useRateLimit(maxRequestsPerSec);
    }

    void enableGzip(size_t minSize = 1024) {
        if (started_.load()) return;
        core_.enableGzip(minSize);
    }

    void enableCors(const std::string& origin = "*") {
        if (started_.load()) return;
        core_.enableCors(origin);
    }

    void enableMetrics(const std::string& path = "/metrics") {
        if (started_.load()) return;
        core_.enableMetrics(path);
    }

private:
    TcpServer server_;                ///< 底层 TCP 服务器
    EventLoop* loop_;                 ///< 事件循环指针（用于优雅关闭）
    HttpCore core_;                   ///< HTTP 协议处理核心（路由 + 解析 + 中间件 + Gzip + 静态文件）
    std::atomic<bool> started_;       ///< 是否已启动
    double idleTimeoutSec_;           ///< 空闲连接超时（秒）

    /// 连接回调：设置空闲超时
    void onConnection(const TcpConnectionPtr& conn) {
        if (conn->connected() && idleTimeoutSec_ > 0) {
            auto weakConn = std::weak_ptr<TcpConnection>(conn);
            conn->getLoop()->runAfter(idleTimeoutSec_, [weakConn]() {
                auto c = weakConn.lock();
                if (c && c->connected()) {
                    c->shutdown();
                }
            });
        }
    }

    /// 消息回调：处理粘包 / HTTP pipeline
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp /*time*/) {
        while (buf->readableBytes() > 0) {
            HttpRequest request;
            HttpCore::ParseResult result = core_.parseRequest(buf, request);

            if (result == HttpCore::ParseResult::Incomplete) {
                return;  // 等待更多数据
            }
            if (result == HttpCore::ParseResult::Error) {
                HttpResponse resp = HttpResponse::badRequest("Bad Request");
                resp.closeConnection = true;
                conn->send(resp.toString());
                conn->shutdown();
                return;
            }

            // 让 handler 能在 deferred 模式下从 req.connection() 拿到 conn
            request.setConnection(conn);

            HttpResponse response;
            core_.handleRequest(request, response);
            core_.postProcessResponse(request, response);

            // Deferred：handler 已把工作丢给后台线程，发送由它自己完成。
            // HttpServer 退出 onMessage，不发、不 shutdown、不 pipeline。
            // 后续 pipeline 数据会在 conn 下次有可读事件时再触发 onMessage。
            if (response.deferred) return;

            response.closeConnection = !request.keepAlive();
            // 三条路径：
            //   - file-body：header 走 send，body 走 sendFile（zero-copy）
            //   - chunked：toString 整体送（chunks 拼合需要）
            //   - 普通：writev header + body
            if (response.hasFileBody()) {
                conn->send(response.toHeader());
                conn->sendFile(response.bodyFileFd(), 0, response.bodyFileSize());
            } else if (response.canSendIov()) {
                std::string header = response.toHeader();
                TcpConnection::IoSlice slices[2] = {
                    { header.data(), header.size() },
                    { response.body.data(), response.body.size() },
                };
                conn->sendIov(slices, 2);
            } else {
                conn->send(response.toString());
            }

            if (response.closeConnection) {
                conn->shutdown();
                return;
            }
            // 继续处理下一个流水线请求
        }
    }
};
