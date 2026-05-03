/**
 * @file KafkaConsumer.h
 * @brief 消费组 Kafka 消费者（基于 librdkafka C++ binding）
 *
 * 设计要点：
 *   - 消费组：多实例并行消费同一 topic 的不同 partition
 *   - 手动提交 offset：处理完业务后再 commit，保证 at-least-once
 *   - poll 循环单线程：业务回调里阻塞太久会拖慢整组消费 → 调用方按需把工作丢 worker pool
 *   - 失败重试：业务回调返回 false → 重试一次；再败 → 抛 dead-letter（这部分由调用方实现）
 *
 * 使用：
 * @code
 *   KafkaConsumer kc({"localhost:9092"}, "im-persister", {"im.messages"});
 *   kc.start([](const std::string& topic, int part, int64_t offset,
 *               const std::string& key, const std::string& value) -> bool {
 *       return persistMessage(value);   // true → commit; false → 不 commit, 下次重投
 *   });
 *   // dtor 自动 stop
 * @endcode
 *
 * 线程安全：start 单次调用；stop 可任意线程；业务回调在内部 poll 线程跑。
 */
#pragma once

#include <librdkafka/rdkafkacpp.h>
#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class KafkaConsumer {
public:
    /// 业务回调；返回 true → 提交 offset（at-least-once）；返回 false → 不提交（重投）
    using MessageCb = std::function<bool(const std::string& topic, int32_t partition,
                                          int64_t offset, const std::string& key,
                                          const std::string& value)>;

    KafkaConsumer(const std::vector<std::string>& brokers,
                  std::string groupId,
                  std::vector<std::string> topics,
                  int pollTimeoutMs = 50)   // Phase 1.7：默认 50ms（旧 500ms）
        : groupId_(std::move(groupId)), topics_(std::move(topics)),
          pollTimeoutMs_(pollTimeoutMs) {
        std::string list;
        for (size_t i = 0; i < brokers.size(); ++i) {
            if (i) list += ',';
            list += brokers[i];
        }
        brokerList_ = std::move(list);
    }

    ~KafkaConsumer() { stop(); }

    bool start(MessageCb cb) {
        if (running_.exchange(true)) return true;
        cb_ = std::move(cb);

        std::string err;
        std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
        conf->set("bootstrap.servers", brokerList_, err);
        conf->set("group.id", groupId_, err);
        // 手动提交，业务处理成功后才 commit
        conf->set("enable.auto.commit", "false", err);
        // 第一次启动时从最早处开始读（避免漏消息）；如果想只读新的可改 latest
        conf->set("auto.offset.reset", "earliest", err);
        // session timeout 30s，给业务回调留处理空间
        conf->set("session.timeout.ms", "30000", err);
        conf->set("max.poll.interval.ms", "300000", err);

        consumer_.reset(RdKafka::KafkaConsumer::create(conf.get(), err));
        if (!consumer_) {
            std::cerr << "[kafka] create consumer fail: " << err << "\n";
            running_ = false;
            return false;
        }
        auto rc = consumer_->subscribe(topics_);
        if (rc != RdKafka::ERR_NO_ERROR) {
            std::cerr << "[kafka] subscribe fail: " << RdKafka::err2str(rc) << "\n";
            consumer_.reset();
            running_ = false;
            return false;
        }
        thread_ = std::thread([this]() { pollLoop(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        if (consumer_) {
            consumer_->close();
            consumer_.reset();
        }
    }

    /// 累计成功处理的消息数（监控用）
    uint64_t consumed() const { return consumed_.load(std::memory_order_relaxed); }
    uint64_t errors()   const { return errors_.load(std::memory_order_relaxed); }
    uint64_t retries()  const { return retries_.load(std::memory_order_relaxed); }

private:
    void pollLoop() {
        while (running_.load()) {
            std::unique_ptr<RdKafka::Message> msg(consumer_->consume(pollTimeoutMs_));
            switch (msg->err()) {
                case RdKafka::ERR__TIMED_OUT:
                    continue;
                case RdKafka::ERR_NO_ERROR: {
                    bool ok = false;
                    std::string key = msg->key() ? *msg->key() : std::string();
                    std::string value(static_cast<const char*>(msg->payload()), msg->len());
                    try {
                        ok = cb_(msg->topic_name(), msg->partition(),
                                  msg->offset(), key, value);
                    } catch (const std::exception& e) {
                        std::cerr << "[kafka] consumer cb threw: " << e.what() << "\n";
                        ok = false;
                    }
                    if (ok) {
                        consumer_->commitAsync(msg.get());
                        consumed_.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        // 不提交 → 下次 poll 还会拿到这条；调用方实现死信判断
                        retries_.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }
                case RdKafka::ERR__PARTITION_EOF:
                    // 该分区暂时没新消息
                    continue;
                default:
                    std::cerr << "[kafka] consume error: " << msg->errstr() << "\n";
                    errors_.fetch_add(1, std::memory_order_relaxed);
                    break;
            }
        }
    }

    std::string brokerList_;
    std::string groupId_;
    std::vector<std::string> topics_;
    MessageCb cb_;
    std::atomic<bool> running_{false};
    std::unique_ptr<RdKafka::KafkaConsumer> consumer_;
    std::thread thread_;
    std::atomic<uint64_t> consumed_{0};
    std::atomic<uint64_t> errors_{0};
    std::atomic<uint64_t> retries_{0};
    int pollTimeoutMs_;
};
