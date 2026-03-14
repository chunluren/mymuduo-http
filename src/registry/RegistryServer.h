// RegistryServer.h - 服务注册中心服务器
// 提供服务注册、发现、心跳、注销等 HTTP API

#pragma once

#include "ServiceMeta.h"
#include "ServiceCatalog.h"
#include "HealthChecker.h"
#include "http/HttpServer.h"
#include "../http/HttpRequest.h"
#include "../http/HttpResponse.h"
#include <memory>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// 服务注册中心服务器
class RegistryServer {
public:
    RegistryServer(EventLoop* loop, const InetAddress& addr, const std::string& name = "RegistryServer")
        : httpServer_(loop, addr, name)
        , catalog_(std::make_unique<ServiceCatalog>())
        , healthChecker_(catalog_.get())
    {
        registerHandlers();
    }

    ~RegistryServer() {
        healthChecker_.stop();
    }

    void setThreadNum(int num) { httpServer_.setThreadNum(num); }

    void start() {
        healthChecker_.start();
        httpServer_.start();
    }

    // 获取服务目录（用于管理）
    ServiceCatalog* catalog() { return catalog_.get(); }

    // 获取统计信息
    ServiceCatalog::Stats getStats() const {
        return catalog_->getStats();
    }

private:
    void registerHandlers() {
        // 注册服务实例
        httpServer_.POST("/api/v1/registry/register", [this](const HttpRequest& req, HttpResponse& resp) {
            handleRegister(req, resp);
        });

        // 注销服务实例
        httpServer_.POST("/api/v1/registry/deregister", [this](const HttpRequest& req, HttpResponse& resp) {
            handleDeregister(req, resp);
        });

        // 心跳
        httpServer_.POST("/api/v1/registry/heartbeat", [this](const HttpRequest& req, HttpResponse& resp) {
            handleHeartbeat(req, resp);
        });

        // 发现服务
        httpServer_.GET("/api/v1/registry/discover", [this](const HttpRequest& req, HttpResponse& resp) {
            handleDiscover(req, resp);
        });

        // 获取所有服务
        httpServer_.GET("/api/v1/registry/services", [this](const HttpRequest& req, HttpResponse& resp) {
            handleGetAllServices(req, resp);
        });

        // 健康检查
        httpServer_.GET("/api/v1/registry/health", [this](const HttpRequest& req, HttpResponse& resp) {
            resp.json(R"({"status":"UP"})");
        });

        // 统计信息
        httpServer_.GET("/api/v1/registry/stats", [this](const HttpRequest& req, HttpResponse& resp) {
            handleGetStats(req, resp);
        });
    }

    // 注册服务实例
    void handleRegister(const HttpRequest& req, HttpResponse& resp) {
        try {
            json body = json::parse(req.body);

            // 解析服务标识
            ServiceKey key = ServiceKey::fromJson(body["service"]);

            // 解析实例信息
            InstanceMeta instance = InstanceMeta::fromJson(body["instance"]);

            // 生成实例 ID（如果没有）
            if (instance.instanceId.empty()) {
                instance.instanceId = generateInstanceId(key, instance);
            }

            // 更新心跳时间
            instance.heartbeat();

            // 注册到目录
            auto instancePtr = std::make_shared<InstanceMeta>(instance);
            catalog_->registerInstance(key, instancePtr);

            // 返回成功
            json result;
            result["success"] = true;
            result["instanceId"] = instance.instanceId;
            resp.json(result.dump());

        } catch (const json::exception& e) {
            resp.json(makeError(-32700, "Parse error: " + std::string(e.what())));
        } catch (const std::exception& e) {
            resp.json(makeError(-32603, "Internal error: " + std::string(e.what())));
        }
    }

    // 注销服务实例
    void handleDeregister(const HttpRequest& req, HttpResponse& resp) {
        try {
            json body = json::parse(req.body);

            ServiceKey key = ServiceKey::fromJson(body["service"]);
            std::string instanceId = body.value("instanceId", "");

            if (instanceId.empty()) {
                resp.json(makeError(-32602, "Missing instanceId"));
                return;
            }

            bool success = catalog_->deregisterInstance(key, instanceId);

            json result;
            result["success"] = success;
            resp.json(result.dump());

        } catch (const json::exception& e) {
            resp.json(makeError(-32700, "Parse error"));
        }
    }

