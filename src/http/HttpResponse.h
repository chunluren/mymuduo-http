// HttpResponse.h - HTTP 响应
#pragma once

#include <string>
#include <unordered_map>
#include <sstream>

enum class HttpStatusCode {
    OK = 200,
    CREATED = 201,
    NO_CONTENT = 204,
    BAD_REQUEST = 400,
    NOT_FOUND = 404,
    INTERNAL_SERVER_ERROR = 500
};

class HttpResponse {
public:
    HttpStatusCode statusCode;
    std::string statusMessage;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool closeConnection;
    
    HttpResponse() : statusCode(HttpStatusCode::OK), closeConnection(false) {}
    
    void setStatusCode(HttpStatusCode code) {
        statusCode = code;
        switch (code) {
            case HttpStatusCode::OK: statusMessage = "OK"; break;
            case HttpStatusCode::CREATED: statusMessage = "Created"; break;
            case HttpStatusCode::NO_CONTENT: statusMessage = "No Content"; break;
            case HttpStatusCode::BAD_REQUEST: statusMessage = "Bad Request"; break;
            case HttpStatusCode::NOT_FOUND: statusMessage = "Not Found"; break;
            case HttpStatusCode::INTERNAL_SERVER_ERROR: statusMessage = "Internal Server Error"; break;
        }
    }
    
    void setContentType(const std::string& type) {
        headers["Content-Type"] = type;
    }
    
    void setContentLength(size_t len) {
        headers["Content-Length"] = std::to_string(len);
    }
    
    void setHeader(const std::string& key, const std::string& value) {
        headers[key] = value;
    }
    
    void setBody(const std::string& b) {
        body = b;
        setContentLength(body.size());
    }
    
    void setJson(const std::string& json) {
        setContentType("application/json");
        setBody(json);
    }
    
    void setHtml(const std::string& html) {
        setContentType("text/html; charset=utf-8");
        setBody(html);
    }
    
    void setText(const std::string& text) {
        setContentType("text/plain; charset=utf-8");
        setBody(text);
    }
    
    // 序列化为字符串
    std::string toString() const {
        std::ostringstream oss;
        
        // 状态行
        oss << "HTTP/1.1 " << static_cast<int>(statusCode) << " " << statusMessage << "\r\n";
        
        // 响应头
        for (const auto& [key, value] : headers) {
            oss << key << ": " << value << "\r\n";
        }
        
        // Connection
        oss << "Connection: " << (closeConnection ? "close" : "keep-alive") << "\r\n";
        
        // Server
        oss << "Server: mymuduo-http/1.0\r\n";
        
        // 空行
        oss << "\r\n";
        
        // 响应体
        oss << body;
        
        return oss.str();
    }
    
    // 快捷方法
    static HttpResponse ok(const std::string& body = "") {
        HttpResponse resp;
        resp.setStatusCode(HttpStatusCode::OK);
        if (!body.empty()) resp.setBody(body);
        return resp;
    }
    
    static HttpResponse json(const std::string& json) {
        HttpResponse resp;
        resp.setStatusCode(HttpStatusCode::OK);
        resp.setJson(json);
        return resp;
    }
    
    static HttpResponse notFound(const std::string& msg = "Not Found") {
        HttpResponse resp;
        resp.setStatusCode(HttpStatusCode::NOT_FOUND);
        resp.setText(msg);
        return resp;
    }
    
    static HttpResponse badRequest(const std::string& msg = "Bad Request") {
        HttpResponse resp;
        resp.setStatusCode(HttpStatusCode::BAD_REQUEST);
        resp.setText(msg);
        return resp;
    }
    
    static HttpResponse serverError(const std::string& msg = "Internal Server Error") {
        HttpResponse resp;
        resp.setStatusCode(HttpStatusCode::INTERNAL_SERVER_ERROR);
        resp.setText(msg);
        return resp;
    }
};