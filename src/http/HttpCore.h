/**
 * @file HttpCore.h
 * @brief HTTP 协议处理核心（被 HttpServer 和 HttpsServer 共享）
 *
 * 职责：
 * - 路由注册 + 匹配（精确 + 正则）
 * - 请求解析（从 Buffer 或 string 读取完整请求）
 * - 中间件链执行
 * - 请求体 Gzip 解压 / 响应体 Gzip 压缩
 * - 静态文件服务
 *
 * 不负责：
 * - TCP 传输（由 HttpServer 的 TcpServer 或 HttpsServer 的 SSL BIO 处理）
 *
 * HttpServer 和 HttpsServer 使用 **组合（composition）** 方式持有
 * 一个 HttpCore 实例，转发路由注册与处理请求的调用，
 * 彼此的传输层保持独立。
 */

#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "GzipMiddleware.h"
#include "net/Buffer.h"
#include "util/RateLimiter.h"
#include "util/Metrics.h"

#include <functional>
#include <unordered_map>
#include <vector>
#include <regex>
#include <shared_mutex>
#include <mutex>
#include <memory>
#include <string>
#include <cstring>
#include <climits>
#include <cstdio>
#include <cstdlib>

/// 请求处理函数类型
using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

/**
 * @struct Route
 * @brief 路由项
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

    Route(HttpMethod m, const std::string& p, HttpHandler h)
        : method(m), pattern(p), handler(std::move(h)), regex(p) {}
};

/**
 * @class HttpCore
 * @brief HTTP 协议处理核心
 */
class HttpCore {
public:
    /// 最大请求体大小 (10MB)
    static constexpr size_t kMaxBodySize = 10 * 1024 * 1024;
    /// 最大请求行 / 请求头长度（字节）
    static constexpr size_t kMaxRequestLine = 8192;

    /// 请求解析结果
    enum class ParseResult {
        Complete,    ///< 解析完成
        Incomplete,  ///< 数据不完整
        Error        ///< 解析错误
    };

    HttpCore() = default;
    HttpCore(const HttpCore&) = delete;
    HttpCore& operator=(const HttpCore&) = delete;

    // ==================== 路由注册 ====================

    void GET(const std::string& path, HttpHandler handler) {
        addRoute(HttpMethod::GET, path, std::move(handler));
    }
    void POST(const std::string& path, HttpHandler handler) {
        addRoute(HttpMethod::POST, path, std::move(handler));
    }
    void PUT(const std::string& path, HttpHandler handler) {
        addRoute(HttpMethod::PUT, path, std::move(handler));
    }
    void DELETE(const std::string& path, HttpHandler handler) {
        addRoute(HttpMethod::DELETE, path, std::move(handler));
    }

    /// 注册中间件
    void use(HttpHandler middleware) {
        std::unique_lock<std::shared_mutex> lock(routesMutex_);
        middlewares_.push_back(std::move(middleware));
    }

    /// 配置静态文件目录映射
    void serveStatic(const std::string& urlPrefix, const std::string& dir) {
        std::unique_lock<std::shared_mutex> lock(routesMutex_);
        staticDirs_[urlPrefix] = dir;
    }

    /// 启用 CORS（作为一个中间件）
    void enableCors(const std::string& origin = "*") {
        std::string capturedOrigin = origin;
        use([capturedOrigin](const HttpRequest& /*req*/, HttpResponse& resp) {
            resp.setCors(capturedOrigin);
        });
    }

