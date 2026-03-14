// LoadBalancer.h - 负载均衡策略
// 实现多种负载均衡算法：轮询、加权轮询、最小连接数

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <random>
#include <map>

// 后端服务器信息
struct BackendServer {
    std::string host;
    int port;
    int weight;           // 权重
    int currentWeight;    // 当前权重（用于平滑加权轮询）
    int connections;      // 当前连接数
    bool healthy;         // 健康状态

    BackendServer(const std::string& h, int p, int w = 1)
        : host(h), port(p), weight(w), currentWeight(0), connections(0), healthy(true) {}

    std::string address() const {
        return host + ":" + std::to_string(port);
    }
};

using BackendServerPtr = std::shared_ptr<BackendServer>;

// 负载均衡策略接口
class ILoadBalanceStrategy {
public:
    virtual ~ILoadBalanceStrategy() = default;

    // 选择一个后端服务器
    virtual BackendServerPtr select(const std::vector<BackendServerPtr>& servers) = 0;

    // 策略名称
    virtual std::string name() const = 0;
};

// 轮询策略
class RoundRobinStrategy : public ILoadBalanceStrategy {
public:
    RoundRobinStrategy() : index_(0) {}

    BackendServerPtr select(const std::vector<BackendServerPtr>& servers) override {
        if (servers.empty()) return nullptr;

        // 过滤健康的服务器
        std::vector<BackendServerPtr> healthy;
        for (const auto& s : servers) {
            if (s->healthy) {
                healthy.push_back(s);
            }
        }

        if (healthy.empty()) return nullptr;

        // 轮询选择
        size_t idx = index_.fetch_add(1) % healthy.size();
        return healthy[idx];
    }

    std::string name() const override {
        return "RoundRobin";
    }

private:
    std::atomic<size_t> index_;
};

// 平滑加权轮询策略（Smooth Weighted Round-Robin）
// Nginx 使用的算法，避免请求分布不均匀
class WeightedRoundRobinStrategy : public ILoadBalanceStrategy {
public:
    WeightedRoundRobinStrategy() {}

    BackendServerPtr select(const std::vector<BackendServerPtr>& servers) override {
        if (servers.empty()) return nullptr;

        std::lock_guard<std::mutex> lock(mutex_);

        BackendServerPtr selected = nullptr;
        int totalWeight = 0;

        for (auto& server : servers) {
            if (!server->healthy) continue;

            // 每个服务器的 currentWeight 加上其 weight
            server->currentWeight += server->weight;
            totalWeight += server->weight;

            // 选择 currentWeight 最大的
            if (!selected || server->currentWeight > selected->currentWeight) {
                selected = server;
            }
        }

        if (!selected) return nullptr;

        // 被选中的服务器 currentWeight 减去总权重
        selected->currentWeight -= totalWeight;

        return selected;
    }

    std::string name() const override {
        return "WeightedRoundRobin";
    }

private:
    std::mutex mutex_;
};

// 最小连接数策略
class LeastConnectionsStrategy : public ILoadBalanceStrategy {
public:
    LeastConnectionsStrategy() {}

    BackendServerPtr select(const std::vector<BackendServerPtr>& servers) override {
        if (servers.empty()) return nullptr;

        BackendServerPtr selected = nullptr;
        int minConns = INT32_MAX;

        for (const auto& server : servers) {
            if (!server->healthy) continue;

            // 选择连接数最少的
            int conns = server->connections;
            if (conns < minConns) {
                minConns = conns;
                selected = server;
            }
        }

        // 增加连接计数
        if (selected) {
            selected->connections++;
        }

        return selected;
    }

    // 释放连接（减少连接计数）
    void release(BackendServerPtr server) {
        if (server && server->connections > 0) {
            server->connections--;
        }
    }

    std::string name() const override {
        return "LeastConnections";
    }
};

// 随机策略
class RandomStrategy : public ILoadBalanceStrategy {
public:
    RandomStrategy() : gen_(rd_()) {}

    BackendServerPtr select(const std::vector<BackendServerPtr>& servers) override {
        if (servers.empty()) return nullptr;

        // 过滤健康的服务器
        std::vector<BackendServerPtr> healthy;
        for (const auto& s : servers) {
            if (s->healthy) {
                healthy.push_back(s);
            }
        }

        if (healthy.empty()) return nullptr;

        std::uniform_int_distribution<size_t> dist(0, healthy.size() - 1);
        return healthy[dist(gen_)];
    }

