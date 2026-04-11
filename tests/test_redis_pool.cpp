// test_redis_pool.cpp - RedisPool 编译 + 单元测试
#include <iostream>
#include <cassert>
#include "src/pool/RedisPool.h"

using namespace std;

void testRedisConnectionWrapper() {
    cout << "=== Testing RedisConnection Wrapper ===" << endl;
    RedisConnection conn(nullptr);
    assert(!conn.valid());
    assert(!conn.ping());
    cout << "RedisConnection wrapper test passed!" << endl;
}

void testRedisPoolConfig() {
    cout << "=== Testing RedisPool Config ===" << endl;
    RedisPoolConfig config;
    config.host = "127.0.0.1";
    config.port = 6379;
    config.password = "";
    config.db = 0;
    config.minSize = 3;
    config.maxSize = 15;
    assert(config.port == 6379);
    assert(config.db == 0);
    cout << "RedisPool config test passed!" << endl;
}

void testRedisPoolCreation() {
    cout << "=== Testing RedisPool Creation (no Redis required) ===" << endl;
    RedisPoolConfig config;
    config.host = "127.0.0.1";
    config.port = 6379;
    config.minSize = 0;
    config.maxSize = 5;
    RedisPool pool(config);
    assert(pool.available() == 0);
    assert(!pool.isClosed());
    cout << "RedisPool creation test passed!" << endl;
}

int main() {
    cout << "Starting RedisPool Tests..." << endl << endl;
    testRedisConnectionWrapper();
    testRedisPoolConfig();
    testRedisPoolCreation();
    cout << endl << "All RedisPool tests passed!" << endl;
    return 0;
}
