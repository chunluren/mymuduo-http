/**
 * @file ServiceMeta.h
 * @brief 服务注册中心核心数据结构
 *
 * 本文件定义了服务注册中心的基础数据结构:
 * - ServiceKey: 服务唯一标识
 * - InstanceMeta: 服务实例元数据
 * - ServiceInstance: 服务实例完整信息
 *
 * @example 使用示例
 * @code
 * // 创建服务标识
 * ServiceKey key("production", "user-service", "v1.0.0");
 *
 * // 创建实例元数据
 * InstanceMeta instance;
 * instance.instanceId = "user-service-192.168.1.1-8080-1234567890";
 * instance.host = "192.168.1.1";
 * instance.port = 8080;
 * instance.weight = 1;
 * instance.ttlSeconds = 30;
 *
 * // 序列化为 JSON
 * json j = instance.toJson();
 *
 * // 反序列化
 * InstanceMeta instance2 = InstanceMeta::fromJson(j);
 * @endcode
 */

#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * @struct ServiceKey
 * @brief 服务唯一标识
 *
 * 一个服务由三个维度确定:
 * - namespace_: 命名空间，用于隔离不同环境
 * - serviceName: 服务名称
 * - version: 版本号
 *
 * @example
 * @code
 * ServiceKey key("production", "user-service", "v1.0.0");
 * std::string keyStr = key.key();  // "production:user-service:v1.0.0"
 * @endcode
 */
struct ServiceKey {
    std::string namespace_;   ///< 命名空间 (如 "production", "testing")
    std::string serviceName;  ///< 服务名称 (如 "user-service")
    std::string version;      ///< 版本号 (如 "v1.0.0")

    ServiceKey() = default;

    /**
     * @brief 构造服务标识
     * @param ns 命名空间
     * @param name 服务名称
     * @param ver 版本号
     */
    ServiceKey(const std::string& ns, const std::string& name, const std::string& ver)
        : namespace_(ns), serviceName(name), version(ver) {}

    /**
     * @brief 生成唯一键字符串
     * @return "namespace:serviceName:version" 格式的字符串
     */
    std::string key() const {
        return namespace_ + ":" + serviceName + ":" + version;
    }

    /// 相等比较
    bool operator==(const ServiceKey& other) const {
        return namespace_ == other.namespace_ &&
               serviceName == other.serviceName &&
               version == other.version;
    }

    /// 小于比较 (用于 std::map)
    bool operator<(const ServiceKey& other) const {
        return key() < other.key();
    }

    /// JSON 序列化
    json toJson() const {
        return json{
            {"namespace", namespace_},
            {"serviceName", serviceName},
            {"version", version}
        };
    }

    /// JSON 反序列化
    static ServiceKey fromJson(const json& j) {
        return ServiceKey(
            j.value("namespace", "default"),
            j.value("serviceName", ""),
            j.value("version", "v1.0.0")
        );
    }
};

/**
 * @struct InstanceMeta
 * @brief 服务实例元数据
 *
 * 存储单个服务实例的所有信息:
 * - 实例 ID、主机地址、端口
 * - 权重 (用于负载均衡)
 * - 心跳时间和 TTL
 * - 状态和扩展元数据
 */
struct InstanceMeta {
    std::string instanceId;           ///< 实例唯一 ID
    std::string host;                 ///< 主机地址
    int port;                         ///< 端口号
    int weight;                       ///< 权重 (用于负载均衡)
    int64_t lastHeartbeatMs;          ///< 最后心跳时间戳 (毫秒)
    int ttlSeconds;                   ///< 心跳超时时间 (秒)
    std::string status;               ///< 状态: UP, DOWN, STARTING
    std::map<std::string, std::string> metadata;  ///< 扩展元数据

    InstanceMeta()
        : port(0), weight(1), lastHeartbeatMs(0), ttlSeconds(30), status("UP") {}

    InstanceMeta(const std::string& id, const std::string& h, int p)
        : instanceId(id), host(h), port(p), weight(1),
          lastHeartbeatMs(currentTimeMs()), ttlSeconds(30), status("UP") {}

    /**
     * @brief 获取地址字符串
     * @return "host:port" 格式的地址
     */
    std::string address() const {
        return host + ":" + std::to_string(port);
    }

    /**
     * @brief 更新心跳时间
     *
     * 设置 lastHeartbeatMs 为当前时间，status 为 "UP"
     */
    void heartbeat() {
        lastHeartbeatMs = currentTimeMs();
        status = "UP";
    }

    /**
     * @brief 检查是否过期
     * @return true 如果超过 TTL 未收到心跳
     */
    bool isExpired() const {
        int64_t elapsed = currentTimeMs() - lastHeartbeatMs;
        return elapsed > (ttlSeconds * 1000);
    }

    /**
     * @brief 获取剩余存活时间
     * @return 剩余时间 (毫秒)
     */
    int64_t remainingTtlMs() const {
        int64_t elapsed = currentTimeMs() - lastHeartbeatMs;
        int64_t ttlMs = ttlSeconds * 1000;
        return std::max(0L, ttlMs - elapsed);
    }

    /// JSON 序列化
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

    /// JSON 反序列化
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
    /// 获取当前时间戳 (毫秒)
    static int64_t currentTimeMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
};

/**
 * @struct ServiceInstance
 * @brief 服务实例条目
 *
 * 包含服务标识和实例信息的完整条目
 */
struct ServiceInstance {
    ServiceKey serviceKey;   ///< 服务标识
    InstanceMeta instance;   ///< 实例信息

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