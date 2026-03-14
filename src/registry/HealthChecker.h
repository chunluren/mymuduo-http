// HealthChecker.h - 健康检查器
// 定期检查服务实例的 TTL 过期状态

#pragma once

#include "ServiceCatalog.h"
#include <thread>
#include <condition_variable>
#include <memory>
#include <functional>
#include <atomic>

// 健康检查回调
using HealthCheckCallback = std::function<void(const std::vector<std::string>& expiredInstances)>;

// 健康检查器
class HealthChecker {
public:
    HealthChecker(ServiceCatalog* catalog)
        : catalog_(catalog)
        , checkIntervalMs_(5000)  // 默认5秒检查一次
        , running_(false)
    {}

    ~HealthChecker() {
        stop();
    }

    // 设置检查间隔
    void setCheckInterval(int ms) {
        checkIntervalMs_ = ms;
    }

    // 设置过期回调
    void setExpiredCallback(HealthCheckCallback cb) {
        expiredCallback_ = std::move(cb);
    }

    // 启动健康检查
    void start() {
        if (running_.exchange(true)) {
            return;  // 已经在运行
        }

        thread_ = std::thread([this]() {
            checkLoop();
        });
    }

    // 停止健康检查
    void stop() {
        if (!running_.exchange(false)) {
            return;  // 已经停止
        }

        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // 手动执行一次检查
    int checkOnce() {
        if (!catalog_) return 0;

        // 标记过期实例为 DOWN
        int marked = catalog_->markExpiredInstancesDown();

        // 触发回调
        if (marked > 0 && expiredCallback_) {
            // 获取所有过期实例的 ID
            auto allServices = catalog_->getAllServices();
            std::vector<std::string> expired;
            for (const auto& [key, instances] : allServices) {
                for (const auto& inst : instances) {
                    if (inst->status == "DOWN") {
                        expired.push_back(inst->instanceId);
                    }
                }
            }
            expiredCallback_(expired);
        }

        return marked;
    }

    // 清理过期实例
    int cleanExpired() {
        if (!catalog_) return 0;
        return catalog_->cleanExpiredInstances();
    }

    bool isRunning() const {
        return running_.load();
    }

private:
    void checkLoop() {
        while (running_) {
            // 执行检查
            checkOnce();

            // 等待下次检查或停止信号
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(checkIntervalMs_), [this]() {
                return !running_;
            });
        }
    }

    ServiceCatalog* catalog_;
    int checkIntervalMs_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    HealthCheckCallback expiredCallback_;
};