/**
 * @file HttpResponse.h
 * @brief HTTP 响应构造类
 *
 * 本文件定义了 HttpResponse 类，用于构造 HTTP 响应。
 * 支持:
 * - 状态码设置
 * - 响应头管理
 * - 多种内容类型 (JSON、HTML、文本、二进制)
 * - Keep-Alive 连接管理
 * - 响应序列化
 *
 * @example 使用示例
 * @code
 * // 创建 JSON 响应
 * HttpResponse resp;
 * resp.setStatusCode(HttpStatusCode::OK);
 * resp.json(R"({"message": "Hello World"})");
 * std::string httpResp = resp.toString();
 *
 * // 使用静态方法快速创建响应
 * HttpResponse resp = HttpResponse::json(R"({"status": "ok"})");
 * HttpResponse resp = HttpResponse::notFound("Page not found");
 * HttpResponse resp = HttpResponse::badRequest("Invalid parameter");
 * @endcode
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdio>

/**
 * @enum HttpStatusCode
 * @brief HTTP 状态码枚举
 */
enum class HttpStatusCode {
    OK = 200,                    ///< 成功
    CREATED = 201,               ///< 已创建
    NO_CONTENT = 204,            ///< 无内容
    MOVED_PERMANENTLY = 301,     ///< 永久重定向
    FOUND = 302,                 ///< 临时重定向
    NOT_MODIFIED = 304,          ///< 未修改（协商缓存）
    BAD_REQUEST = 400,           ///< 错误请求
    UNAUTHORIZED = 401,          ///< 未认证
    FORBIDDEN = 403,             ///< 禁止访问
    NOT_FOUND = 404,             ///< 未找到
    METHOD_NOT_ALLOWED = 405,    ///< 方法不允许
    REQUEST_TIMEOUT = 408,       ///< 请求超时
    PAYLOAD_TOO_LARGE = 413,     ///< 请求体太大
    TOO_MANY_REQUESTS = 429,     ///< 请求过于频繁
    INTERNAL_SERVER_ERROR = 500, ///< 服务器内部错误
    BAD_GATEWAY = 502,           ///< 网关错误
    SERVICE_UNAVAILABLE = 503,   ///< 服务不可用
    GATEWAY_TIMEOUT = 504        ///< 网关超时
};

/**
 * @class HttpResponse
 * @brief HTTP 响应类
 *
 * 存储和序列化 HTTP 响应:
 * - 状态码和状态消息
 * - 响应头
 * - 响应体
 * - 连接控制
 *
 * 使用方法:
 * 1. 设置状态码 (setStatusCode 或静态方法)
 * 2. 设置响应头 (setContentType, setHeader)
 * 3. 设置响应体 (setBody, setJson, setHtml, setText)
 * 4. 序列化为字符串 (toString)
 */
class HttpResponse {
public:
    HttpStatusCode statusCode;                               ///< 状态码
    std::string statusMessage;                               ///< 状态消息
    std::unordered_map<std::string, std::string> headers;    ///< 响应头
    std::string body;                                        ///< 响应体
    bool closeConnection;                                    ///< 是否关闭连接
    bool chunked_ = false;                              ///< 是否 chunked
    std::vector<std::string> chunks_;                   ///< chunk 数据

    /**
     * @brief 默认构造函数
     *
     * 初始化状态码为 200 OK，closeConnection 为 false
     */
    HttpResponse() : statusCode(HttpStatusCode::OK), statusMessage("OK"), closeConnection(false) {}

