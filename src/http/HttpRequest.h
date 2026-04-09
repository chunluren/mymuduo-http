/**
 * @file HttpRequest.h
 * @brief HTTP 请求解析类
 *
 * 本文件定义了 HttpRequest 类，用于解析和存储 HTTP 请求信息。
 * 支持 HTTP/1.0 和 HTTP/1.1 协议。
 *
 * @example 使用示例
 * @code
 * HttpRequest request;
 *
 * // 解析请求行: "GET /path?query=value HTTP/1.1"
 * request.parseRequestLine("GET /path?query=value HTTP/1.1");
 *
 * // 解析请求头: "Content-Type: application/json"
 * request.parseHeader("Content-Type: application/json");
 *
 * // 获取信息
 * std::string path = request.path;        // "/path"
 * std::string query = request.query;      // "query=value"
 * std::string contentType = request.getHeader("content-type");  // "application/json"
 * bool keepAlive = request.keepAlive();   // true (HTTP/1.1 默认)
 * @endcode
 */

#pragma once

#include <string>
#include <unordered_map>
#include <algorithm>

/**
 * @enum HttpMethod
 * @brief HTTP 方法枚举
 */
enum class HttpMethod {
    GET,      ///< GET 方法，获取资源
    POST,     ///< POST 方法，提交数据
    PUT,      ///< PUT 方法，更新资源
    DELETE,   ///< DELETE 方法，删除资源
    HEAD,     ///< HEAD 方法，获取响应头
    UNKNOWN   ///< 未知方法
};

/**
 * @enum HttpVersion
 * @brief HTTP 版本枚举
 */
enum class HttpVersion {
    HTTP_10,  ///< HTTP/1.0
    HTTP_11,  ///< HTTP/1.1
    UNKNOWN   ///< 未知版本
};

/**
 * @class HttpRequest
 * @brief HTTP 请求类
 *
 * 存储和解析 HTTP 请求的所有信息:
 * - 请求方法、路径、查询字符串
 * - HTTP 版本
 * - 请求头
 * - 请求体
 * - 解析参数
 *
 * 使用方法:
 * 1. 调用 parseRequestLine() 解析请求行
 * 2. 多次调用 parseHeader() 解析请求头
 * 3. 设置 body
 * 4. 使用 getHeader()、keepAlive() 等方法获取信息
 */
class HttpRequest {
public:
    HttpMethod method;                                      ///< HTTP 方法
    HttpVersion version;                                    ///< HTTP 版本
    std::string path;                                       ///< 请求路径
    std::string query;                                      ///< 查询字符串
    std::unordered_map<std::string, std::string> headers;   ///< 请求头 (小写键)
    std::unordered_map<std::string, std::string> params;    ///< POST 参数
    std::string body;                                       ///< 请求体

    /**
     * @brief 默认构造函数
     *
     * 初始化 method 和 version 为 UNKNOWN
     */
    HttpRequest() : method(HttpMethod::UNKNOWN), version(HttpVersion::UNKNOWN) {}

    /**
     * @brief 解析请求行
     * @param line 请求行字符串，如 "GET /path?query HTTP/1.1"
     * @return 解析是否成功
     *
     * 请求行格式: METHOD PATH HTTP/VERSION
     *
     * @example
     * @code
     * request.parseRequestLine("GET /index.html?name=test HTTP/1.1");
     * // 结果:
     * // method = HttpMethod::GET
     * // path = "/index.html"
     * // query = "name=test"
     * // params = {"name": "test"}
     * // version = HttpVersion::HTTP_11
     * @endcode
     */
    bool parseRequestLine(const std::string& line) {
        // 格式: GET /path?query HTTP/1.1
        size_t method_end = line.find(' ');
        if (method_end == std::string::npos) return false;

        // 解析方法
        std::string method_str = line.substr(0, method_end);
        method = stringToMethod(method_str);

        // 找路径开始和结束位置
        size_t path_start = method_end + 1;
        size_t path_end = line.find(' ', path_start);
        if (path_end == std::string::npos) return false;

        // 提取完整路径 (包含查询字符串)
        std::string full_path = line.substr(path_start, path_end - path_start);

        // 分离 path 和 query
        size_t query_pos = full_path.find('?');
        if (query_pos != std::string::npos) {
            path = full_path.substr(0, query_pos);
            query = full_path.substr(query_pos + 1);
            parseQuery(query);  // 解析查询参数
        } else {
            path = full_path;
        }

        // 解析版本
        std::string version_str = line.substr(path_end + 1);
        version = stringToVersion(version_str);

        return true;
    }

