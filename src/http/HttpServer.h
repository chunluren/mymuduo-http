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
 * @example 基本使用
 * @code
 * EventLoop loop;
 * InetAddress addr(8080);
 * HttpServer server(&loop, addr, "MyHttpServer");
 *
 * // 注册路由
 * server.GET("/", [](const HttpRequest& req, HttpResponse& resp) {
 *     resp.setHtml("<h1>Hello World</h1>");
 * });
 *
 * server.GET("/api/users", [](const HttpRequest& req, HttpResponse& resp) {
 *     resp.json(R"([{"id":1,"name":"Alice"}])");
 * });
 *
 * server.POST("/api/users", [](const HttpRequest& req, HttpResponse& resp) {
 *     // 处理 POST 请求
 *     resp.setStatusCode(HttpStatusCode::CREATED);
 *     resp.json(R"({"success":true})");
 * });
 *
 * // 静态文件服务
 * server.serveStatic("/static", "/var/www/html");
 *
 * server.setThreadNum(4);
 * server.start();
 * loop.loop();
 * @endcode
 */

#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "GzipMiddleware.h"
#include "util/RateLimiter.h"
#include "util/Metrics.h"
#include "net/TcpServer.h"
#include "net/EventLoop.h"
#include "net/Buffer.h"
#include <functional>
#include <unordered_map>
#include <regex>
#include <atomic>
#include <shared_mutex>
#include <climits>
#include <memory>

/// 请求处理函数类型
using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

/**
 * @struct Route
 * @brief 路由项结构体
 *
 * 存储一个路由的所有信息:
 * - method: HTTP 方法
 * - pattern: URL 路径模式
 * - handler: 处理函数
 * - regex: 用于匹配的正则表达式
 */
struct Route {
    HttpMethod method;       ///< HTTP 方法
    std::string pattern;     ///< URL 路径模式
    HttpHandler handler;     ///< 处理函数
    std::regex regex;        ///< 正则表达式对象

    /**
     * @brief 构造路由项
     * @param m HTTP 方法
     * @param p URL 路径模式
     * @param h 处理函数
     */
    Route(HttpMethod m, const std::string& p, HttpHandler h)
        : method(m), pattern(p), handler(h), regex(p) {}
};

/**
 * @class HttpServer
 * @brief HTTP 服务器类
 *
 * HttpServer 是一个功能完整的 HTTP 服务器实现，特点:
 * - 支持路由注册 (GET、POST、PUT、DELETE)
 * - 支持正则表达式路由匹配
 * - 支持静态文件服务
 * - 支持中间件
 * - 自动处理粘包和 HTTP Pipeline
 * - 请求体大小限制
 *
 * 线程模型:
 * - 继承自 TcpServer，使用 One Loop Per Thread 模型
 * - 可以配置多个 I/O 线程
 */
class HttpServer {
public:
    /// 最大请求体大小 (10MB)
    static constexpr size_t kMaxBodySize = 10 * 1024 * 1024;
    /// 最大请求行长度
    static constexpr size_t kMaxRequestLine = 8192;

    /**
     * @brief 构造 HTTP 服务器
     * @param loop 事件循环
     * @param addr 监听地址
     * @param name 服务器名称
     *
     * 自动设置 TcpServer 的连接回调和消息回调
     */
    HttpServer(EventLoop* loop, const InetAddress& addr, const std::string& name = "HttpServer")
        : server_(loop, addr, name)
        , loop_(loop)
        , started_(false)
        , idleTimeoutSec_(60.0)
    {
        // 设置连接回调
        server_.setConnectionCallback([this](const TcpConnectionPtr& conn) {
            onConnection(conn);
        });
        // 设置消息回调
        server_.setMessageCallback([this](const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
            onMessage(conn, buf, time);
        });
        server_.setThreadNum(4);
    }

    /**
     * @brief 设置 I/O 线程数量
     * @param num 线程数量
     */
    void setThreadNum(int num) { server_.setThreadNum(num); }

    /**
     * @brief 设置空闲连接超时时间
     * @param seconds 超时秒数（0 表示禁用）
     */
    void setIdleTimeout(double seconds) { idleTimeoutSec_ = seconds; }

    /**
     * @brief 启动服务器
     *
     * 启动后不允许再注册路由或中间件
     */
    void start() {
        started_.store(true);
        server_.start();
    }

    /**
     * @brief 优雅关闭
     *
     * 停止接受新连接，等待 timeoutSec 秒后强制关闭所有连接并退出事件循环。
     *
     * @param timeoutSec 超时时间（秒），超时后强制退出事件循环
     */
    void shutdown(double timeoutSec = 5.0) {
        started_.store(false);
        // Use the event loop to schedule force shutdown after timeout
        loop_->runAfter(timeoutSec, [this]() {
            loop_->quit();
        });
    }