    /**
     * @brief 设置状态码
     * @param code HTTP 状态码
     *
     * 自动设置对应的状态消息
     */
    void setStatusCode(HttpStatusCode code) {
        statusCode = code;
        switch (code) {
            case HttpStatusCode::OK:                    statusMessage = "OK"; break;
            case HttpStatusCode::CREATED:               statusMessage = "Created"; break;
            case HttpStatusCode::NO_CONTENT:             statusMessage = "No Content"; break;
            case HttpStatusCode::MOVED_PERMANENTLY:      statusMessage = "Moved Permanently"; break;
            case HttpStatusCode::FOUND:                  statusMessage = "Found"; break;
            case HttpStatusCode::NOT_MODIFIED:            statusMessage = "Not Modified"; break;
            case HttpStatusCode::BAD_REQUEST:            statusMessage = "Bad Request"; break;
            case HttpStatusCode::UNAUTHORIZED:           statusMessage = "Unauthorized"; break;
            case HttpStatusCode::FORBIDDEN:              statusMessage = "Forbidden"; break;
            case HttpStatusCode::NOT_FOUND:              statusMessage = "Not Found"; break;
            case HttpStatusCode::METHOD_NOT_ALLOWED:     statusMessage = "Method Not Allowed"; break;
            case HttpStatusCode::REQUEST_TIMEOUT:        statusMessage = "Request Timeout"; break;
            case HttpStatusCode::PAYLOAD_TOO_LARGE:      statusMessage = "Payload Too Large"; break;
            case HttpStatusCode::TOO_MANY_REQUESTS:      statusMessage = "Too Many Requests"; break;
            case HttpStatusCode::INTERNAL_SERVER_ERROR:  statusMessage = "Internal Server Error"; break;
            case HttpStatusCode::BAD_GATEWAY:            statusMessage = "Bad Gateway"; break;
            case HttpStatusCode::SERVICE_UNAVAILABLE:    statusMessage = "Service Unavailable"; break;
            case HttpStatusCode::GATEWAY_TIMEOUT:        statusMessage = "Gateway Timeout"; break;
        }
    }

    /**
     * @brief 设置 Content-Type 响应头
     * @param type MIME 类型
     *
     * @example
     * @code
     * response.setContentType("application/json");
     * response.setContentType("text/html; charset=utf-8");
     * @endcode
     */
    void setContentType(const std::string& type) {
        headers["Content-Type"] = type;
    }

    /**
     * @brief 设置 Content-Length 响应头
     * @param len 内容长度
     */
    void setContentLength(size_t len) {
        headers["Content-Length"] = std::to_string(len);
    }

    /**
     * @brief 设置自定义响应头
     * @param key 响应头键
     * @param value 响应头值
     */
    void setHeader(const std::string& key, const std::string& value) {
        headers[key] = value;
    }

    /**
     * @brief 设置响应体
     * @param b 响应体内容
     *
     * 自动设置 Content-Length
     */
    void setBody(const std::string& b) {
        body = b;
        setContentLength(body.size());
    }

    /**
     * @brief 设置 JSON 响应
     * @param json JSON 字符串
     *
     * 自动设置 Content-Type 为 application/json
     */
    void setJson(const std::string& json) {
        setContentType("application/json");
        setBody(json);
    }

    /**
     * @brief 设置 HTML 响应
     * @param html HTML 字符串
     *
     * 自动设置 Content-Type 为 text/html; charset=utf-8
     */
    void setHtml(const std::string& html) {
        setContentType("text/html; charset=utf-8");
        setBody(html);
    }

    /**
     * @brief 设置纯文本响应
     * @param text 文本内容
     *
     * 自动设置 Content-Type 为 text/plain; charset=utf-8
     */
    void setText(const std::string& text) {
        setContentType("text/plain; charset=utf-8");
        setBody(text);
    }

    /**
     * @brief 启用 Chunked Transfer Encoding
     * @param enabled 是否启用
     */
    void setChunked(bool enabled) {
        chunked_ = enabled;
    }

    /**
     * @brief 添加 chunk 数据
     * @param data chunk 内容，空字符串表示结束
     */
    void addChunk(const std::string& data) {
        chunks_.push_back(data);
    }

    /**
     * @brief 序列化为 HTTP 响应字符串
     * @return 完整的 HTTP 响应字符串
     *
     * 格式:
     * @code
     * HTTP/1.1 200 OK\r\n
     * Content-Type: application/json\r\n
     * Content-Length: 27\r\n
     * Connection: keep-alive\r\n
     * Server: mymuduo-http/1.0\r\n
     * \r\n
     * {"message": "Hello World"}
     * @endcode
     */
    std::string toString() const {
        std::string result;
        // Pre-reserve approximate size to avoid reallocations
        result.reserve(256 + body.size());

        // 状态行
        result += "HTTP/1.1 ";
        result += std::to_string(static_cast<int>(statusCode));
        result += ' ';
        result += statusMessage;
        result += "\r\n";

        // 响应头
        for (const auto& [key, value] : headers) {
            if (chunked_ && key == "Content-Length") continue;  // chunked 模式不输出 Content-Length
            result += key;
            result += ": ";
            result += value;
            result += "\r\n";
        }

        // Chunked 头
        if (chunked_) {
            result += "Transfer-Encoding: chunked\r\n";
        }

        // Connection 头
        result += "Connection: ";
        result += (closeConnection ? "close" : "keep-alive");
        result += "\r\n";

        // Server 头
        result += "Server: mymuduo-http/1.0\r\n";

        // 空行
        result += "\r\n";

        // 响应体
        if (chunked_ && !chunks_.empty()) {
            char hexbuf[32];
            for (const auto& chunk : chunks_) {
                if (chunk.empty()) {
                    result += "0\r\n\r\n";  // 结束标记
                } else {
                    int n = snprintf(hexbuf, sizeof(hexbuf), "%zx\r\n", chunk.size());
                    result.append(hexbuf, n);
                    result += chunk;
                    result += "\r\n";
                }
            }
        } else {
            result += body;
        }

        return result;
    }