    /// 启用基于客户端 IP 的令牌桶限流
    void useRateLimit(int maxRequestsPerSec) {
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

    /// 启用 Prometheus 指标收集 + /metrics 端点
    void enableMetrics(const std::string& path = "/metrics") {
        use([](const HttpRequest& req, HttpResponse& /*resp*/) {
            Metrics::instance().increment("http_requests_total");
            Metrics::instance().increment(
                "http_requests_" + methodToString(req.method));
        });
        GET(path, [](const HttpRequest& /*req*/, HttpResponse& resp) {
            resp.setContentType("text/plain; version=0.0.4");
            resp.setBody(Metrics::instance().toPrometheus());
        });
    }

    /// 启用响应体 Gzip 压缩
    void enableGzip(size_t minSize = 1024) {
        gzipEnabled_ = true;
        gzipMinSize_ = minSize;
    }

    bool gzipEnabled() const { return gzipEnabled_; }
    size_t gzipMinSize() const { return gzipMinSize_; }

    // ==================== 请求解析 ====================

    /**
     * @brief 从 Buffer 解析 HTTP 请求（成功时消费缓冲区中的数据）
     *
     * 先 peek 缓冲区内容查找请求头结束位置，不消费数据；
     * 确认请求完整后再 retrieve。
     *
     * 解析成功且请求体非空时，如果请求声明了 Content-Encoding: gzip，
     * 将自动对 body 进行 gzip 解压。
     */
    ParseResult parseRequest(Buffer* buf, HttpRequest& request) {
        const char* data = buf->peek();
        size_t len = buf->readableBytes();

        const char* headerEnd = static_cast<const char*>(
            memmem(data, len, "\r\n\r\n", 4));
        if (!headerEnd) {
            if (len > kMaxRequestLine) {
                return ParseResult::Error;
            }
            return ParseResult::Incomplete;
        }

        size_t headerLen = headerEnd - data + 4;  // 含 \r\n\r\n
        std::string header(data, headerLen - 4);
        if (!parseHeader(header, request)) {
            return ParseResult::Error;
        }

        size_t contentLen = 0;
        try {
            contentLen = request.contentLength();
        } catch (...) {
            contentLen = 0;
        }
        if (contentLen > kMaxBodySize) {
            return ParseResult::Error;
        }

        size_t totalLen = headerLen + contentLen;
        if (len < totalLen) {
            return ParseResult::Incomplete;
        }

        buf->retrieve(headerLen);
        if (contentLen > 0) {
            request.body.assign(buf->peek(), contentLen);
            buf->retrieve(contentLen);
        }

        maybeDecompressRequestBody(request);
        return ParseResult::Complete;
    }

    /**
     * @brief 从累积的明文字符串中解析 HTTP 请求（HTTPS 用）
     *
     * 成功时会从 data 开头擦除已解析的请求字节。
     */
    ParseResult parseRequest(std::string& data, HttpRequest& request) {
        auto headerEnd = data.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            if (data.size() > kMaxRequestLine) {
                return ParseResult::Error;
            }
            return ParseResult::Incomplete;
        }

        size_t headerLen = headerEnd + 4;
        std::string header = data.substr(0, headerEnd);
        if (!parseHeader(header, request)) {
            return ParseResult::Error;
        }

        size_t contentLen = 0;
        try {
            contentLen = request.contentLength();
        } catch (...) {
            contentLen = 0;
        }
        if (contentLen > kMaxBodySize) {
            return ParseResult::Error;
        }

        size_t totalLen = headerLen + contentLen;
        if (data.size() < totalLen) {
            return ParseResult::Incomplete;
        }

        if (contentLen > 0) {
            request.body = data.substr(headerLen, contentLen);
        }
        data.erase(0, totalLen);

        maybeDecompressRequestBody(request);
        return ParseResult::Complete;
    }

    // ==================== 请求处理 ====================

    /**
     * @brief 执行完整的请求处理流水线
     *
     * 流程:
     *   1. 执行所有中间件（若中间件设置 >=400 状态码则中止）
     *   2. 精确路径匹配（O(1) 哈希）
     *   3. 正则路径匹配
     *   4. 静态文件
     *   5. 回落到 404
     */
    void handleRequest(const HttpRequest& request, HttpResponse& response) {
        std::shared_lock<std::shared_mutex> lock(routesMutex_);

        // 中间件
        for (auto& middleware : middlewares_) {
            middleware(request, response);
            if (static_cast<int>(response.statusCode) >= 400) {
                return;
            }
        }

        // 精确匹配（快速路径）
        auto pathIt = exactRoutes_.find(request.path);
        if (pathIt != exactRoutes_.end()) {
            auto methodIt = pathIt->second.find(static_cast<int>(request.method));
            if (methodIt != pathIt->second.end()) {
                methodIt->second(request, response);
                return;
            }
        }

        // 正则匹配（慢速路径）
        for (const auto& route : routes_) {
            if (route.method == request.method &&
                std::regex_match(request.path, route.regex)) {
                route.handler(request, response);
                return;
            }
        }

        // 静态文件 — 用最长前缀匹配（避免 "/" 拦截 "/uploads" 等更具体的路径）
        const std::string* bestPrefix = nullptr;
        const std::string* bestDir = nullptr;
        for (const auto& kv : staticDirs_) {
            const std::string& prefix = kv.first;
            if (request.path.find(prefix) == 0) {
                if (!bestPrefix || prefix.size() > bestPrefix->size()) {
                    bestPrefix = &kv.first;
                    bestDir = &kv.second;
                }
            }
        }
        if (bestPrefix) {
            // 剥掉前缀后再去掉前导 '/'（serveFile 不接受以 '/' 开头的子路径）
            std::string sub = request.path.substr(bestPrefix->size());
            while (!sub.empty() && sub.front() == '/') sub.erase(0, 1);
            // 子路径为空时（如访问目录根），默认尝试 index.html
            if (sub.empty()) sub = "index.html";
            serveFile(request, response, *bestDir, sub);
            return;
        }

        // 404
        response = HttpResponse::notFound("Not Found: " + request.path);
    }

