/**
 * @file HttpsServer.h
 * @brief HTTPS 服务器实现（TLS over non-blocking TCP）
 *
 * 基于 memory BIO 的 OpenSSL 集成，与 Reactor 模式无缝配合:
 * - 原始 TCP 数据到达 -> 写入 SSL 读 BIO -> SSL_read() 得到明文
 * - SSL_write() 加密 -> 从 SSL 写 BIO 读出 -> 通过 TcpConnection 发送
 *
 * 这种方式避免了直接操作 fd，完全兼容 epoll 非阻塞事件循环。
 *
 * API 与 HttpServer 保持一致（GET/POST/PUT/DELETE 路由注册）。
 *
 * @example 使用示例
 * @code
 * EventLoop loop;
 * InetAddress addr(443);
 * HttpsServer server(&loop, addr, "cert.pem", "key.pem", "MyHttpsServer");
 *
 * server.GET("/", [](const HttpRequest& req, HttpResponse& resp) {
 *     resp.setJson(R"({"message":"Hello HTTPS!"})");
 * });
 *
 * server.setThreadNum(4);
 * server.start();
 * loop.loop();
 * @endcode
 */

#pragma once

#include "SslContext.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "GzipMiddleware.h"
#include "net/TcpServer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/Buffer.h"
#include "net/Callbacks.h"
#include "net/logger.h"

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>

#include <functional>
#include <unordered_map>
#include <regex>
#include <mutex>
#include <vector>
#include <string>
#include <memory>
#include <cstring>
#include <climits>

/// 请求处理函数类型（与 HttpServer 共用）
using HttpsHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

/**
 * @class HttpsServer
 * @brief HTTPS 服务器，基于 memory BIO 实现非阻塞 SSL
 *
 * 架构:
 * - TcpServer 接收原始 TCP 连接
 * - 每个连接关联一个 SSL* + memory BIO pair
 * - onMessage 中: raw data -> BIO_write -> SSL_do_handshake/SSL_read -> HTTP parse
 * - 响应时: HTTP -> SSL_write -> BIO_read -> conn->send()
 *
 * 线程安全:
 * - 路由表在 start() 前注册，启动后只读
 * - SSL 连接映射使用 mutex 保护（连接建立/断开时写入）
 * - 每个连接的 SSL 操作在对应的 IO 线程中执行（无竞争）
 */
class HttpsServer {
public:
    /// 最大请求体大小 (10MB)
    static constexpr size_t kMaxBodySize = 10 * 1024 * 1024;
    /// 最大请求行长度
    static constexpr size_t kMaxRequestLine = 8192;

    /**
     * @brief 构造 HTTPS 服务器
     * @param loop 事件循环（mainReactor）
     * @param addr 监听地址
     * @param certFile PEM 证书文件路径
     * @param keyFile PEM 私钥文件路径
     * @param name 服务器名称
     *
     * @throws std::runtime_error 如果证书加载失败
     */
    HttpsServer(EventLoop* loop, const InetAddress& addr,
                const std::string& certFile, const std::string& keyFile,
                const std::string& name = "HttpsServer")
        : server_(loop, addr, name)
        , loop_(loop)
        , sslCtx_()
    {
        if (!sslCtx_.loadCert(certFile, keyFile)) {
            LOG_FATAL("HttpsServer: failed to load certificate(%s) or key(%s)",
                      certFile.c_str(), keyFile.c_str());
        }

        server_.setConnectionCallback([this](const TcpConnectionPtr& conn) {
            onConnection(conn);
        });
        server_.setMessageCallback([this](const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
            onMessage(conn, buf, time);
        });
        server_.setThreadNum(4);
    }

    /**
     * @brief 启动服务器
     */
    void start() { server_.start(); }

    /**
     * @brief 设置 I/O 线程数量
     * @param num 线程数量
     */
    void setThreadNum(int num) { server_.setThreadNum(num); }

    /**
     * @brief 设置空闲连接超时（秒）
     * @param seconds 超时时间，0 表示禁用
     */
    void setIdleTimeout(double seconds) { idleTimeoutSec_ = seconds; }

    // ==================== 路由注册 ====================

    /**
     * @brief 注册 GET 路由
     * @param path URL 路径（支持正则表达式）
     * @param handler 处理函数
     */
    void GET(const std::string& path, HttpsHandler handler) {
        routes_.push_back({HttpMethod::GET, path, handler, std::regex(path)});
    }

    /**
     * @brief 注册 POST 路由
     */
    void POST(const std::string& path, HttpsHandler handler) {
        routes_.push_back({HttpMethod::POST, path, handler, std::regex(path)});
    }