    /**
     * @brief 注册 GET 路由
     * @param path URL 路径 (支持正则表达式)
     * @param handler 处理函数
     *
     * @note 必须在 start() 之前调用
     *
     * @example
     * @code
     * server.GET("/users", [](const HttpRequest& req, HttpResponse& resp) {
     *     resp.json(R"([{"id":1,"name":"Alice"}])");
     * });
     *
     * // 正则路由
     * server.GET("/users/([0-9]+)", [](const HttpRequest& req, HttpResponse& resp) {
     *     // 匹配 /users/123 等
     * });
     * @endcode
     */
    void GET(const std::string& path, HttpHandler handler) {
        if (started_.load()) return;  // 启动后不允许注册
        std::unique_lock<std::shared_mutex> lock(routesMutex_);
        routes_.push_back({HttpMethod::GET, path, handler});
    }

    /**
     * @brief 注册 POST 路由
     * @param path URL 路径
     * @param handler 处理函数
     */
    void POST(const std::string& path, HttpHandler handler) {
        if (started_.load()) return;
        std::unique_lock<std::shared_mutex> lock(routesMutex_);
        routes_.push_back({HttpMethod::POST, path, handler});
    }

    /**
     * @brief 注册 PUT 路由
     * @param path URL 路径
     * @param handler 处理函数
     */
    void PUT(const std::string& path, HttpHandler handler) {
        if (started_.load()) return;
        std::unique_lock<std::shared_mutex> lock(routesMutex_);
        routes_.push_back({HttpMethod::PUT, path, handler});
    }

    /**
     * @brief 注册 DELETE 路由
     * @param path URL 路径
     * @param handler 处理函数
     */
    void DELETE(const std::string& path, HttpHandler handler) {
        if (started_.load()) return;
        std::unique_lock<std::shared_mutex> lock(routesMutex_);
        routes_.push_back({HttpMethod::DELETE, path, handler});
    }

    /**
     * @brief 配置静态文件服务
     * @param urlPrefix URL 前缀
     * @param dir 本地目录路径
     *
     * @example
     * @code
     * // 访问 /static/style.css 会返回 /var/www/html/style.css
     * server.serveStatic("/static", "/var/www/html");
     * @endcode
     */
    void serveStatic(const std::string& urlPrefix, const std::string& dir) {
        if (started_.load()) return;
        std::unique_lock<std::shared_mutex> lock(routesMutex_);
        staticDirs_[urlPrefix] = dir;
    }

    /**
     * @brief 添加中间件
     * @param middleware 中间件函数
     *
     * 中间件会在路由处理之前执行，可以用于:
     * - 日志记录
     * - 认证授权
     * - 请求修改
     *
     * @example
     * @code
     * server.use([](const HttpRequest& req, HttpResponse& resp) {
     *     // 记录请求日志
     *     LOG_INFO << req.method << " " << req.path;
     * });
     * @endcode
     */
    void use(HttpHandler middleware) {
        if (started_.load()) return;
        std::unique_lock<std::shared_mutex> lock(routesMutex_);
        middlewares_.push_back(middleware);
    }

    /**
     * @brief 启用请求频率限制
     * @param maxRequestsPerSec 每秒最大请求数（同时作为令牌桶容量）
     *
     * 基于客户端 IP 的令牌桶限流，优先从 X-Real-IP / X-Forwarded-For 获取 IP。
     * 超限返回 429 Too Many Requests。
     *
     * @note 必须在 start() 之前调用
     *
     * @example
     * @code
     * server.useRateLimit(100);  // 每个 IP 最多 100 请求/秒
     * @endcode
     */
    void useRateLimit(int maxRequestsPerSec) {
        if (started_.load()) return;
        auto limiter = std::make_shared<TokenBucketLimiter>(
            maxRequestsPerSec, maxRequestsPerSec);
        use([limiter](const HttpRequest& req, HttpResponse& resp) {
            std::string ip = req.getHeader("x-real-ip");
            if (ip.empty()) ip = req.getHeader("x-forwarded-for");
            if (ip.empty()) ip = "unknown";
            if (!limiter->allow(ip)) {
                resp.setStatusCode(HttpStatusCode::TOO_MANY_REQUESTS);
                resp.setText("Rate limit exceeded");
            }
        });
    }