    // 心跳
    void handleHeartbeat(const HttpRequest& req, HttpResponse& resp) {
        try {
            json body = json::parse(req.body);

            ServiceKey key = ServiceKey::fromJson(body["service"]);
            std::string instanceId = body.value("instanceId", "");

            if (instanceId.empty()) {
                resp.json(makeError(-32602, "Missing instanceId"));
                return;
            }

            bool success = catalog_->heartbeat(key, instanceId);

            json result;
            result["success"] = success;
            resp.json(result.dump());

        } catch (const json::exception& e) {
            resp.json(makeError(-32700, "Parse error"));
        }
    }

    // 发现服务
    void handleDiscover(const HttpRequest& req, HttpResponse& resp) {
        try {
            // 从查询参数获取服务信息
            std::string ns = getQueryParam(req.path, "namespace", "default");
            std::string serviceName = getQueryParam(req.path, "serviceName", "");
            std::string version = getQueryParam(req.path, "version", "v1.0.0");

            if (serviceName.empty()) {
                resp.json(makeError(-32602, "Missing serviceName"));
                return;
            }

            ServiceKey key(ns, serviceName, version);
            auto instances = catalog_->discover(key);

            json result;
            result["success"] = true;
            result["service"] = key.toJson();

            json instancesJson = json::array();
            for (const auto& inst : instances) {
                instancesJson.push_back(inst->toJson());
            }
            result["instances"] = instancesJson;

            resp.json(result.dump());

        } catch (const std::exception& e) {
            resp.json(makeError(-32603, "Internal error"));
        }
    }

    // 获取所有服务
    void handleGetAllServices(const HttpRequest& req, HttpResponse& resp) {
        auto allServices = catalog_->getAllServices();

        json result;
        result["success"] = true;

        json servicesJson = json::array();
        for (const auto& [key, instances] : allServices) {
            json serviceInfo;
            serviceInfo["service"] = key.toJson();

            json instancesJson = json::array();
            for (const auto& inst : instances) {
                instancesJson.push_back(inst->toJson());
            }
            serviceInfo["instances"] = instancesJson;
            servicesJson.push_back(serviceInfo);
        }
        result["services"] = servicesJson;

        resp.json(result.dump());
    }

    // 获取统计信息
    void handleGetStats(const HttpRequest& req, HttpResponse& resp) {
        auto stats = catalog_->getStats();

        json result;
        result["success"] = true;
        result["stats"] = {
            {"totalServices", stats.totalServices},
            {"totalInstances", stats.totalInstances},
            {"healthyInstances", stats.healthyInstances},
            {"expiredInstances", stats.expiredInstances}
        };

        resp.json(result.dump());
    }

    // 生成实例 ID
    std::string generateInstanceId(const ServiceKey& key, const InstanceMeta& instance) {
        return key.serviceName + "-" + instance.host + "-" + std::to_string(instance.port) +
               "-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()
               ).count());
    }

    // 解析查询参数
    std::string getQueryParam(const std::string& path, const std::string& param, const std::string& defaultValue) {
        size_t queryPos = path.find('?');
        if (queryPos == std::string::npos) {
            return defaultValue;
        }

        std::string query = path.substr(queryPos + 1);
        std::string searchKey = param + "=";

        size_t startPos = query.find(searchKey);
        if (startPos == std::string::npos) {
            return defaultValue;
        }

        startPos += searchKey.length();
        size_t endPos = query.find('&', startPos);

        if (endPos == std::string::npos) {
            return query.substr(startPos);
        }
        return query.substr(startPos, endPos - startPos);
    }

    // 生成错误响应
    std::string makeError(int code, const std::string& message) {
        json error;
        error["success"] = false;
        error["error"]["code"] = code;
        error["error"]["message"] = message;
        return error.dump();
    }

private:
    HttpServer httpServer_;
    std::unique_ptr<ServiceCatalog> catalog_;
    HealthChecker healthChecker_;
};