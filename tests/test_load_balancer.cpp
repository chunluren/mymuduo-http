// test_load_balancer.cpp - 负载均衡器单元测试

#include "balancer/LoadBalancer.h"
#include <iostream>
#include <cassert>
#include <map>

// 测试辅助宏
#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    test_##name(); \
    std::cout << "PASSED" << std::endl; \
} while(0)

// 测试轮询策略
TEST(round_robin_basic) {
    LoadBalancer lb(LoadBalancer::Strategy::RoundRobin);
    lb.addServer("192.168.1.1", 8080);
    lb.addServer("192.168.1.2", 8080);
    lb.addServer("192.168.1.3", 8080);

    // 统计每个服务器被选中的次数
    std::map<std::string, int> counts;
    for (int i = 0; i < 100; i++) {
        auto server = lb.select();
        assert(server != nullptr);
        counts[server->address()]++;
    }

    // 每个服务器应该被选中大约 33 次
    for (auto& [addr, count] : counts) {
        assert(count > 25 && count < 40);  // 允许一定偏差
    }

    assert(lb.strategyName() == "RoundRobin");
}

// 测试轮询策略 - 不健康服务器
TEST(round_robin_unhealthy) {
    LoadBalancer lb(LoadBalancer::Strategy::RoundRobin);
    lb.addServer("192.168.1.1", 8080);
    lb.addServer("192.168.1.2", 8080);
    lb.addServer("192.168.1.3", 8080);

    // 标记一个服务器为不健康
    lb.setServerHealth("192.168.1.2", 8080, false);

    // 统计每个服务器被选中的次数
    std::map<std::string, int> counts;
    for (int i = 0; i < 100; i++) {
        auto server = lb.select();
        assert(server != nullptr);
        counts[server->address()]++;
    }

    // 不健康的服务器不应该被选中
    assert(counts.count("192.168.1.2:8080") == 0);

    // 健康的服务器应该平分请求
    assert(counts["192.168.1.1:8080"] > 40);
    assert(counts["192.168.1.3:8080"] > 40);
}

// 测试加权轮询策略
TEST(weighted_round_robin) {
    LoadBalancer lb(LoadBalancer::Strategy::WeightedRoundRobin);
    lb.addServer("192.168.1.1", 8080, 5);  // 权重 5
    lb.addServer("192.168.1.2", 8080, 3);  // 权重 3
    lb.addServer("192.168.1.3", 8080, 2);  // 权重 2

    // 统计每个服务器被选中的次数
    std::map<std::string, int> counts;
    for (int i = 0; i < 100; i++) {
        auto server = lb.select();
        assert(server != nullptr);
        counts[server->address()]++;
    }

    // 权重高的服务器应该被选中更多次
    assert(counts["192.168.1.1:8080"] > counts["192.168.1.2:8080"]);
    assert(counts["192.168.1.2:8080"] > counts["192.168.1.3:8080"]);

    // 检查大致比例 (5:3:2)
    int total = counts["192.168.1.1:8080"] + counts["192.168.1.2:8080"] + counts["192.168.1.3:8080"];
    assert(total == 100);

    assert(lb.strategyName() == "WeightedRoundRobin");
}

// 测试平滑加权轮询
// 验证请求分布均匀，不会出现连续请求集中到一个服务器
TEST(weighted_round_robin_smooth) {
    LoadBalancer lb(LoadBalancer::Strategy::WeightedRoundRobin);
    lb.addServer("A", 8080, 5);
    lb.addServer("B", 8080, 1);
    lb.addServer("C", 8080, 1);

    // 记录选择序列
    std::map<std::string, int> counts;
    for (int i = 0; i < 70; i++) {  // 10 个周期 (5+1+1=7)
        auto server = lb.select();
        counts[server->host]++;
    }

    // 验证大致比例 5:1:1
    assert(counts["A"] == 50);  // 70 * 5/7 = 50
    assert(counts["B"] == 10);  // 70 * 1/7 = 10
    assert(counts["C"] == 10);  // 70 * 1/7 = 10
}

