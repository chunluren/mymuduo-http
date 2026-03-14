// test_registry.cpp - 服务注册中心测试
#include <iostream>
#include <cassert>
#include "src/registry/ServiceMeta.h"
#include "src/registry/ServiceCatalog.h"
#include "src/registry/HealthChecker.h"

using namespace std;

void testServiceKey() {
    cout << "=== Testing ServiceKey ===" << endl;

    ServiceKey key1("production", "user-service", "v1.0.0");
    assert(key1.namespace_ == "production");
    assert(key1.serviceName == "user-service");
    assert(key1.version == "v1.0.0");
    assert(key1.key() == "production:user-service:v1.0.0");

    // JSON 序列化
    json j = key1.toJson();
    ServiceKey key2 = ServiceKey::fromJson(j);
    assert(key1 == key2);

    cout << "ServiceKey test passed!" << endl;
}

void testInstanceMeta() {
    cout << "=== Testing InstanceMeta ===" << endl;

    InstanceMeta meta("inst-001", "192.168.1.100", 8080);
    assert(meta.instanceId == "inst-001");
    assert(meta.host == "192.168.1.100");
    assert(meta.port == 8080);
    assert(meta.address() == "192.168.1.100:8080");
    assert(meta.status == "UP");
    assert(!meta.isExpired());

    // JSON 序列化
    json j = meta.toJson();
    InstanceMeta meta2 = InstanceMeta::fromJson(j);
    assert(meta2.instanceId == meta.instanceId);
    assert(meta2.host == meta.host);
    assert(meta2.port == meta.port);

    cout << "InstanceMeta test passed!" << endl;
}

void testServiceCatalog() {
    cout << "=== Testing ServiceCatalog ===" << endl;

    ServiceCatalog catalog;

    // 注册服务
    ServiceKey key("default", "test-service", "v1.0.0");
    auto instance1 = make_shared<InstanceMeta>("inst-001", "192.168.1.100", 8080);
    auto instance2 = make_shared<InstanceMeta>("inst-002", "192.168.1.101", 8080);

    catalog.registerInstance(key, instance1);
    catalog.registerInstance(key, instance2);

    // 发现服务
    auto instances = catalog.discover(key);
    assert(instances.size() == 2);

    // 统计
    auto stats = catalog.getStats();
    assert(stats.totalServices == 1);
    assert(stats.totalInstances == 2);
    assert(stats.healthyInstances == 2);

    // 心跳
    catalog.heartbeat(key, "inst-001");

    // 注销
    catalog.deregisterInstance(key, "inst-001");
    instances = catalog.discover(key);
    assert(instances.size() == 1);

    cout << "ServiceCatalog test passed!" << endl;
}

void testHealthChecker() {
    cout << "=== Testing HealthChecker ===" << endl;

    ServiceCatalog catalog;
    HealthChecker checker(&catalog);
    checker.setCheckInterval(1000);  // 1秒检查一次

    // 注册一个短 TTL 实例
    ServiceKey key("default", "test-service", "v1.0.0");
    auto instance = make_shared<InstanceMeta>("inst-001", "192.168.1.100", 8080);
    instance->ttlSeconds = 1;  // 1秒过期
    catalog.registerInstance(key, instance);

    auto stats = catalog.getStats();
    assert(stats.healthyInstances == 1);

    // 启动检查器
    checker.start();

    // 等待过期
    this_thread::sleep_for(chrono::milliseconds(1500));

    // 检查过期
    checker.checkOnce();

    stats = catalog.getStats();
    assert(stats.expiredInstances >= 1 || stats.healthyInstances == 0);

    checker.stop();

    cout << "HealthChecker test passed!" << endl;
}

int main() {
    cout << "Starting Registry Tests..." << endl << endl;

    testServiceKey();
    testInstanceMeta();
    testServiceCatalog();
    testHealthChecker();

    cout << endl << "All Registry tests passed!" << endl;
    return 0;
}