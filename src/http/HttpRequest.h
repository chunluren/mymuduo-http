// HttpRequest.h - HTTP 请求解析
#pragma once

#include <string>
#include <unordered_map>
#include <algorithm>

enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    HEAD,
    UNKNOWN
};

enum class HttpVersion {
    HTTP_10,  // HTTP/1.0
    HTTP_11,  // HTTP/1.1
    UNKNOWN
};

class HttpRequest {
public:
    HttpMethod method;
    HttpVersion version;
    std::string path;
    std::string query;  // 查询字符串
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> params;  // POST 参数
    std::string body;
    
    HttpRequest() : method(HttpMethod::UNKNOWN), version(HttpVersion::UNKNOWN) {}
    
    // 解析请求行
    bool parseRequestLine(const std::string& line) {
        // GET /path?query HTTP/1.1
        size_t method_end = line.find(' ');
        if (method_end == std::string::npos) return false;
        
        std::string method_str = line.substr(0, method_end);
        method = stringToMethod(method_str);
        
        size_t path_start = method_end + 1;
        size_t path_end = line.find(' ', path_start);
        if (path_end == std::string::npos) return false;
        
        std::string full_path = line.substr(path_start, path_end - path_start);
        
        // 分离 path 和 query
        size_t query_pos = full_path.find('?');
        if (query_pos != std::string::npos) {
            path = full_path.substr(0, query_pos);
            query = full_path.substr(query_pos + 1);
            parseQuery(query);
        } else {
            path = full_path;
        }
        
        // 版本
        std::string version_str = line.substr(path_end + 1);
        version = stringToVersion(version_str);
        
        return true;
    }
    
    // 解析请求头
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
        
        // 转小写
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        headers[key] = value;
        
        return true;
    }
    
    // 获取 header
    std::string getHeader(const std::string& key) const {
        std::string lower_key = key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        auto it = headers.find(lower_key);
        return it != headers.end() ? it->second : "";
    }
    
    // 是否保持连接
    bool keepAlive() const {
        if (version == HttpVersion::HTTP_10) {
            return getHeader("connection") == "keep-alive";
        }
        return getHeader("connection") != "close";
    }
    
    // 获取 Content-Length
    size_t contentLength() const {
        std::string len = getHeader("content-length");
        return len.empty() ? 0 : std::stoul(len);
    }

private:
    HttpMethod stringToMethod(const std::string& s) {
        if (s == "GET") return HttpMethod::GET;
        if (s == "POST") return HttpMethod::POST;
        if (s == "PUT") return HttpMethod::PUT;
        if (s == "DELETE") return HttpMethod::DELETE;
        if (s == "HEAD") return HttpMethod::HEAD;
        return HttpMethod::UNKNOWN;
    }
    
    HttpVersion stringToVersion(const std::string& s) {
        if (s == "HTTP/1.0") return HttpVersion::HTTP_10;
        if (s == "HTTP/1.1") return HttpVersion::HTTP_11;
        return HttpVersion::UNKNOWN;
    }
    
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