    /**
     * @brief 对响应体进行 Gzip 压缩（条件满足时）
     *
     * 条件:
     *  - enableGzip() 已调用
     *  - 响应体大小 >= minSize
     *  - 请求 Accept-Encoding 含 gzip
     *  - Content-Type 属于可压缩类型
     *  - 压缩后确实更小
     */
    void postProcessResponse(const HttpRequest& request, HttpResponse& response) {
        if (!gzipEnabled_ || response.body.size() < gzipMinSize_) return;

        std::string acceptEncoding = request.getHeader("accept-encoding");
        if (acceptEncoding.find("gzip") == std::string::npos) return;

        auto it = response.headers.find("Content-Type");
        std::string contentType = (it != response.headers.end()) ? it->second : "";
        if (!GzipCodec::shouldCompress(contentType)) return;

        std::string compressed = GzipCodec::compress(response.body);
        if (!compressed.empty() && compressed.size() < response.body.size()) {
            response.body = std::move(compressed);
            response.setContentLength(response.body.size());
            response.setHeader("Content-Encoding", "gzip");
            response.setHeader("Vary", "Accept-Encoding");
        }
    }

private:
    // ==================== 路由注册内部实现 ====================

    void addRoute(HttpMethod method, const std::string& path,
                  HttpHandler handler) {
        std::unique_lock<std::shared_mutex> lock(routesMutex_);
        if (isExactPath(path)) {
            exactRoutes_[path][static_cast<int>(method)] = std::move(handler);
        } else {
            routes_.emplace_back(method, path, std::move(handler));
        }
    }

    /// 判断路径是否为精确字符串（不含正则元字符）
    static bool isExactPath(const std::string& path) {
        for (char c : path) {
            if (c == '(' || c == ')' || c == '[' || c == ']' ||
                c == '{' || c == '}' || c == '.' || c == '*' ||
                c == '+' || c == '?' || c == '|' || c == '^' || c == '$') {
                return false;
            }
        }
        return true;
    }

    /// HttpMethod -> 小写字符串（用于 Metrics tag）
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

    // ==================== 请求头解析 ====================

    bool parseHeader(const std::string& header, HttpRequest& request) {
        size_t lineEnd = header.find("\r\n");
        if (lineEnd == std::string::npos) {
            // 仅有请求行的极简请求
            if (header.size() > kMaxRequestLine) return false;
            return request.parseRequestLine(header);
        }

        std::string requestLine = header.substr(0, lineEnd);
        if (requestLine.size() > kMaxRequestLine) return false;
        if (!request.parseRequestLine(requestLine)) return false;

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

    /// 若请求声明 Content-Encoding: gzip 则对 body 原地解压
    static void maybeDecompressRequestBody(HttpRequest& request) {
        if (request.body.empty()) return;
        std::string encoding = request.getHeader("content-encoding");
        if (encoding.find("gzip") == std::string::npos) return;
        std::string decompressed = GzipCodec::decompress(request.body);
        if (!decompressed.empty()) {
            request.body = std::move(decompressed);
        }
    }

    // ==================== 静态文件服务 ====================

    /// URL 解码（%XX -> 原字符）
    static std::string urlDecode(const std::string& encoded) {
        std::string result;
        result.reserve(encoded.size());
        for (size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] == '%' && i + 2 < encoded.size()) {
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

    /// 提供静态文件（含路径遍历防护）
    void serveFile(const HttpRequest& /*request*/, HttpResponse& response,
                   const std::string& dir, const std::string& filename) {
        std::string decodedFilename = urlDecode(filename);

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
        if (std::string(resolvedPath).find(resolvedDir) != 0) {
            response = HttpResponse::badRequest("Invalid path");
            return;
        }

        FILE* fp = fopen(filepath.c_str(), "rb");
        if (!fp) {
            response = HttpResponse::notFound();
            return;
        }
        std::unique_ptr<FILE, decltype(&fclose)> fileGuard(fp, &fclose);

        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (size <= 0 || size > static_cast<long>(kMaxBodySize)) {
            response = HttpResponse::serverError("File too large");
            return;
        }

        std::string content(size, '\0');
        size_t readSize = fread(&content[0], 1, size, fp);
        if (readSize != static_cast<size_t>(size)) {
            response = HttpResponse::serverError("File read error");
            return;
        }

        // 根据扩展名设置 Content-Type
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

    // ==================== 成员变量 ====================

    mutable std::shared_mutex routesMutex_;

    /// 精确匹配路由（外层 key = path，内层 key = HttpMethod）
    std::unordered_map<std::string, std::unordered_map<int, HttpHandler>> exactRoutes_;
    /// 正则路由
    std::vector<Route> routes_;
    /// 中间件列表
    std::vector<HttpHandler> middlewares_;
    /// 静态目录映射（URL prefix -> 本地目录）
    std::unordered_map<std::string, std::string> staticDirs_;

    /// Gzip 响应压缩配置
    bool gzipEnabled_ = false;
    size_t gzipMinSize_ = 1024;
};
