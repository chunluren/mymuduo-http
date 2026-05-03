/**
 * @file test_redis_sentinel.cpp
 * @brief RedisPool sentinel 模式 (Phase 5b.1) + master switch 计数器 (5b.6)
 *
 * 跑前提：
 *   bash deploy/redis-sentinel/up.sh   # 在 muduo-im 仓库根
 *
 * 跳过：SKIP_SENTINEL_TEST=1
 */
#include "src/pool/RedisPool.h"
#include "src/util/Metrics.h"
#include <cassert>
#include <cstdlib>
#include <iostream>

#define TEST(name) static void name()
#define RUN_TEST(name) do { std::cerr << "[run] " #name "\n"; name(); std::cerr << "[ok]  " #name "\n"; } while (0)

TEST(sentinel_resolves_master_and_does_io) {
    RedisPoolConfig cfg;
    cfg.sentinels = {"127.0.0.1:26379", "127.0.0.1:26380", "127.0.0.1:26381"};
    cfg.sentinelMaster = "im-master";
    cfg.minSize = 1;
    cfg.maxSize = 3;

    RedisPool pool(cfg);
    auto conn = pool.acquire(2000);
    assert(conn && conn->valid());

    // 写读测试：set / get
    bool ok = conn->set("sentinel-test-k", "v1");
    assert(ok);
    std::string v = conn->get("sentinel-test-k");
    assert(v == "v1");

    pool.release(std::move(conn));
}

TEST(sentinel_unknown_master_falls_back_to_host_port) {
    RedisPoolConfig cfg;
    cfg.sentinels = {"127.0.0.1:26379"};
    cfg.sentinelMaster = "no-such-master";
    // host/port 兜底设到一个本机已经在跑的 redis（master 端口 6379）
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.minSize = 1;
    cfg.maxSize = 1;

    RedisPool pool(cfg);
    // sentinel 解析失败 → 回退 host/port → 还是能连上
    auto conn = pool.acquire(2000);
    assert(conn && conn->valid());
    assert(conn->ping());
    pool.release(std::move(conn));
}

// Phase 5b.6: 多次 createConnection 同一主，counter 不应递增（只有 master 切换时才 +1）
TEST(switch_counter_stable_when_master_unchanged) {
    int64_t before = Metrics::instance().getCounter("redis_sentinel_master_switches_total");
    RedisPoolConfig cfg;
    cfg.sentinels = {"127.0.0.1:26379"};
    cfg.sentinelMaster = "im-master";
    cfg.minSize = 1; cfg.maxSize = 5;
    RedisPool pool(cfg);
    // 强制建多条连接（触发多次 sentinel resolve）
    for (int i = 0; i < 4; ++i) {
        auto c = pool.acquire(2000);
        assert(c && c->valid());
        // 不 release，强制 createConnection 走多次
    }
    int64_t after = Metrics::instance().getCounter("redis_sentinel_master_switches_total");
    // 同一主，counter 不应递增
    assert(after == before);
}

int main() {
    if (std::getenv("SKIP_SENTINEL_TEST")) {
        std::cerr << "SKIP\n";
        return 0;
    }
    RUN_TEST(sentinel_resolves_master_and_does_io);
    RUN_TEST(sentinel_unknown_master_falls_back_to_host_port);
    RUN_TEST(switch_counter_stable_when_master_unchanged);
    std::cerr << "ALL OK\n";
    return 0;
}
