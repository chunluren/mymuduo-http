// RegistryClient.h - 服务注册中心客户端 SDK
// 提供服务注册、发现、心跳等功能

#pragma once

#include "ServiceMeta.h"
#include "../balancer/LoadBalancer.h"
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// HTTP 客户端（简单实现，用于与注册中心通信）
class SimpleHttpClient {
public:
    struct Response {
        int statusCode;
        std::string body;
        bool success;
    };

    // 发送 POST 请求
    static Response post(const std::string& host, int port,
                         const std::string& path, const std::string& body,
                         const std::string& contentType = "application/json") {
        Response resp;
        resp.success = false;
        resp.statusCode = 0;

        // 创建 socket
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            resp.body = "Socket creation failed";
            return resp;
        }

        // 设置超时
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        // 连接服务器
        struct sockaddr_in serverAddr;
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr);

        if (connect(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
            close(sockfd);
            resp.body = "Connection failed";
            return resp;
        }

        // 构造 HTTP 请求
        std::string request = "POST " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + "\r\n";
        request += "Content-Type: " + contentType + "\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        request += "Connection: close\r\n";
        request += "\r\n";
        request += body;

        // 发送请求
        if (send(sockfd, request.c_str(), request.size(), 0) < 0) {
            close(sockfd);
            resp.body = "Send failed";
            return resp;
        }

        // 接收响应
        char buffer[4096];
        std::string response;
        ssize_t bytesRead;
        while ((bytesRead = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
            buffer[bytesRead] = '\0';
            response += buffer;
        }
        close(sockfd);

        // 解析响应
        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            resp.body = "Invalid HTTP response";
            return resp;
        }

        // 解析状态码
        std::string statusLine = response.substr(0, response.find("\r\n"));
        if (sscanf(statusLine.c_str(), "HTTP/1.%*d %d", &resp.statusCode) != 1) {
            resp.body = "Invalid status line";
            return resp;
        }

        resp.body = response.substr(headerEnd + 4);
        resp.success = (resp.statusCode >= 200 && resp.statusCode < 300);
        return resp;
    }

    // 发送 GET 请求
    static Response get(const std::string& host, int port,
                        const std::string& path) {
        Response resp;
        resp.success = false;
        resp.statusCode = 0;

        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            resp.body = "Socket creation failed";
            return resp;
        }

        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        struct sockaddr_in serverAddr;
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr);

        if (connect(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
            close(sockfd);
            resp.body = "Connection failed";
            return resp;
        }

        std::string request = "GET " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + "\r\n";
        request += "Connection: close\r\n";
        request += "\r\n";

        if (send(sockfd, request.c_str(), request.size(), 0) < 0) {
            close(sockfd);
            resp.body = "Send failed";
            return resp;
        }

        char buffer[4096];
        std::string response;
        ssize_t bytesRead;
        while ((bytesRead = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
            buffer[bytesRead] = '\0';
            response += buffer;
        }
        close(sockfd);

        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            resp.body = "Invalid HTTP response";
            return resp;
        }

        std::string statusLine = response.substr(0, response.find("\r\n"));
        if (sscanf(statusLine.c_str(), "HTTP/1.%*d %d", &resp.statusCode) != 1) {
            resp.body = "Invalid status line";
            return resp;
        }

        resp.body = response.substr(headerEnd + 4);
        resp.success = (resp.statusCode >= 200 && resp.statusCode < 300);
        return resp;
    }
};

// 服务注册客户端
class RegistryClient {
public:
    using ServiceChangedCallback = std::function<void(const ServiceKey&, const std::vector<InstancePtr>&)>;

    RegistryClient(const std::string& registryHost, int registryPort)
        : registryHost_(registryHost)
        , registryPort_(registryPort)
        , heartbeatIntervalMs_(10000)  // 默认10秒心跳
        , running_(false)
    {}

    ~RegistryClient() {
        stop();
    }

    // 注册服务实例
    bool registerService(const ServiceKey& key, const InstanceMeta& instance, std::string* outInstanceId = nullptr) {
        json body;
        body["service"] = key.toJson();
        body["instance"] = instance.toJson();

        auto resp = SimpleHttpClient::post(registryHost_, registryPort_,
                                           "/api/v1/registry/register", body.dump());
        if (!resp.success) {
            return false;
        }

        try {
            json result = json::parse(resp.body);
            if (result.value("success", false)) {
                if (outInstanceId) {
                    *outInstanceId = result.value("instanceId", "");
                }
                return true;
            }
        } catch (...) {}

        return false;
    }