    std::string name() const override {
        return "Random";
    }

private:
    std::random_device rd_;
    std::mt19937 gen_;
};

// 一致性哈希策略（基于客户端IP）
class ConsistentHashStrategy : public ILoadBalanceStrategy {
public:
    ConsistentHashStrategy(int virtualNodes = 150)
        : virtualNodes_(virtualNodes) {}

    void init(const std::vector<BackendServerPtr>& servers) {
        ring_.clear();
        servers_.clear();

        for (const auto& server : servers) {
            if (!server->healthy) continue;

            servers_.push_back(server);

            // 为每个服务器创建虚拟节点
            for (int i = 0; i < virtualNodes_; i++) {
                std::string nodeKey = server->address() + "#" + std::to_string(i);
                uint32_t hash = hashString(nodeKey);
                ring_[hash] = server;
            }
        }
    }

    BackendServerPtr select(const std::vector<BackendServerPtr>& servers) override {
        return selectWithKey(0);  // 默认使用 0 作为 key
    }

    // 根据客户端 IP 或其他 key 选择服务器
    BackendServerPtr selectWithKey(uint32_t key) {
        if (ring_.empty()) return nullptr;

        auto it = ring_.lower_bound(key);
        if (it == ring_.end()) {
            it = ring_.begin();
        }
        return it->second;
    }

    std::string name() const override {
        return "ConsistentHash";
    }

private:
    // 简单的字符串哈希函数（实际生产中可使用 MurmurHash）
    uint32_t hashString(const std::string& str) {
        uint32_t hash = 5381;
        for (char c : str) {
            hash = ((hash << 5) + hash) + static_cast<unsigned char>(c);
        }
        return hash;
    }

    int virtualNodes_;
    std::map<uint32_t, BackendServerPtr> ring_;  // 哈希环
    std::vector<BackendServerPtr> servers_;
};

// 负载均衡器（策略工厂）
class LoadBalancer {
public:
    enum class Strategy {
        RoundRobin,
        WeightedRoundRobin,
        LeastConnections,
        Random,
        ConsistentHash
    };

    LoadBalancer(Strategy strategy = Strategy::RoundRobin)
        : strategyType_(strategy) {
        createStrategy();
    }

    // 添加后端服务器
    void addServer(const std::string& host, int port, int weight = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        servers_.push_back(std::make_shared<BackendServer>(host, port, weight));
    }

    // 移除后端服务器
    void removeServer(const std::string& host, int port) {
        std::lock_guard<std::mutex> lock(mutex_);
        servers_.erase(
            std::remove_if(servers_.begin(), servers_.end(),
                [&](const BackendServerPtr& s) {
                    return s->host == host && s->port == port;
                }),
            servers_.end()
        );
    }

    // 选择服务器
    BackendServerPtr select() {
        std::lock_guard<std::mutex> lock(mutex_);
        return strategyImpl_->select(servers_);
    }

    // 释放连接（用于最小连接数策略）
    void releaseConnection(BackendServerPtr server) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto lc = dynamic_cast<LeastConnectionsStrategy*>(strategyImpl_.get())) {
            lc->release(server);
        }
    }

    // 设置服务器健康状态
    void setServerHealth(const std::string& host, int port, bool healthy) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& s : servers_) {
            if (s->host == host && s->port == port) {
                s->healthy = healthy;
                break;
            }
        }
    }

    // 获取所有服务器（返回拷贝，线程安全）
    std::vector<BackendServerPtr> servers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return servers_;
    }

    // 获取策略名称
    std::string strategyName() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return strategyImpl_->name();
    }

private:
    void createStrategy() {
        switch (strategyType_) {
            case Strategy::RoundRobin:
                strategyImpl_ = std::make_unique<RoundRobinStrategy>();
                break;
            case Strategy::WeightedRoundRobin:
                strategyImpl_ = std::make_unique<WeightedRoundRobinStrategy>();
                break;
            case Strategy::LeastConnections:
                strategyImpl_ = std::make_unique<LeastConnectionsStrategy>();
                break;
            case Strategy::Random:
                strategyImpl_ = std::make_unique<RandomStrategy>();
                break;
            case Strategy::ConsistentHash:
                strategyImpl_ = std::make_unique<ConsistentHashStrategy>();
                break;
        }
    }

    Strategy strategyType_;
    std::unique_ptr<ILoadBalanceStrategy> strategyImpl_;
    std::vector<BackendServerPtr> servers_;
    mutable std::mutex mutex_;  // 保护 servers_
};