    // ==================== 静态工厂方法 ====================

    /**
     * @brief 创建 200 OK 响应
     * @param body 响应体 (可选)
     * @return HttpResponse 对象
     */
    static HttpResponse ok(const std::string& body = "") {
        HttpResponse resp;
        resp.setStatusCode(HttpStatusCode::OK);
        if (!body.empty()) resp.setBody(body);
        return resp;
    }

    /**
     * @brief 创建 JSON 响应
     * @param json JSON 字符串
     * @return HttpResponse 对象
     */
    static HttpResponse json(const std::string& json) {
        HttpResponse resp;
        resp.setStatusCode(HttpStatusCode::OK);
        resp.setJson(json);
        return resp;
    }

    /**
     * @brief 创建 404 Not Found 响应
     * @param msg 错误消息 (可选)
     * @return HttpResponse 对象
     */
    static HttpResponse notFound(const std::string& msg = "Not Found") {
        HttpResponse resp;
        resp.setStatusCode(HttpStatusCode::NOT_FOUND);
        resp.setText(msg);
        return resp;
    }

    /**
     * @brief 创建 400 Bad Request 响应
     * @param msg 错误消息 (可选)
     * @return HttpResponse 对象
     */
    static HttpResponse badRequest(const std::string& msg = "Bad Request") {
        HttpResponse resp;
        resp.setStatusCode(HttpStatusCode::BAD_REQUEST);
        resp.setText(msg);
        return resp;
    }

    /**
     * @brief 创建 500 Internal Server Error 响应
     * @param msg 错误消息 (可选)
     * @return HttpResponse 对象
     */
    static HttpResponse serverError(const std::string& msg = "Internal Server Error") {
        HttpResponse resp;
        resp.setStatusCode(HttpStatusCode::INTERNAL_SERVER_ERROR);
        resp.setText(msg);
        return resp;
    }

    /**
     * @brief 创建重定向响应
     * @param url 重定向目标 URL
     * @param code 状态码（默认 302 临时重定向）
     */
    static HttpResponse redirect(const std::string& url,
                                  HttpStatusCode code = HttpStatusCode::FOUND) {
        HttpResponse resp;
        resp.setStatusCode(code);
        resp.setHeader("Location", url);
        resp.setBody("");
        return resp;
    }

    // ==================== CORS ====================

    /**
     * @brief 设置 CORS 响应头
     * @param origin 允许的源（默认 "*"）
     */
    void setCors(const std::string& origin = "*") {
        headers["Access-Control-Allow-Origin"] = origin;
        headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS";
        headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization";
        headers["Access-Control-Max-Age"] = "86400";
    }

    // ==================== Cookie ====================

    /**
     * @brief 设置 Cookie
     * @param name Cookie 名
     * @param value Cookie 值
     * @param maxAge 过期时间（秒），-1 表示会话 Cookie
     * @param path 作用路径
     * @param httpOnly 是否 HttpOnly（JS 不可读）
     * @param secure 是否仅 HTTPS 传输
     */
    void setCookie(const std::string& name, const std::string& value,
                   int maxAge = -1, const std::string& path = "/",
                   bool httpOnly = true, bool secure = false) {
        std::string cookie = name + "=" + value;
        cookie += "; Path=" + path;
        if (maxAge >= 0) {
            cookie += "; Max-Age=" + std::to_string(maxAge);
        }
        if (httpOnly) cookie += "; HttpOnly";
        if (secure) cookie += "; Secure";
        headers["Set-Cookie"] = cookie;
    }
};