    // 注销服务实例
    bool deregisterService(const ServiceKey& key, const std::string& instanceId) {
        json body;
        body["service"] = key.toJson();
        body["instanceId"] = instanceId;

        auto resp = SimpleHttpClient::post(registryHost_, registryPort_,
                                           "/api/v1/registry/deregister", body.dump());
        if (!resp.success) {
            return false;
        }

        try {
            json result = json::parse(resp.body);
            return result.value("success", false);
        } catch (...) {}

        return false;
    }

    // 发送心跳
    bool sendHeartbeat(const ServiceKey& key, const std::string& instanceId) {
        json body;
        body["service"] = key.toJson();
        body["instanceId"] = instanceId;

        auto resp = SimpleHttpClient::post(registryHost_, registryPort_,
                                           "/api/v1/registry/heartbeat", body.dump());
        if (!resp.success) {
            return false;
        }

        try {
            json result = json::parse(resp.body);
            return result.value("success", false);
        } catch (...) {}

        return false;
    }

    // 发现服务
    std::vector<InstancePtr> discoverService(const ServiceKey& key) {
        std::string path = "/api/v1/registry/discover?namespace=" + key.namespace_ +
                          "&serviceName=" + key.serviceName +
                          "&version=" + key.version;

        auto resp = SimpleHttpClient::get(registryHost_, registryPort_, path);
        if (!resp.success) {
            return {};
        }

        std::vector<InstancePtr> instances;
        try {
            json result = json::parse(resp.body);
            if (result.value("success", false) && result.contains("instances")) {
                for (const auto& instJson : result["instances"]) {
                    auto instance = std::make_shared<InstanceMeta>(InstanceMeta::fromJson(instJson));
                    instances.push_back(instance);
                }
            }
        } catch (...) {}

        return instances;
    }

    // 启动自动心跳
    void startHeartbeat(const ServiceKey& key, const std::string& instanceId) {
        if (running_.exchange(true)) {
            return;  // 已经在运行
        }

        serviceKey_ = key;
        instanceId_ = instanceId;

        heartbeatThread_ = std::thread([this]() {
            heartbeatLoop();
        });
    }

    // 停止自动心跳
    void stop() {
        if (!running_.exchange(false)) {
            return;
        }

        cv_.notify_all();
        if (heartbeatThread_.joinable()) {
            heartbeatThread_.join();
        }
    }

    // 设置心跳间隔
    void setHeartbeatInterval(int ms) {
        heartbeatIntervalMs_ = ms;
    }

    // 获取单个服务实例（使用负载均衡）
    InstancePtr selectInstance(const ServiceKey& key, LoadBalancer::Strategy strategy = LoadBalancer::Strategy::RoundRobin) {
        auto instances = discoverService(key);
        if (instances.empty()) {
            return nullptr;
        }

        // 转换为 BackendServer 格式
        LoadBalancer balancer(strategy);
        for (const auto& inst : instances) {
            balancer.addServer(inst->host, inst->port, inst->weight);
        }

        auto selected = balancer.select();
        if (!selected) {
            return nullptr;
        }

        // 找到对应的 InstancePtr
        for (const auto& inst : instances) {
            if (inst->host == selected->host && inst->port == selected->port) {
                return inst;
            }
        }

        return nullptr;
    }

    // 检查注册中心健康状态
    bool isRegistryHealthy() {
        auto resp = SimpleHttpClient::get(registryHost_, registryPort_, "/api/v1/registry/health");
        return resp.success && resp.statusCode == 200;
    }

private:
    void heartbeatLoop() {
        while (running_) {
            sendHeartbeat(serviceKey_, instanceId_);

            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(heartbeatIntervalMs_), [this]() {
                return !running_;
            });
        }
    }

    std::string registryHost_;
    int registryPort_;
    int heartbeatIntervalMs_;

    std::atomic<bool> running_;
    std::thread heartbeatThread_;
    std::mutex mutex_;
    std::condition_variable cv_;

    ServiceKey serviceKey_;
    std::string instanceId_;
};