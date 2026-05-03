/**
 * @file test_kafka_producer_consumer.cpp
 * @brief Kafka producer/consumer 端到端 smoke 测试
 *
 * 假设 Kafka 已经在 localhost:9092 跑起来 + im.messages topic 已建。
 * 通过环境变量 KAFKA_BROKER 可改地址。SKIP_KAFKA_TEST=1 可跳过（CI 默认无 Kafka）。
 */
#include "util/KafkaProducer.h"
#include "util/KafkaConsumer.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <thread>

#define TEST(name) static void name()
#define RUN_TEST(name) do { std::cerr << "[run] " #name "\n"; name(); std::cerr << "[ok]  " #name "\n"; } while (0)

static std::string broker() {
    if (const char* e = std::getenv("KAFKA_BROKER")) return e;
    return "localhost:9092";
}

TEST(produce_then_consume_one) {
    KafkaProducer p({broker()}, "test-producer");
    assert(p.start());

    std::atomic<int> delivered{0};
    bool ok = p.produce("im.messages", "test-key-1", R"({"hello":"world","msg_id":"smoke-1"})",
                         [&](bool succ, const std::string& err) {
                             if (succ) delivered++;
                             else std::cerr << "  delivery err: " << err << "\n";
                         });
    assert(ok);

    // 单独 consumer group，避免 offset 跟其它测试串起来
    std::string group = "test-group-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    KafkaConsumer c({broker()}, group, {"im.messages"});
    std::atomic<int> received{0};
    std::string lastValue;

    bool started = c.start([&](const std::string&, int32_t, int64_t,
                                const std::string& key, const std::string& value) -> bool {
        if (key == "test-key-1" && value.find("smoke-1") != std::string::npos) {
            received++;
            lastValue = value;
        }
        return true;
    });
    assert(started);

    // 给 deliver + consume 各最多 5s
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (received.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    assert(delivered.load() >= 1);
    assert(received.load() >= 1);
    assert(lastValue.find("smoke-1") != std::string::npos);

    p.stop();
    c.stop();
}

TEST(produce_burst_check_count) {
    KafkaProducer p({broker()}, "test-producer-burst");
    assert(p.start());

    constexpr int N = 200;
    std::atomic<int> delivered{0};
    for (int i = 0; i < N; ++i) {
        std::string key = "burst-" + std::to_string(i);
        std::string val = R"({"i":)" + std::to_string(i) + R"(,"msg_id":"burst-)" + std::to_string(i) + "\"}";
        p.produce("im.messages", key, val, [&](bool ok, const std::string&) {
            if (ok) delivered.fetch_add(1);
        });
    }

    // wait deliveries
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (delivered.load() < N && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(delivered.load() == N);
    p.stop();
}

int main() {
    if (std::getenv("SKIP_KAFKA_TEST")) {
        std::cerr << "SKIP (KAFKA_TEST disabled)\n";
        return 0;
    }
    std::cerr << "broker=" << broker() << "\n";
    RUN_TEST(produce_then_consume_one);
    RUN_TEST(produce_burst_check_count);
    std::cerr << "ALL OK\n";
    return 0;
}
