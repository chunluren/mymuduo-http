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
 * 实现说明：
 *   协议级逻辑（路由、请求解析、中间件、Gzip、静态文件）已抽取到 HttpCore，
 *   HttpsServer 通过 **组合（composition）** 持有一个 HttpCore 实例。
 *   本类只负责 TLS 状态管理（SSL 对象 + memory BIO + 握手）
 *   和明文缓冲区的 HTTP 解析调度。
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

#include "HttpCore.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "SslContext.h"
#include "net/TcpServer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/Buffer.h"
#include "net/Callbacks.h"
#include "net/logger.h"

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

/// 请求处理函数类型（与 HttpServer 共用）
using HttpsHandler = HttpHandler;

/**
 * @class HttpsServer
 * @brief HTTPS 服务器（基于 memory BIO 的非阻塞 SSL）
 *
 * 架构:
 * - TcpServer 接收原始 TCP 连接
 * - 每个连接关联一个 SSL* + memory BIO pair
 * - onMessage 中: raw data -> BIO_write -> SSL_do_handshake/SSL_read -> HTTP parse
 * - 响应时: HTTP -> SSL_write -> BIO_read -> conn->send()
 *
 * 线程安全:
 * - 路由由 HttpCore 以 shared_mutex 保护，注册 / 匹配线程安全
 * - SSL 连接映射使用 mutex 保护（连接建立/断开时写入）
 * - 每个连接的 SSL 操作在对应的 IO 线程中执行（无竞争）
 */
class HttpsServer {
public:
    static constexpr size_t kMaxBodySize = HttpCore::kMaxBodySize;
    static constexpr size_t kMaxRequestLine = HttpCore::kMaxRequestLine;

    /**
     * @brief 构造 HTTPS 服务器
     * @param loop 事件循环（mainReactor）
     * @param addr 监听地址
     * @param certFile PEM 证书文件路径
     * @param keyFile PEM 私钥文件路径
     * @param name 服务器名称
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
        server_.setMessageCallback(
            [this](const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
                onMessage(conn, buf, time);
            });
        server_.setThreadNum(4);
    }

    void start() { server_.start(); }
    void setThreadNum(int num) { server_.setThreadNum(num); }
    void setIdleTimeout(double seconds) { idleTimeoutSec_ = seconds; }

    // ==================== 路由注册（转发到 HttpCore） ====================

    void GET(const std::string& path, HttpsHandler handler) {
        core_.GET(path, std::move(handler));
    }
    void POST(const std::string& path, HttpsHandler handler) {
        core_.POST(path, std::move(handler));
    }
    void PUT(const std::string& path, HttpsHandler handler) {
        core_.PUT(path, std::move(handler));
    }
    void DELETE(const std::string& path, HttpsHandler handler) {
        core_.DELETE(path, std::move(handler));
    }

    void use(HttpsHandler middleware) { core_.use(std::move(middleware)); }
    void enableGzip(size_t minSize = 1024) { core_.enableGzip(minSize); }
    void enableCors(const std::string& origin = "*") { core_.enableCors(origin); }
    void serveStatic(const std::string& urlPrefix, const std::string& dir) {
        core_.serveStatic(urlPrefix, dir);
    }
    void useRateLimit(int maxRequestsPerSec) {
        core_.useRateLimit(maxRequestsPerSec);
    }
    void enableMetrics(const std::string& path = "/metrics") {
        core_.enableMetrics(path);
    }

private:
    /**
     * @struct SslConn
     * @brief 每个 TCP 连接的 SSL 状态
     *
     * - rbio: 应用向其中写入原始 TCP 数据，OpenSSL 从中读取
     * - wbio: OpenSSL 向其中写入加密数据，应用从中读出并发送
     * SSL_set_bio() 后 SSL 对象拥有 BIO 的所有权，析构时只需 SSL_free()。
     */
    struct SslConn {
        SSL* ssl = nullptr;
        BIO* rbio = nullptr;
        BIO* wbio = nullptr;
        bool handshakeDone = false;
        std::string pendingPlaintext;  ///< 已解密但尚未构成完整 HTTP 请求的数据

        ~SslConn() {
            if (ssl) {
                SSL_free(ssl);  // 会自动释放关联的 BIO
                ssl = nullptr;
            }
        }

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

    // ==================== 回调实现 ====================

