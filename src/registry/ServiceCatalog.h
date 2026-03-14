/**
 * @file ServiceCatalog.h
 * @brief 服务目录 (内存索引)
 *
 * 本文件定义了 ServiceCatalog 类，管理已注册的服务实例。
 * 提供服务注册、发现、心跳、过期清理等功能。
 *
 * @example 使用示例
 * @code
 * ServiceCatalog catalog;
 *
 * // 注册服务实例
 * ServiceKey key("production", "user-service", "v1.0.0");
 * auto instance = std::make_shared<InstanceMeta>();
 * instance->host = "192.168.1.1";
 * instance->port = 8080;
 * catalog.registerInstance(key, instance);
 *
 * // 发现服务
 * auto instances = catalog.discover(key);
 * for (const auto& inst : instances) {
 *     std::cout << inst->address() << std::endl;
 * }
 *
 * // 清理过期实例
 * int cleaned = catalog.cleanExpiredInstances();
 * @endcode
 */

#pragma once

#include "ServiceMeta.h"
#include <mutex>
#include <unordered_map>
#include <map>
#include <vector>
#include <memory>
#include <functional>

/// 服务实例指针
using InstancePtr = std::shared_ptr<InstanceMeta>;

/**
 * @class ServiceCatalog
 * @brief 服务目录: 管理服务实例的内存索引
 *
 * 提供服务实例的 CRUD 操作和查询功能。
 * 所有方法都是线程安全的。
 *
 * 内部使用 std::map 存储服务到实例列表的映射
 */
class ServiceCatalog {
public:
    /**
     * @brief 注册服务实例
     * @param key 服务标识
     * @param instance 实例指针
     * @return 是否成功
     *
     * 如果实例 ID 已存在，会更新现有实例
     */
    bool registerInstance(const ServiceKey& key, const InstancePtr& instance) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto& instances = catalog_[key];
        // 检查是否已存在相同实例 ID
        for (auto& inst : instances) {
            if (inst->instanceId == instance->instanceId) {
                // 替换整个 shared_ptr，避免数据竞争
                inst = instance;
                inst->heartbeat();
                return true;
            }
        }
        // 新实例，添加到列表
        instances.push_back(instance);
        return true;
    }

    /**
     * @brief 注销服务实例
     * @param key 服务标识
     * @param instanceId 实例 ID
     * @return 是否成功
     */
    bool deregisterInstance(const ServiceKey& key, const std::string& instanceId) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = catalog_.find(key);
        if (it == catalog_.end()) {
            return false;
        }

        auto& instances = it->second;
        for (auto iter = instances.begin(); iter != instances.end(); ++iter) {
            if ((*iter)->instanceId == instanceId) {
                instances.erase(iter);
                // 如果该服务没有实例了，移除整个服务
                if (instances.empty()) {
                    catalog_.erase(it);
                }
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 更新心跳
     * @param key 服务标识
     * @param instanceId 实例 ID
     * @return 是否成功
     */
    bool heartbeat(const ServiceKey& key, const std::string& instanceId) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = catalog_.find(key);
        if (it == catalog_.end()) {
            return false;
        }

        for (auto& inst : it->second) {
            if (inst->instanceId == instanceId) {
                inst->heartbeat();
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 发现服务实例
     * @param key 服务标识
     * @return 健康的实例列表
     *
     * 只返回 status == "UP" 且未过期的实例
     */
    std::vector<InstancePtr> discover(const ServiceKey& key) const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<InstancePtr> result;
        auto it = catalog_.find(key);
        if (it != catalog_.end()) {
            // 只返回健康的实例
            for (const auto& inst : it->second) {
                if (inst->status == "UP" && !inst->isExpired()) {
                    result.push_back(inst);
                }
            }
        }
        return result;
    }

    /**
     * @brief 按命名空间发现所有服务
     * @param ns 命名空间
     * @return 服务标识列表
     */
    std::vector<ServiceKey> discoverByNamespace(const std::string& ns) const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<ServiceKey> result;
        for (const auto& [key, instances] : catalog_) {
            if (key.namespace_ == ns) {
                result.push_back(key);
            }
        }
        return result;
    }

    /**
     * @brief 获取所有服务
     * @return 服务到实例列表的映射
     */
    std::map<ServiceKey, std::vector<InstancePtr>> getAllServices() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return catalog_;
    }

    /**
     * @brief 清理过期实例
     * @return 清理的实例数量
     */
    int cleanExpiredInstances() {
        std::lock_guard<std::mutex> lock(mutex_);

        int cleaned = 0;
        for (auto it = catalog_.begin(); it != catalog_.end(); ) {
            auto& instances = it->second;
            for (auto iter = instances.begin(); iter != instances.end(); ) {
                if ((*iter)->isExpired()) {
                    (*iter)->status = "DOWN";
                    iter = instances.erase(iter);
                    cleaned++;
                } else {
                    ++iter;
                }
            }
            // 移除空服务
            if (instances.empty()) {
                it = catalog_.erase(it);
            } else {
                ++it;
            }
        }
        return cleaned;
    }

    /**
     * @brief 标记过期实例为 DOWN
     * @return 标记的实例数量
     */
    int markExpiredInstancesDown() {
        std::lock_guard<std::mutex> lock(mutex_);

        int marked = 0;
        for (auto& [key, instances] : catalog_) {
            for (auto& inst : instances) {
                if (inst->status == "UP" && inst->isExpired()) {
                    inst->status = "DOWN";
                    marked++;
                }
            }
        }
        return marked;
    }

    /**
     * @brief 统计信息结构体
     */
    struct Stats {
        size_t totalServices;      ///< 服务总数
        size_t totalInstances;     ///< 实例总数
        size_t healthyInstances;   ///< 健康实例数
        size_t expiredInstances;   ///< 过期实例数
    };

    /**
     * @brief 获取统计信息
     * @return 统计信息结构体
     */
    Stats getStats() const {
        std::lock_guard<std::mutex> lock(mutex_);

        Stats stats{};
        stats.totalServices = catalog_.size();

        for (const auto& [key, instances] : catalog_) {
            stats.totalInstances += instances.size();
            for (const auto& inst : instances) {
                if (inst->status == "UP" && !inst->isExpired()) {
                    stats.healthyInstances++;
                } else {
                    stats.expiredInstances++;
                }
            }
        }
        return stats;
    }

    /// 清空所有数据
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        catalog_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::map<ServiceKey, std::vector<InstancePtr>> catalog_;
};

/// 哈希函数 (用于 unordered_map)
namespace std {
    template<>
    struct hash<ServiceKey> {
        size_t operator()(const ServiceKey& k) const {
            return hash<string>()(k.key());
        }
    };
}