    /**
     * @brief 解析请求头
     * @param line 请求头行，如 "Content-Type: application/json"
     * @return 解析是否成功
     *
     * @note 键会被转换为小写，便于后续查询
     */
    bool parseHeader(const std::string& line) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) return false;

        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        // 去除前导空格
        size_t start = value.find_first_not_of(' ');
        if (start != std::string::npos) {
            value = value.substr(start);
        }

        // 转小写 (正确处理负值 char，避免未定义行为)
        std::transform(key.begin(), key.end(), key.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        headers[key] = value;

        return true;
    }

    /**
     * @brief 获取请求头值
     * @param key 请求头键名 (大小写不敏感)
     * @return 请求头值，不存在则返回空字符串
     *
     * @example
     * @code
     * std::string contentType = request.getHeader("Content-Type");
     * // 或
     * std::string contentType = request.getHeader("content-type");
     * // 两种写法等效
     * @endcode
     */
    std::string getHeader(const std::string& key) const {
        std::string lower_key = key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto it = headers.find(lower_key);
        return it != headers.end() ? it->second : "";
    }

    /**
     * @brief 判断是否保持连接
     * @return true 如果应该保持连接
     *
     * HTTP/1.0: 默认关闭，需 Connection: keep-alive 才保持
     * HTTP/1.1: 默认保持，需 Connection: close 才关闭
     */
    bool keepAlive() const {
        if (version == HttpVersion::HTTP_10) {
            return getHeader("connection") == "keep-alive";
        }
        return getHeader("connection") != "close";
    }

    /**
     * @brief 获取 Content-Length
     * @return Content-Length 值，无效或不存在则返回 0
     */
    size_t contentLength() const {
        std::string len = getHeader("content-length");
        if (len.empty()) return 0;
        try {
            return std::stoul(len);
        } catch (...) {
            return 0;
        }
    }

    /**
     * @brief 获取查询参数值
     * @param key 参数名
     * @param defaultVal 默认值
     * @return 参数值，不存在则返回默认值
     */
    std::string getParam(const std::string& key, const std::string& defaultVal = "") const {
        auto it = params.find(key);
        return it != params.end() ? it->second : defaultVal;
    }

    // ==================== Cookie 解析 ====================

    /**
     * @brief 解析所有 Cookie
     * @return Cookie 键值对映射
     *
     * 解析 Cookie 头: "session=abc123; theme=dark; lang=zh"
     */
    std::unordered_map<std::string, std::string> cookies() const {
        std::unordered_map<std::string, std::string> result;
        std::string cookieHeader = getHeader("cookie");
        if (cookieHeader.empty()) return result;

        size_t start = 0;
        while (start < cookieHeader.size()) {
            size_t semi = cookieHeader.find(';', start);
            std::string pair;
            if (semi == std::string::npos) {
                pair = cookieHeader.substr(start);
                start = cookieHeader.size();
            } else {
                pair = cookieHeader.substr(start, semi - start);
                start = semi + 1;
            }

            // 去除前导空格
            size_t pairStart = pair.find_first_not_of(' ');
            if (pairStart == std::string::npos) continue;
            pair = pair.substr(pairStart);

            size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                result[pair.substr(0, eq)] = pair.substr(eq + 1);
            }
        }
        return result;
    }

    /**
     * @brief 获取单个 Cookie 值
     * @param name Cookie 名
     * @return Cookie 值，不存在返回空字符串
     */
    std::string cookie(const std::string& name) const {
        auto all = cookies();
        auto it = all.find(name);
        return it != all.end() ? it->second : "";
    }

private:
    /**
     * @brief 字符串转 HTTP 方法
     * @param s 方法字符串
     * @return HttpMethod 枚举值
     */
    HttpMethod stringToMethod(const std::string& s) {
        if (s == "GET") return HttpMethod::GET;
        if (s == "POST") return HttpMethod::POST;
        if (s == "PUT") return HttpMethod::PUT;
        if (s == "DELETE") return HttpMethod::DELETE;
        if (s == "HEAD") return HttpMethod::HEAD;
        return HttpMethod::UNKNOWN;
    }

    /**
     * @brief 字符串转 HTTP 版本
     * @param s 版本字符串
     * @return HttpVersion 枚举值
     */
    HttpVersion stringToVersion(const std::string& s) {
        if (s == "HTTP/1.0") return HttpVersion::HTTP_10;
        if (s == "HTTP/1.1") return HttpVersion::HTTP_11;
        return HttpVersion::UNKNOWN;
    }

    /**
     * @brief 解析查询字符串
     * @param q 查询字符串，如 "name=Alice&age=20"
     *
     * 将查询参数解析到 params 映射表中
     */
    void parseQuery(const std::string& q) {
        size_t start = 0;
        while (start < q.size()) {
            size_t eq = q.find('=', start);
            size_t amp = q.find('&', start);

            if (eq == std::string::npos) break;

            std::string key = q.substr(start, eq - start);
            std::string value;

            if (amp == std::string::npos) {
                value = q.substr(eq + 1);
                start = q.size();
            } else {
                value = q.substr(eq + 1, amp - eq - 1);
                start = amp + 1;
            }

            params[key] = value;
        }
    }
};