    /**
     * @brief 注册 PUT 路由
     */
    void PUT(const std::string& path, HttpsHandler handler) {
        routes_.push_back({HttpMethod::PUT, path, handler, std::regex(path)});
    }

    /**
     * @brief 注册 DELETE 路由
     */
    void DELETE(const std::string& path, HttpsHandler handler) {
        routes_.push_back({HttpMethod::DELETE, path, handler, std::regex(path)});
    }

    /**
     * @brief 添加中间件
     * @param middleware 中间件函数
     */
    void use(HttpsHandler middleware) {
        middlewares_.push_back(middleware);
    }

    /**
     * @brief 启用 Gzip 压缩
     * @param minSize 最小压缩大小（字节）
     */
    void enableGzip(size_t minSize = 1024) {
        gzipEnabled_ = true;
        gzipMinSize_ = minSize;
    }

    /**
     * @brief 启用 CORS
     * @param origin 允许的源
     */
    void enableCors(const std::string& origin = "*") {
        std::string capturedOrigin = origin;
        use([capturedOrigin](const HttpRequest& /*req*/, HttpResponse& resp) {
            resp.setCors(capturedOrigin);
        });
    }

private:
    /**
     * @struct SslConn
     * @brief 每个 TCP 连接的 SSL 状态
     *
     * 使用 memory BIO 对:
     * - rbio: 应用向其中写入原始 TCP 数据，OpenSSL 从中读取
     * - wbio: OpenSSL 向其中写入加密数据，应用从中读出并发送
     *
     * SSL_set_bio() 后 SSL 对象拥有 BIO 的所有权，析构时只需 SSL_free()
     */
    struct SslConn {
        SSL* ssl = nullptr;
        BIO* rbio = nullptr;      ///< read BIO: feed raw TCP data here
        BIO* wbio = nullptr;      ///< write BIO: read encrypted output from here
        bool handshakeDone = false;
        std::string pendingPlaintext;  ///< 已解密但尚未构成完整 HTTP 请求的数据

        ~SslConn() {
            // SSL_free 会自动释放关联的 BIO
            if (ssl) {
                SSL_free(ssl);
                ssl = nullptr;
            }
        }

        // 不可拷贝
        SslConn() = default;
        SslConn(const SslConn&) = delete;
        SslConn& operator=(const SslConn&) = delete;
        SslConn(SslConn&& other) noexcept
            : ssl(other.ssl), rbio(other.rbio), wbio(other.wbio),
              handshakeDone(other.handshakeDone),
              pendingPlaintext(std::move(other.pendingPlaintext)) {
            other.ssl = nullptr;
            other.rbio = nullptr;
            other.wbio = nullptr;
        }
        SslConn& operator=(SslConn&& other) noexcept {
            if (this != &other) {
                if (ssl) SSL_free(ssl);
                ssl = other.ssl;
                rbio = other.rbio;
                wbio = other.wbio;
                handshakeDone = other.handshakeDone;
                pendingPlaintext = std::move(other.pendingPlaintext);
                other.ssl = nullptr;
                other.rbio = nullptr;
                other.wbio = nullptr;
            }
            return *this;
        }
    };

    /**
     * @struct RouteEntry
     * @brief 路由表条目
     */
    struct RouteEntry {
        HttpMethod method;
        std::string pattern;
        HttpsHandler handler;
        std::regex regex;
    };

    // ==================== 回调实现 ====================

    /**
     * @brief 连接建立/断开回调
     *
     * 连接建立时: 创建 SSL* + memory BIO pair，设为 accept 状态
     * 连接断开时: 清理 SSL 资源
     */
    void onConnection(const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            // 创建 SSL 对象和 memory BIO 对
            SSL* ssl = SSL_new(sslCtx_.get());
            if (!ssl) {
                LOG_ERROR("HttpsServer: SSL_new failed for %s", conn->name().c_str());
                conn->shutdown();
                return;
            }

            BIO* rbio = BIO_new(BIO_s_mem());
            BIO* wbio = BIO_new(BIO_s_mem());
            if (!rbio || !wbio) {
                if (rbio) BIO_free(rbio);
                if (wbio) BIO_free(wbio);
                SSL_free(ssl);
                LOG_ERROR("HttpsServer: BIO_new failed for %s", conn->name().c_str());
                conn->shutdown();
                return;
            }

            // 设置非阻塞 BIO 模式
            BIO_set_nbio(rbio, 1);
            BIO_set_nbio(wbio, 1);

            // SSL_set_bio 将 BIO 所有权转移给 SSL 对象
            SSL_set_bio(ssl, rbio, wbio);
            SSL_set_accept_state(ssl);  // 服务端模式

            // 存储 SSL 连接状态
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto& sc = sslConns_[conn->name()];
                sc.ssl = ssl;
                sc.rbio = rbio;
                sc.wbio = wbio;
                sc.handshakeDone = false;
            }