    /**
     * @brief 启用 Gzip 压缩
     * @param minSize 最小压缩大小（字节），响应体小于此值不压缩
     *
     * 启用后，对满足条件的响应自动进行 Gzip 压缩:
     * - 客户端发送 Accept-Encoding: gzip
     * - Content-Type 为可压缩类型（text/html, application/json 等）
     * - 响应体大小 >= minSize
     *
     * @note 必须在 start() 之前调用
     */
    void enableGzip(size_t minSize = 1024) {
        if (started_.load()) return;
        gzipEnabled_ = true;
        gzipMinSize_ = minSize;
    }

    /**
     * @brief 启用 CORS 支持
     * @param origin 允许的源（默认 "*"）
     *
     * 自动添加 CORS 中间件，并为所有路径处理 OPTIONS 预检请求。
     */
    void enableCors(const std::string& origin = "*") {
        if (started_.load()) return;

        // 添加 CORS 中间件：为每个响应设置 CORS 头
        std::string capturedOrigin = origin;
        use([capturedOrigin](const HttpRequest& /*req*/, HttpResponse& resp) {
            resp.setCors(capturedOrigin);
        });
    }

    /**
     * @brief 启用 Metrics 指标收集
     * @param path 指标导出端点路径（默认 "/metrics"）
     *
     * 启用后:
     * - 自动记录 http_requests_total 计数
     * - 按 HTTP 方法分别计数（http_requests_get 等）
     * - 提供 Prometheus 格式的指标导出端点
     *
     * @note 必须在 start() 之前调用
     *
     * @example
     * @code
     * server.enableMetrics();           // 默认 /metrics
     * server.enableMetrics("/stats");   // 自定义路径
     * @endcode
     */
    void enableMetrics(const std::string& path = "/metrics") {
        if (started_.load()) return;

        // Metrics middleware
        use([](const HttpRequest& req, HttpResponse& /*resp*/) {
            Metrics::instance().increment("http_requests_total");
            Metrics::instance().increment("http_requests_" + methodToString(req.method));
        });

        // Metrics endpoint
        GET(path, [](const HttpRequest& /*req*/, HttpResponse& resp) {
            resp.setContentType("text/plain; version=0.0.4");
            resp.setBody(Metrics::instance().toPrometheus());
        });
    }

private:
    TcpServer server_;                              ///< 底层 TCP 服务器
    EventLoop* loop_;                               ///< 事件循环指针（用于优雅关闭）
    std::vector<Route> routes_;                     ///< 路由表
    std::vector<HttpHandler> middlewares_;          ///< 中间件列表
    std::unordered_map<std::string, std::string> staticDirs_;  ///< 静态文件目录映射
    std::atomic<bool> started_;                     ///< 是否已启动
    mutable std::shared_mutex routesMutex_;         ///< 保护路由/中间件/静态目录的读写锁
    double idleTimeoutSec_;                          ///< 空闲连接超时（秒）
    bool gzipEnabled_ = false;                       ///< 是否启用 Gzip 压缩
    size_t gzipMinSize_ = 1024;                      ///< Gzip 最小压缩大小（字节）

    /**
     * @brief 将 HttpMethod 转换为小写字符串
     * @param m HTTP 方法枚举
     * @return 方法名称字符串
     */
    static std::string methodToString(HttpMethod m) {
        switch (m) {
            case HttpMethod::GET:    return "get";
            case HttpMethod::POST:   return "post";
            case HttpMethod::PUT:    return "put";
            case HttpMethod::DELETE: return "delete";
            case HttpMethod::HEAD:   return "head";
            default:                 return "unknown";
        }
    }

    /**
     * @brief 连接回调
     * @param conn TCP 连接
     *
     * 可以在此记录连接状态
     */
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

