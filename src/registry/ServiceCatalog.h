// ServiceCatalog.h - 服务目录（内存索引）
// 管理已注册的服务实例，提供服务发现和查询功能

#pragma once

#include "ServiceMeta.h"
#include <mutex>
#include <unordered_map>
#include <map>
#include <vector>
#include <memory>
#include <functional>

// 服务实例指针
using InstancePtr = std::shared_ptr<InstanceMeta>;

// 服务目录：管理服务实例的内存索引
class ServiceCatalog {
public:
    // 注册服务实例
    bool registerInstance(const ServiceKey& key, const InstancePtr& instance) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto& instances = catalog_[key];
        // 检查是否已存在相同实例ID
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

    // 注销服务实例
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

    // 更新心跳
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

    // 发现服务实例
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

    // 按命名空间发现所有服务
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

    // 获取所有服务（用于管理接口）
    std::map<ServiceKey, std::vector<InstancePtr>> getAllServices() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return catalog_;
    }

    // 清理过期实例
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

    // 标记过期实例为 DOWN
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

    // 获取统计信息
    struct Stats {
        size_t totalServices;
        size_t totalInstances;
        size_t healthyInstances;
        size_t expiredInstances;
    };

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

    // 清空所有数据
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        catalog_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::map<ServiceKey, std::vector<InstancePtr>> catalog_;
};

// 哈希函数（用于 unordered_map）
namespace std {
    template<>
    struct hash<ServiceKey> {
        size_t operator()(const ServiceKey& k) const {
            return hash<string>()(k.key());
        }
    };
}