            // 空闲超时
            if (idleTimeoutSec_ > 0) {
                auto weakConn = std::weak_ptr<TcpConnection>(conn);
                conn->getLoop()->runAfter(idleTimeoutSec_, [weakConn]() {
                    auto c = weakConn.lock();
                    if (c && c->connected()) {
                        c->shutdown();
                    }
                });
            }
        } else {
            // 连接断开，清理 SSL 资源
            std::lock_guard<std::mutex> lock(mutex_);
            sslConns_.erase(conn->name());
        }
    }

    /**
     * @brief 消息到达回调
     *
     * 处理流程:
     * 1. 将原始 TCP 数据写入 SSL 的 read BIO
     * 2. 如果握手未完成，执行 SSL_do_handshake()
     * 3. 握手完成后，SSL_read() 获取解密数据
     * 4. 累积解密数据，尝试解析 HTTP 请求
     * 5. 路由匹配 + 处理 + SSL_write 响应
     */
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp /*time*/) {
        SslConn* sc = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sslConns_.find(conn->name());
            if (it == sslConns_.end()) return;
            sc = &it->second;
        }

        // 1. Feed raw TCP data into SSL's read BIO
        size_t rawLen = buf->readableBytes();
        if (rawLen > 0) {
            int written = BIO_write(sc->rbio, buf->peek(), static_cast<int>(rawLen));
            if (written > 0) {
                buf->retrieve(static_cast<size_t>(written));
            } else {
                // BIO write failed
                conn->shutdown();
                return;
            }
        }

        // 2. Handshake phase
        if (!sc->handshakeDone) {
            int ret = SSL_do_handshake(sc->ssl);
            // Flush any handshake output (ServerHello, Certificate, etc.)
            flushSslOutput(conn, sc->wbio);

            if (ret == 1) {
                // Handshake complete
                sc->handshakeDone = true;
            } else {
                int err = SSL_get_error(sc->ssl, ret);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    // Need more data from client to continue handshake
                    return;
                }
                // Handshake failed
                LOG_ERROR("HttpsServer: SSL handshake failed for %s (err=%d)",
                          conn->name().c_str(), err);
                conn->shutdown();
                return;
            }
        }

        // 3. Read decrypted application data
        char plaintext[16384];
        int n;
        while ((n = SSL_read(sc->ssl, plaintext, sizeof(plaintext))) > 0) {
            sc->pendingPlaintext.append(plaintext, n);
        }

        // Check for SSL errors after SSL_read
        if (n <= 0) {
            int err = SSL_get_error(sc->ssl, n);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE &&
                err != SSL_ERROR_ZERO_RETURN) {
                // Fatal SSL error
                conn->shutdown();
                return;
            }
            if (err == SSL_ERROR_ZERO_RETURN) {
                // Peer sent close_notify
                conn->shutdown();
                return;
            }
        }

        // 4. Parse HTTP requests from accumulated plaintext
        while (!sc->pendingPlaintext.empty()) {
            HttpRequest request;
            ParseResult result = parseHttpRequest(sc->pendingPlaintext, request);

            if (result == ParseResult::Incomplete) {
                // Wait for more data
                break;
            } else if (result == ParseResult::Error) {
                // Send 400 Bad Request
                HttpResponse resp = HttpResponse::badRequest("Bad Request");
                resp.closeConnection = true;
                sslWrite(conn, sc, resp.toString());
                conn->shutdown();
                return;
            }

            // 5. Handle the request
            HttpResponse response;
            handleRequest(request, response);

            // Gzip compression
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

            // Send response
            response.closeConnection = !request.keepAlive();
            sslWrite(conn, sc, response.toString());

            if (response.closeConnection) {
                // Send SSL close_notify before TCP shutdown
                SSL_shutdown(sc->ssl);
                flushSslOutput(conn, sc->wbio);
                conn->shutdown();
                return;
            }
        }
    }

    // ==================== SSL I/O helpers ====================

    /**
     * @brief 将加密数据从 SSL 写 BIO 刷新到 TCP 连接
     * @param conn TCP 连接
     * @param wbio SSL 写 BIO
     *
     * 读取 OpenSSL 产生的所有待发送加密数据，通过 TcpConnection 发送。
     */
    void flushSslOutput(const TcpConnectionPtr& conn, BIO* wbio) {
        char buf[16384];
        int pending;
        while ((pending = BIO_ctrl_pending(wbio)) > 0) {
            int n = BIO_read(wbio, buf, std::min(pending, static_cast<int>(sizeof(buf))));
            if (n > 0) {
                conn->send(std::string(buf, n));
            } else {
                break;
            }
        }
    }

    /**
     * @brief 通过 SSL 发送数据（加密后发送）
     * @param conn TCP 连接
     * @param sc SSL 连接状态
     * @param data 明文数据
     *
     * 流程: data -> SSL_write() 加密 -> BIO_read() 读出密文 -> conn->send()
     */
    void sslWrite(const TcpConnectionPtr& conn, SslConn* sc, const std::string& data) {
        if (data.empty()) return;

        size_t totalWritten = 0;
        while (totalWritten < data.size()) {
            int toWrite = static_cast<int>(
                std::min(data.size() - totalWritten, size_t(16384)));
            int written = SSL_write(sc->ssl, data.c_str() + totalWritten, toWrite);
            if (written > 0) {
                totalWritten += static_cast<size_t>(written);
                // Flush encrypted data to TCP
                flushSslOutput(conn, sc->wbio);
            } else {
                int err = SSL_get_error(sc->ssl, written);
                if (err == SSL_ERROR_WANT_WRITE) {
                    flushSslOutput(conn, sc->wbio);
                    continue;
                }
                // Fatal error
                LOG_ERROR("HttpsServer: SSL_write failed (err=%d)", err);
                break;
            }
        }
    }

    // ==================== HTTP 解析 ====================

    /// 解析结果枚举
    enum class ParseResult {
        Complete,    ///< 请求完整
        Incomplete,  ///< 数据不足
        Error        ///< 解析错误
    };

    /**
     * @brief 从累积的明文中解析 HTTP 请求
     * @param data 累积的明文数据（成功时会消费已解析部分）
     * @param request 输出请求对象
     * @return 解析结果
     */
    ParseResult parseHttpRequest(std::string& data, HttpRequest& request) {
        // 查找请求头结束标记
        auto headerEnd = data.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            if (data.size() > kMaxRequestLine) {
                return ParseResult::Error;
            }
            return ParseResult::Incomplete;
        }

        size_t headerLen = headerEnd + 4;  // 包含 \r\n\r\n
        std::string header = data.substr(0, headerEnd);  // 不含末尾 \r\n\r\n

        // 解析请求头
        if (!parseHeader(header, request)) {
            return ParseResult::Error;
        }

        // 检查请求体
        size_t contentLen = request.contentLength();
        size_t totalLen = headerLen + contentLen;

        if (data.size() < totalLen) {
            return ParseResult::Incomplete;
        }

        // 提取请求体
        if (contentLen > 0) {
            if (contentLen > kMaxBodySize) {
                return ParseResult::Error;
            }
            request.body = data.substr(headerLen, contentLen);
        }

        // 消费已解析的数据
        data.erase(0, totalLen);
        return ParseResult::Complete;
    }

    /**
     * @brief 解析 HTTP 头部
     * @param header 头部字符串（不含尾部空行）
     * @param request 输出请求对象
     * @return 是否成功
     */
    bool parseHeader(const std::string& header, HttpRequest& request) {
        size_t lineEnd = header.find("\r\n");
        if (lineEnd == std::string::npos) {
            // 只有请求行，没有额外的头部
            return request.parseRequestLine(header);
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

    // ==================== 路由处理 ====================

    /**
     * @brief 处理 HTTP 请求（中间件 + 路由匹配）
     * @param request HTTP 请求
     * @param response HTTP 响应
     */
    void handleRequest(const HttpRequest& request, HttpResponse& response) {
        // 执行中间件
        for (auto& middleware : middlewares_) {
            middleware(request, response);
            if (static_cast<int>(response.statusCode) >= 400) {
                return;
            }
        }

        // 路由匹配
        for (const auto& route : routes_) {
            if (route.method == request.method) {
                if (std::regex_match(request.path, route.regex)) {
                    route.handler(request, response);
                    return;
                }
            }
        }

        // 404
        response = HttpResponse::notFound("Not Found: " + request.path);
    }

    // ==================== 成员变量 ====================

    TcpServer server_;                              ///< 底层 TCP 服务器
    EventLoop* loop_;                               ///< 事件循环指针
    SslContext sslCtx_;                             ///< SSL 上下文
    double idleTimeoutSec_ = 60.0;                  ///< 空闲超时（秒）

    /// 路由表
    std::vector<RouteEntry> routes_;
    /// 中间件列表
    std::vector<HttpsHandler> middlewares_;

    /// Gzip 配置
    bool gzipEnabled_ = false;
    size_t gzipMinSize_ = 1024;

    /// SSL 连接映射（连接名 -> SSL 状态）
    std::mutex mutex_;
    std::unordered_map<std::string, SslConn> sslConns_;
};