    /**
     * @brief 消息回调
     * @param conn TCP 连接
     * @param buf 输入缓冲区
     * @param time 时间戳
     *
     * 循环处理粘包/流水线请求
     */
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp /*time*/) {
        // 循环处理粘包/流水线
        while (buf->readableBytes() > 0) {
            HttpRequest request;

            // 解析请求
            ParseResult result = parseRequest(buf, request);

            if (result == ParseResult::Incomplete) {
                // 数据不完整，等待更多数据
                return;
            } else if (result == ParseResult::Error) {
                // 解析失败，发送 400
                HttpResponse resp = HttpResponse::badRequest("Bad Request");
                resp.closeConnection = true;
                conn->send(resp.toString());
                conn->shutdown();
                return;
            }

            // 检查请求体大小
            size_t contentLen = 0;
            try {
                contentLen = request.contentLength();
            } catch (...) {
                contentLen = 0;
            }

            if (contentLen > kMaxBodySize) {
                HttpResponse resp = HttpResponse::badRequest("Request body too large");
                resp.closeConnection = true;
                conn->send(resp.toString());
                conn->shutdown();
                return;
            }

            // 请求体 Gzip 解压
            if (!request.body.empty()) {
                std::string encoding = request.getHeader("content-encoding");
                if (encoding.find("gzip") != std::string::npos) {
                    std::string decompressed = GzipCodec::decompress(request.body);
                    if (!decompressed.empty()) {
                        request.body = std::move(decompressed);
                    }
                }
            }

            // 处理请求
            HttpResponse response;
            handleRequest(request, response);

            // Gzip 压缩
            if (gzipEnabled_ && response.body.size() >= gzipMinSize_) {
                std::string acceptEncoding = request.getHeader("accept-encoding");
                if (acceptEncoding.find("gzip") != std::string::npos) {
                    auto it = response.headers.find("Content-Type");
                    std::string contentType = (it != response.headers.end()) ? it->second : "";
                    if (GzipCodec::shouldCompress(contentType)) {
                        std::string compressed = GzipCodec::compress(response.body);
                        if (!compressed.empty() && compressed.size() < response.body.size()) {
                            response.body = std::move(compressed);
                            response.setContentLength(response.body.size());
                            response.setHeader("Content-Encoding", "gzip");
                            response.setHeader("Vary", "Accept-Encoding");
                        }
                    }
                }
            }

            // 发送响应
            response.closeConnection = !request.keepAlive();
            conn->send(response.toString());

            if (response.closeConnection) {
                conn->shutdown();
                return;
            }

            // 继续处理可能的下一个请求 (流水线)
        }
    }

    /// 解析结果枚举
    enum class ParseResult {
        Complete,    ///< 解析完成
        Incomplete,  ///< 数据不完整
        Error        ///< 解析错误
    };

    /**
     * @brief 解析 HTTP 请求 (支持粘包)
     * @param buf 输入缓冲区
     * @param request 输出请求对象
     * @return 解析结果
     *
     * 先 peek 数据查找请求头结束位置，不消费数据
     * 确认数据完整后再消费
     */
    ParseResult parseRequest(Buffer* buf, HttpRequest& request) {
        // 先 peek，不消费数据
        const char* data = buf->peek();
        size_t len = buf->readableBytes();

        // 找请求头结束位置
        const char* headerEnd = static_cast<const char*>(
            memmem(data, len, "\r\n\r\n", 4));

        if (!headerEnd) {
            // 请求头不完整，检查是否超过限制
            if (len > 8192) {  // 请求头最大 8KB
                return ParseResult::Error;
            }
            return ParseResult::Incomplete;
        }

        size_t headerLen = headerEnd - data + 4;  // 包含 \r\n\r\n

        // 解析请求头
        std::string header(data, headerLen - 4);  // 不含末尾 \r\n\r\n
        if (!parseHeader(header, request)) {
            return ParseResult::Error;
        }

        // 检查请求体
        size_t contentLen = request.contentLength();
        size_t totalLen = headerLen + contentLen;

        if (len < totalLen) {
            // 请求体不完整
            return ParseResult::Incomplete;
        }

        // 现在可以消费数据了
        buf->retrieve(headerLen);
        if (contentLen > 0) {
            request.body.assign(buf->peek(), contentLen);
            buf->retrieve(contentLen);
        }

        return ParseResult::Complete;
    }

    /**
     * @brief 解析 HTTP 头部
     * @param header 头部字符串
     * @param request 输出请求对象
     * @return 是否成功
     */
    bool parseHeader(const std::string& header, HttpRequest& request) {
        // 找请求行
        size_t lineEnd = header.find("\r\n");
        if (lineEnd == std::string::npos) {
            return false;
        }

        // 解析请求行
        std::string requestLine = header.substr(0, lineEnd);
        if (requestLine.size() > kMaxRequestLine) {
            return false;
        }
        if (!request.parseRequestLine(requestLine)) {
            return false;
        }

        // 解析请求头字段
        size_t pos = lineEnd + 2;
        while (pos < header.size()) {
            lineEnd = header.find("\r\n", pos);
            if (lineEnd == std::string::npos) {
                lineEnd = header.size();
            }

            std::string line = header.substr(pos, lineEnd - pos);
            if (!line.empty()) {
                request.parseHeader(line);
            }
            pos = lineEnd + 2;
        }

        return true;
    }

    /**
     * @brief 处理 HTTP 请求
     * @param request HTTP 请求
     * @param response HTTP 响应
     *
     * 处理流程:
     * 1. 执行中间件
     * 2. 路由匹配
     * 3. 静态文件服务
     * 4. 返回 404
     */
    void handleRequest(const HttpRequest& request, HttpResponse& response) {
        std::shared_lock<std::shared_mutex> lock(routesMutex_);

        // 执行中间件
        for (auto& middleware : middlewares_) {
            middleware(request, response);
            // 如果中间件设置了错误状态码，停止处理
            if (static_cast<int>(response.statusCode) >= 400) {
                return;
            }
        }

        // 路由匹配（regex 已在路由注册时编译，匹配不会抛出 std::regex_error）
        for (const auto& route : routes_) {
            if (route.method == request.method) {
                if (std::regex_match(request.path, route.regex)) {
                    route.handler(request, response);
                    return;
                }
            }
        }

        // 静态文件
        for (const auto& [prefix, dir] : staticDirs_) {
            if (request.path.find(prefix) == 0) {
                serveFile(request, response, dir, request.path.substr(prefix.size()));
                return;
            }
        }

        // 404
        response = HttpResponse::notFound("Not Found: " + request.path);
    }

    /**
     * @brief URL 解码
     * @param encoded 编码的字符串
     * @return 解码后的字符串
     */
    std::string urlDecode(const std::string& encoded) {
        std::string result;
        result.reserve(encoded.size());

        for (size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] == '%' && i + 2 < encoded.size()) {
                // 解析十六进制值
                char hex[3] = {encoded[i + 1], encoded[i + 2], '\0'};
                char* end = nullptr;
                long val = strtol(hex, &end, 16);
                if (end == hex + 2 && val >= 0 && val <= 255) {
                    result += static_cast<char>(val);
                    i += 2;
                    continue;
                }
            }
            result += encoded[i];
        }
        return result;
    }

    /**
     * @brief 提供静态文件服务
     * @param request HTTP 请求
     * @param response HTTP 响应
     * @param dir 目录路径
     * @param filename 文件名
     */
    void serveFile(const HttpRequest& /*request*/, HttpResponse& response,
                   const std::string& dir, const std::string& filename) {
        // 先进行 URL 解码，再检查路径遍历
        std::string decodedFilename = urlDecode(filename);

        // 安全检查：防止路径遍历攻击（包括编码形式）
        if (decodedFilename.empty() ||
            decodedFilename.find("..") != std::string::npos ||
            decodedFilename[0] == '/' ||
            decodedFilename[0] == '~' ||
            decodedFilename.find('\\') != std::string::npos) {
            response = HttpResponse::badRequest("Invalid path");
            return;
        }

        std::string filepath = dir + "/" + decodedFilename;

        // realpath 验证：确保解析后的路径仍在 dir 下
        char resolvedPath[PATH_MAX];
        char resolvedDir[PATH_MAX];
        if (!realpath(filepath.c_str(), resolvedPath) ||
            !realpath(dir.c_str(), resolvedDir)) {
            response = HttpResponse::notFound();
            return;
        }
        // 确保文件路径以目录路径为前缀
        if (std::string(resolvedPath).find(resolvedDir) != 0) {
            response = HttpResponse::badRequest("Invalid path");
            return;
        }

        // 使用 RAII 包装 FILE*
        FILE* fp = fopen(filepath.c_str(), "rb");
        if (!fp) {
            response = HttpResponse::notFound();
            return;
        }

        // 使用 unique_ptr 管理 FILE*
        std::unique_ptr<FILE, decltype(&fclose)> fileGuard(fp, &fclose);

        // 获取文件大小
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (size <= 0 || size > static_cast<long>(kMaxBodySize)) {
            response = HttpResponse::serverError("File too large");
            return;
        }

        std::string content(size, '\0');
        size_t read_size = fread(&content[0], 1, size, fp);
        if (read_size != static_cast<size_t>(size)) {
            response = HttpResponse::serverError("File read error");
            return;
        }

        // 设置 Content-Type
        if (filename.find(".html") != std::string::npos) {
            response.setContentType("text/html; charset=utf-8");
        } else if (filename.find(".css") != std::string::npos) {
            response.setContentType("text/css");
        } else if (filename.find(".js") != std::string::npos) {
            response.setContentType("application/javascript");
        } else if (filename.find(".json") != std::string::npos) {
            response.setContentType("application/json");
        } else if (filename.find(".png") != std::string::npos) {
            response.setContentType("image/png");
        } else if (filename.find(".jpg") != std::string::npos ||
                   filename.find(".jpeg") != std::string::npos) {
            response.setContentType("image/jpeg");
        } else {
            response.setContentType("application/octet-stream");
        }

        response.setBody(content);
    }
};