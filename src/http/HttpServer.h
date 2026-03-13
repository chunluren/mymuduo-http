// HttpServer.h - HTTP 服务器
#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "../TcpServer.h"
#include "../EventLoop.h"
#include "../Buffer.h"
#include <functional>
#include <unordered_map>
#include <regex>

// 请求处理函数类型
using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

// 路由项
struct Route {
    HttpMethod method;
    std::string pattern;  // 支持正则
    HttpHandler handler;
    std::regex regex;
    
    Route(HttpMethod m, const std::string& p, HttpHandler h)
        : method(m), pattern(p), handler(h), regex(p) {}
};

class HttpServer {
public:
    HttpServer(EventLoop* loop, const InetAddress& addr, const std::string& name = "HttpServer")
        : server_(loop, addr, name)
    {
        server_.setConnectionCallback([this](const TcpConnectionPtr& conn) {
            onConnection(conn);
        });
        server_.setMessageCallback([this](const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
            onMessage(conn, buf, time);
        });
        server_.setThreadNum(4);
    }
    
    void setThreadNum(int num) { server_.setThreadNum(num); }
    void start() { server_.start(); }
    
    // 路由注册
    void GET(const std::string& path, HttpHandler handler) {
        routes_.push_back({HttpMethod::GET, path, handler});
    }
    
    void POST(const std::string& path, HttpHandler handler) {
        routes_.push_back({HttpMethod::POST, path, handler});
    }
    
    void PUT(const std::string& path, HttpHandler handler) {
        routes_.push_back({HttpMethod::PUT, path, handler});
    }
    
    void DELETE(const std::string& path, HttpHandler handler) {
        routes_.push_back({HttpMethod::DELETE, path, handler});
    }
    
    // 静态文件服务
    void serveStatic(const std::string& urlPrefix, const std::string& dir) {
        staticDirs_[urlPrefix] = dir;
    }
    
    // 中间件（全局处理）
    void use(HttpHandler middleware) {
        middlewares_.push_back(middleware);
    }
    
private:
    TcpServer server_;
    std::vector<Route> routes_;
    std::vector<HttpHandler> middlewares_;
    std::unordered_map<std::string, std::string> staticDirs_;
    
    void onConnection(const TcpConnectionPtr& conn) {
        // 可以在这里记录连接状态
    }
    
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
        // 解析 HTTP 请求
        HttpRequest request;
        if (!parseRequest(buf, request)) {
            // 解析失败，发送 400
            HttpResponse resp = HttpResponse::badRequest("Invalid HTTP Request");
            resp.closeConnection = true;
            conn->send(resp.toString());
            conn->shutdown();
            return;
        }
        
        // 检查是否接收完整
        size_t contentLength = request.contentLength();
        if (contentLength > 0 && request.body.size() < contentLength) {
            // 等待更多数据
            return;
        }
        
        // 处理请求
        HttpResponse response;
        handleRequest(request, response);
        
        // 发送响应
        response.closeConnection = !request.keepAlive();
        conn->send(response.toString());
        
        if (response.closeConnection) {
            conn->shutdown();
        }
    }
    
    bool parseRequest(Buffer* buf, HttpRequest& request) {
        std::string data = buf->retrieveAllAsString();
        
        // 找到请求头的结束位置
        size_t headerEnd = data.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            return false;  // 请求头不完整
        }
        
        // 解析请求头
        std::string header = data.substr(0, headerEnd);
        size_t lineEnd = header.find("\r\n");
        
        if (lineEnd == std::string::npos) {
            return false;
        }
        
        // 请求行
        std::string requestLine = header.substr(0, lineEnd);
        if (!request.parseRequestLine(requestLine)) {
            return false;
        }
        
        // 请求头字段
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
        
        // 请求体
        request.body = data.substr(headerEnd + 4);
        
        return true;
    }
    
    void handleRequest(const HttpRequest& request, HttpResponse& response) {
        // 执行中间件
        for (auto& middleware : middlewares_) {
            middleware(request, response);
        }
        
        // 路由匹配
        for (const auto& route : routes_) {
            if (route.method == request.method && 
                std::regex_match(request.path, route.regex)) {
                route.handler(request, response);
                return;
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
    
    void serveFile(const HttpRequest& request, HttpResponse& response, 
                   const std::string& dir, const std::string& filename) {
        // 简化：返回文件内容（实际应该检查文件是否存在、权限等）
        std::string filepath = dir + filename;
        
        FILE* fp = fopen(filepath.c_str(), "rb");
        if (!fp) {
            response = HttpResponse::notFound();
            return;
        }
        
        // 读取文件
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        std::string content(size, '\0');
        fread(&content[0], 1, size, fp);
        fclose(fp);
        
        // 设置 Content-Type
        if (filename.find(".html") != std::string::npos) {
            response.setContentType("text/html");
        } else if (filename.find(".css") != std::string::npos) {
            response.setContentType("text/css");
        } else if (filename.find(".js") != std::string::npos) {
            response.setContentType("application/javascript");
        } else if (filename.find(".json") != std::string::npos) {
            response.setContentType("application/json");
        } else {
            response.setContentType("application/octet-stream");
        }
        
        response.setBody(content);
    }
};