// 测试最小连接数策略
TEST(least_connections) {
    LoadBalancer lb(LoadBalancer::Strategy::LeastConnections);
    lb.addServer("192.168.1.1", 8080);
    lb.addServer("192.168.1.2", 8080);
    lb.addServer("192.168.1.3", 8080);

    // 初始状态，所有服务器连接数都是 0
    auto server1 = lb.select();
    assert(server1 != nullptr);
    assert(server1->connections == 1);

    // 再次选择，应该选择连接数最小的（其他服务器连接数为 0）
    auto server2 = lb.select();
    assert(server2 != nullptr);
    assert(server2->connections == 1);
    assert(server2 != server1);  // 应该是不同的服务器

    auto server3 = lb.select();
    assert(server3 != nullptr);
    assert(server3->connections == 1);
    assert(server3 != server1 && server3 != server2);

    // 释放连接
    lb.releaseConnection(server1);
    assert(server1->connections == 0);

    // 现在应该选择 server1（连接数最少）
    auto server4 = lb.select();
    assert(server4 == server1);
    assert(server1->connections == 1);

    assert(lb.strategyName() == "LeastConnections");
}

// 测试随机策略
TEST(random_strategy) {
    LoadBalancer lb(LoadBalancer::Strategy::Random);
    lb.addServer("192.168.1.1", 8080);
    lb.addServer("192.168.1.2", 8080);
    lb.addServer("192.168.1.3", 8080);

    // 统计每个服务器被选中的次数
    std::map<std::string, int> counts;
    for (int i = 0; i < 1000; i++) {
        auto server = lb.select();
        assert(server != nullptr);
        counts[server->address()]++;
    }

    // 随机分布，每个服务器应该被选中大约 333 次
    for (auto& [addr, count] : counts) {
        assert(count > 200 && count < 500);  // 允许较大偏差
    }

    assert(lb.strategyName() == "Random");
}

// 测试空服务器列表
TEST(empty_servers) {
    LoadBalancer lb(LoadBalancer::Strategy::RoundRobin);

    auto server = lb.select();
    assert(server == nullptr);
}

// 测试单服务器
TEST(single_server) {
    LoadBalancer lb(LoadBalancer::Strategy::RoundRobin);
    lb.addServer("192.168.1.1", 8080);

    for (int i = 0; i < 10; i++) {
        auto server = lb.select();
        assert(server != nullptr);
        assert(server->host == "192.168.1.1");
        assert(server->port == 8080);
    }
}

// 测试添加和移除服务器
TEST(add_remove_servers) {
    LoadBalancer lb(LoadBalancer::Strategy::RoundRobin);
    lb.addServer("192.168.1.1", 8080);
    lb.addServer("192.168.1.2", 8080);

    assert(lb.servers().size() == 2);

    lb.removeServer("192.168.1.1", 8080);
    assert(lb.servers().size() == 1);
    assert(lb.servers()[0]->host == "192.168.1.2");

    // 只剩一个服务器
    auto server = lb.select();
    assert(server->host == "192.168.1.2");
}

// 测试服务器信息
TEST(server_info) {
    BackendServer server("192.168.1.1", 8080, 5);

    assert(server.host == "192.168.1.1");
    assert(server.port == 8080);
    assert(server.weight == 5);
    assert(server.connections == 0);
    assert(server.healthy == true);
    assert(server.address() == "192.168.1.1:8080");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Load Balancer Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    RUN_TEST(round_robin_basic);
    RUN_TEST(round_robin_unhealthy);
    RUN_TEST(weighted_round_robin);
    RUN_TEST(weighted_round_robin_smooth);
    RUN_TEST(least_connections);
    RUN_TEST(random_strategy);
    RUN_TEST(empty_servers);
    RUN_TEST(single_server);
    RUN_TEST(add_remove_servers);
    RUN_TEST(server_info);

    std::cout << "========================================" << std::endl;
    std::cout << "  All tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}