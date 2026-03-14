// HttpServer.h - HTTP 服务器（修复版）
#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "net/TcpServer.h"
#include "net/EventLoop.h"
#include "net/Buffer.h"
#include <functional>
#include <unordered_map>
#include <regex>
#include <atomic>

// 请求处理函数类型
using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

// 路由项
struct Route {
    HttpMethod method;
    std::string pattern;
    HttpHandler handler;
    std::regex regex;
    
    Route(HttpMethod m, const std::string& p, HttpHandler h)
        : method(m), pattern(p), handler(h), regex(p) {}
};

// HTTP 服务器（修复版）
class HttpServer {
public:
    // 最大请求体大小（10MB）
    static constexpr size_t kMaxBodySize = 10 * 1024 * 1024;
    
    HttpServer(EventLoop* loop, const InetAddress& addr, const std::string& name = "HttpServer")
        : server_(loop, addr, name)
        , started_(false)
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
    
    void start() {
        started_.store(true);
        server_.start();
    }
    
    // 路由注册（启动前调用）
    void GET(const std::string& path, HttpHandler handler) {
        if (started_.load()) return;  // 启动后不允许注册
        routes_.push_back({HttpMethod::GET, path, handler});
    }
    
    void POST(const std::string& path, HttpHandler handler) {
        if (started_.load()) return;
        routes_.push_back({HttpMethod::POST, path, handler});
    }
    
    void PUT(const std::string& path, HttpHandler handler) {
        if (started_.load()) return;
        routes_.push_back({HttpMethod::PUT, path, handler});
    }
    
    void DELETE(const std::string& path, HttpHandler handler) {
        if (started_.load()) return;
        routes_.push_back({HttpMethod::DELETE, path, handler});
    }
    
    void serveStatic(const std::string& urlPrefix, const std::string& dir) {
        if (started_.load()) return;
        staticDirs_[urlPrefix] = dir;
    }
    
    void use(HttpHandler middleware) {
        if (started_.load()) return;
        middlewares_.push_back(middleware);
    }

private:
    TcpServer server_;
    std::vector<Route> routes_;
    std::vector<HttpHandler> middlewares_;
    std::unordered_map<std::string, std::string> staticDirs_;
    std::atomic<bool> started_;
    
    void onConnection(const TcpConnectionPtr& /*conn*/) {
        // 可以记录连接状态
    }
    
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
                // Content-Length 解析失败，忽略
                contentLen = 0;
            }
            
            if (contentLen > kMaxBodySize) {
                HttpResponse resp = HttpResponse::badRequest("Request body too large");
                resp.closeConnection = true;
                conn->send(resp.toString());
                conn->shutdown();
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
                return;
            }
            
            // 继续处理可能的下一个请求（流水线）
        }
    }
    
    enum class ParseResult {
        Complete,    // 解析完成
        Incomplete,  // 数据不完整
        Error        // 解析错误
    };
    
    // 改进的解析函数（支持粘包）
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
    
    bool parseHeader(const std::string& header, HttpRequest& request) {
        // 找请求行
        size_t lineEnd = header.find("\r\n");
        if (lineEnd == std::string::npos) {
            return false;
        }
        
        // 解析请求行
        std::string requestLine = header.substr(0, lineEnd);
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
    
    void handleRequest(const HttpRequest& request, HttpResponse& response) {
        // 执行中间件
        for (auto& middleware : middlewares_) {
            middleware(request, response);
        }
        
        // 路由匹配
        for (const auto& route : routes_) {
            if (route.method == request.method) {
                try {
                    if (std::regex_match(request.path, route.regex)) {
                        route.handler(request, response);
                        return;
                    }
                } catch (const std::regex_error& e) {
                    response = HttpResponse::serverError("Regex error");
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
    
    void serveFile(const HttpRequest& /*request*/, HttpResponse& response, 
                   const std::string& dir, const std::string& filename) {
        // 安全检查：防止路径遍历攻击
        if (filename.empty() || 
            filename.find("..") != std::string::npos ||
            filename[0] == '/' ||
            filename[0] == '~') {
            response = HttpResponse::badRequest("Invalid path");
            return;
        }
        
        std::string filepath = dir + "/" + filename;
        
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