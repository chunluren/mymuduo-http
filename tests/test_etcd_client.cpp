/**
 * @file test_etcd_client.cpp
 * @brief EtcdClient 烟测：grant lease + put + get + keepalive + poll diff + del
 *
 * 假设 etcd 已经在 127.0.0.1:2379 跑起来。SKIP_ETCD_TEST=1 跳过。
 */
#include "util/EtcdClient.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#define TEST(name) static void name()
#define RUN_TEST(name) do { std::cerr << "[run] " #name "\n"; name(); std::cerr << "[ok]  " #name "\n"; } while (0)

TEST(grant_put_get_del) {
    EtcdClient c("127.0.0.1", 2379);
    int64_t lease = c.grantLease(60);
    assert(lease > 0);

    bool ok = c.put("services/test/A", R"({"addr":"127.0.0.1:9100"})", lease);
    assert(ok);
    ok = c.put("services/test/B", R"({"addr":"127.0.0.1:9101"})", lease);
    assert(ok);

    auto kvs = c.getPrefix("services/test/");
    assert(kvs.size() == 2);
    bool gotA = false, gotB = false;
    for (auto& kv : kvs) {
        if (kv.key == "services/test/A" && kv.value.find("9100") != std::string::npos) gotA = true;
        if (kv.key == "services/test/B" && kv.value.find("9101") != std::string::npos) gotB = true;
    }
    assert(gotA && gotB);

    int ttlLeft = c.keepAlive(lease);
    assert(ttlLeft > 0);

    c.del("services/test/A");
    c.del("services/test/B");
    auto kvs2 = c.getPrefix("services/test/");
    assert(kvs2.empty());
}

TEST(poll_diff_added_removed_updated) {
    EtcdClient c("127.0.0.1", 2379);
    int64_t lease = c.grantLease(60);
    assert(lease > 0);

    int addedCount = 0, removedCount = 0, updatedCount = 0;
    auto cb = [&](const std::vector<EtcdClient::KV>& a,
                  const std::vector<std::string>& r,
                  const std::vector<EtcdClient::KV>& u) {
        addedCount += a.size();
        removedCount += r.size();
        updatedCount += u.size();
    };

    // 初始空：第一次 pollOnce 不触发 cb（snapshot 为空）
    c.pollOnce("services/poll/", cb);
    assert(addedCount == 0 && removedCount == 0 && updatedCount == 0);

    // put X → next poll 应当 added=1
    c.put("services/poll/X", "v1", lease);
    c.pollOnce("services/poll/", cb);
    assert(addedCount == 1);

    // 改 X → updated=1
    c.put("services/poll/X", "v2", lease);
    c.pollOnce("services/poll/", cb);
    assert(updatedCount == 1);

    // del X → removed=1
    c.del("services/poll/X");
    c.pollOnce("services/poll/", cb);
    assert(removedCount == 1);
}

int main() {
    if (std::getenv("SKIP_ETCD_TEST")) {
        std::cerr << "SKIP\n";
        return 0;
    }
    RUN_TEST(grant_put_get_del);
    RUN_TEST(poll_diff_added_removed_updated);
    std::cerr << "ALL OK\n";
    return 0;
}