    void onConnection(const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            SSL* ssl = SSL_new(sslCtx_.get());
            if (!ssl) {
                LOG_ERROR("HttpsServer: SSL_new failed for %s",
                          conn->name().c_str());
                conn->shutdown();
                return;
            }

            BIO* rbio = BIO_new(BIO_s_mem());
            BIO* wbio = BIO_new(BIO_s_mem());
            if (!rbio || !wbio) {
                if (rbio) BIO_free(rbio);
                if (wbio) BIO_free(wbio);
                SSL_free(ssl);
                LOG_ERROR("HttpsServer: BIO_new failed for %s",
                          conn->name().c_str());
                conn->shutdown();
                return;
            }
            BIO_set_nbio(rbio, 1);
            BIO_set_nbio(wbio, 1);

            SSL_set_bio(ssl, rbio, wbio);
            SSL_set_accept_state(ssl);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto& sc = sslConns_[conn->name()];
                sc.ssl = ssl;
                sc.rbio = rbio;
                sc.wbio = wbio;
                sc.handshakeDone = false;
            }

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
            std::lock_guard<std::mutex> lock(mutex_);
            sslConns_.erase(conn->name());
        }
    }

    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp /*time*/) {
        SslConn* sc = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sslConns_.find(conn->name());
            if (it == sslConns_.end()) return;
            sc = &it->second;
        }

        // 第 1 步：原始密文 -> read BIO
        size_t rawLen = buf->readableBytes();
        if (rawLen > 0) {
            int written = BIO_write(sc->rbio, buf->peek(),
                                    static_cast<int>(rawLen));
            if (written > 0) {
                buf->retrieve(static_cast<size_t>(written));
            } else {
                conn->shutdown();
                return;
            }
        }

        // 第 2 步：TLS 握手
        if (!sc->handshakeDone) {
            int ret = SSL_do_handshake(sc->ssl);
            flushSslOutput(conn, sc->wbio);

            if (ret == 1) {
                sc->handshakeDone = true;
            } else {
                int err = SSL_get_error(sc->ssl, ret);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    return;
                }
                LOG_ERROR("HttpsServer: SSL handshake failed for %s (err=%d)",
                          conn->name().c_str(), err);
                conn->shutdown();
                return;
            }
        }

        // 第 3 步：SSL_read 读出明文
        char plaintext[16384];
        int n;
        while ((n = SSL_read(sc->ssl, plaintext, sizeof(plaintext))) > 0) {
            sc->pendingPlaintext.append(plaintext, n);
        }
        if (n <= 0) {
            int err = SSL_get_error(sc->ssl, n);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE &&
                err != SSL_ERROR_ZERO_RETURN) {
                conn->shutdown();
                return;
            }
            if (err == SSL_ERROR_ZERO_RETURN) {
                conn->shutdown();
                return;
            }
        }

        // 第 4 步：从明文缓冲区解析 HTTP 请求（可能有多个 - pipeline）
        while (!sc->pendingPlaintext.empty()) {
            HttpRequest request;
            HttpCore::ParseResult result =
                core_.parseRequest(sc->pendingPlaintext, request);

            if (result == HttpCore::ParseResult::Incomplete) {
                break;
            }
            if (result == HttpCore::ParseResult::Error) {
                HttpResponse resp = HttpResponse::badRequest("Bad Request");
                resp.closeConnection = true;
                sslWrite(conn, sc, resp.toString());
                conn->shutdown();
                return;
            }

            // 第 5 步：中间件 + 路由 + Gzip
            HttpResponse response;
            core_.handleRequest(request, response);
            core_.postProcessResponse(request, response);

            response.closeConnection = !request.keepAlive();
            sslWrite(conn, sc, response.toString());

            if (response.closeConnection) {
                SSL_shutdown(sc->ssl);
                flushSslOutput(conn, sc->wbio);
                conn->shutdown();
                return;
            }
        }
    }

    // ==================== SSL I/O helpers ====================

    /// 将 wbio 中待发送的加密数据通过 TcpConnection 发出
    void flushSslOutput(const TcpConnectionPtr& conn, BIO* wbio) {
        char buf[16384];
        int pending;
        while ((pending = BIO_ctrl_pending(wbio)) > 0) {
            int n = BIO_read(wbio, buf,
                             std::min(pending, static_cast<int>(sizeof(buf))));
            if (n > 0) {
                conn->send(std::string(buf, n));
            } else {
                break;
            }
        }
    }

    /// 明文 -> SSL_write 加密 -> flushSslOutput 发送
    void sslWrite(const TcpConnectionPtr& conn, SslConn* sc,
                  const std::string& data) {
        if (data.empty()) return;

        size_t totalWritten = 0;
        while (totalWritten < data.size()) {
            int toWrite = static_cast<int>(
                std::min(data.size() - totalWritten, size_t(16384)));
            int written = SSL_write(sc->ssl, data.c_str() + totalWritten,
                                    toWrite);
            if (written > 0) {
                totalWritten += static_cast<size_t>(written);
                flushSslOutput(conn, sc->wbio);
            } else {
                int err = SSL_get_error(sc->ssl, written);
                if (err == SSL_ERROR_WANT_WRITE) {
                    flushSslOutput(conn, sc->wbio);
                    continue;
                }
                LOG_ERROR("HttpsServer: SSL_write failed (err=%d)", err);
                break;
            }
        }
    }

    // ==================== 成员变量 ====================

    TcpServer server_;           ///< 底层 TCP 服务器
    EventLoop* loop_;            ///< 事件循环指针
    SslContext sslCtx_;          ///< SSL 上下文
    HttpCore core_;              ///< HTTP 协议处理核心
    double idleTimeoutSec_ = 60.0;

    std::mutex mutex_;           ///< 保护 sslConns_
    std::unordered_map<std::string, SslConn> sslConns_;
};
