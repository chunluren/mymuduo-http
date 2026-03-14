// ServiceMeta.h - 服务注册中心核心数据结构
// 定义服务标识、实例元数据等基础类型

#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// 服务唯一标识（命名空间 + 服务名 + 版本）
struct ServiceKey {
    std::string namespace_;      // 命名空间（如 "production", "testing"）
    std::string serviceName;     // 服务名称（如 "user-service"）
    std::string version;         // 版本号（如 "v1.0.0"）

    ServiceKey() = default;

    ServiceKey(const std::string& ns, const std::string& name, const std::string& ver)
        : namespace_(ns), serviceName(name), version(ver) {}

    // 生成唯一键字符串
    std::string key() const {
        return namespace_ + ":" + serviceName + ":" + version;
    }

    // 比较运算符
    bool operator==(const ServiceKey& other) const {
        return namespace_ == other.namespace_ &&
               serviceName == other.serviceName &&
               version == other.version;
    }

    bool operator<(const ServiceKey& other) const {
        return key() < other.key();
    }

    // JSON 序列化
    json toJson() const {
        return json{
            {"namespace", namespace_},
            {"serviceName", serviceName},
            {"version", version}
        };
    }

    // JSON 反序列化
    static ServiceKey fromJson(const json& j) {
        return ServiceKey(
            j.value("namespace", "default"),
            j.value("serviceName", ""),
            j.value("version", "v1.0.0")
        );
    }
};

// 服务实例元数据
struct InstanceMeta {
    std::string instanceId;           // 实例唯一ID
    std::string host;                 // 主机地址
    int port;                         // 端口号
    int weight;                       // 权重（用于负载均衡）
    int64_t lastHeartbeatMs;          // 最后心跳时间戳（毫秒）
    int ttlSeconds;                   // 心跳超时时间（秒）
    std::string status;               // 状态：UP, DOWN, STARTING
    std::map<std::string, std::string> metadata;  // 扩展元数据

    InstanceMeta()
        : port(0), weight(1), lastHeartbeatMs(0), ttlSeconds(30), status("UP") {}

    InstanceMeta(const std::string& id, const std::string& h, int p)
        : instanceId(id), host(h), port(p), weight(1),
          lastHeartbeatMs(currentTimeMs()), ttlSeconds(30), status("UP") {}

    // 获取地址字符串
    std::string address() const {
        return host + ":" + std::to_string(port);
    }

    // 更新心跳时间
    void heartbeat() {
        lastHeartbeatMs = currentTimeMs();
        status = "UP";
    }

    // 检查是否过期
    bool isExpired() const {
        int64_t elapsed = currentTimeMs() - lastHeartbeatMs;
        return elapsed > (ttlSeconds * 1000);
    }

    // 剩余存活时间（毫秒）
    int64_t remainingTtlMs() const {
        int64_t elapsed = currentTimeMs() - lastHeartbeatMs;
        int64_t ttlMs = ttlSeconds * 1000;
        return std::max(0L, ttlMs - elapsed);
    }

    // JSON 序列化
    json toJson() const {
        json j;
        j["instanceId"] = instanceId;
        j["host"] = host;
        j["port"] = port;
        j["weight"] = weight;
        j["lastHeartbeatMs"] = lastHeartbeatMs;
        j["ttlSeconds"] = ttlSeconds;
        j["status"] = status;
        if (!metadata.empty()) {
            j["metadata"] = metadata;
        }
        return j;
    }

    // JSON 反序列化
    static InstanceMeta fromJson(const json& j) {
        InstanceMeta meta;
        meta.instanceId = j.value("instanceId", "");
        meta.host = j.value("host", "");
        meta.port = j.value("port", 0);
        meta.weight = j.value("weight", 1);
        meta.lastHeartbeatMs = j.value("lastHeartbeatMs", currentTimeMs());
        meta.ttlSeconds = j.value("ttlSeconds", 30);
        meta.status = j.value("status", "UP");
        if (j.contains("metadata") && j["metadata"].is_object()) {
            meta.metadata = j["metadata"].get<std::map<std::string, std::string>>();
        }
        return meta;
    }

private:
    // 获取当前时间戳（毫秒）
    static int64_t currentTimeMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
};

// 服务实例条目（包含服务标识和实例信息）
struct ServiceInstance {
    ServiceKey serviceKey;
    InstanceMeta instance;

    ServiceInstance() = default;
    ServiceInstance(const ServiceKey& key, const InstanceMeta& inst)
        : serviceKey(key), instance(inst) {}

    json toJson() const {
        json j;
        j["service"] = serviceKey.toJson();
        j["instance"] = instance.toJson();
        return j;
    }

    static ServiceInstance fromJson(const json& j) {
        ServiceInstance si;
        if (j.contains("service")) {
            si.serviceKey = ServiceKey::fromJson(j["service"]);
        }
        if (j.contains("instance")) {
            si.instance = InstanceMeta::fromJson(j["instance"]);
        }
        return si;
    }
};