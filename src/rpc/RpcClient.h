// RpcClient.h - RPC 客户端
#pragma once

#include <string>
#include <unordered_map>
#include <atomic>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <future>

using json = nlohmann::json;

class RpcClient {
public:
    RpcClient(const std::string& host, int port)
        : host_(host), port_(port), nextId_(1)
    {}
    
    // 同步调用
    json call(const std::string& method, const json& params = json()) {
        int sock = connect();
        if (sock < 0) {
            return {{"error", "connection failed"}};
        }
        
        // 构造请求
        json request;
        request["jsonrpc"] = "2.0";
        request["method"] = method;
        request["params"] = params;
        request["id"] = nextId_++;
        
        // 发送 HTTP 请求
        std::string body = request.dump();
        std::string httpReq = "POST /rpc HTTP/1.1\r\n"
                             "Host: " + host_ + "\r\n"
                             "Content-Type: application/json\r\n"
                             "Content-Length: " + std::to_string(body.size()) + "\r\n"
                             "\r\n" + body;
        
        send(sock, httpReq.c_str(), httpReq.size(), 0);
        
        // 接收响应
        char buf[4096];
        std::string response;
        ssize_t n;
        while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[n] = '\0';
            response += buf;
        }
        
        close(sock);
        
        // 解析 HTTP 响应
        size_t bodyStart = response.find("\r\n\r\n");
        if (bodyStart == std::string::npos) {
            return {{"error", "invalid response"}};
        }
        
        std::string respBody = response.substr(bodyStart + 4);

        json resp;
        try {
            resp = json::parse(respBody);
        } catch (const json::parse_error& e) {
            return {{"error", "JSON parse error"}};
        }

        if (resp.contains("error")) {
            return resp["error"];
        }
        
        return resp["result"];
    }
    
    // 异步调用（返回 future）
    std::future<json> asyncCall(const std::string& method, const json& params = json()) {
        return std::async(std::launch::async, [this, method, params]() {
            return call(method, params);
        });
    }

private:
    std::string host_;
    int port_;
    std::atomic<int> nextId_;
    
    int connect() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;
        
        // 解析主机名
        hostent* host = gethostbyname(host_.c_str());
        if (!host) {
            close(sock);
            return -1;
        }
        
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr = *((in_addr*)host->h_addr);
        
        if (::connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            return -1;
        }
        
        return sock;
    }
};