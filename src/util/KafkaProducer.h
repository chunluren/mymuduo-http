/**
 * @file KafkaProducer.h
 * @brief 异步 Kafka producer（基于 librdkafka C++ binding）
 *
 * 设计要点：
 *   - 异步：produce() 立即返回，poll loop 在独立线程跑 delivery callback
 *   - acks=all + idempotence + retries → "at-least-once + 写入有序"
 *   - 默认 batch + linger 10ms：吞吐优先；调用方需要单条立即发可设 linger=0
 *   - producer 析构时 flush + close，给最后一批留至多 5s 落地
 *
 * 使用：
 * @code
 *   KafkaProducer kp({"localhost:9092"}, "im-producer-1");
 *   kp.start();
 *   kp.produce("im.messages", "conv-key", payload, [](bool ok, std::string err){
 *       if (!ok) LOG_ERROR("kafka deliver fail: %s", err.c_str());
 *   });
 *   // dtor 自动 flush
 * @endcode
 *
 * 线程安全：start/stop 单线程调用；produce 多线程调用安全（librdkafka rd_kafka_t 自身线程安全）。
 */
#pragma once

#include <librdkafka/rdkafkacpp.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class KafkaProducer {
public:
    /// 投递回调：ok=true 表示已被 broker 确认；err 字段在 ok=false 时填错误描述
    using DeliveryCb = std::function<void(bool ok, const std::string& err)>;

    explicit KafkaProducer(const std::vector<std::string>& brokers,
                           std::string clientId = "muduo-im")
        : clientId_(std::move(clientId)) {
        std::string brokerList;
        for (size_t i = 0; i < brokers.size(); ++i) {
            if (i) brokerList += ',';
            brokerList += brokers[i];
        }
        brokerList_ = std::move(brokerList);
    }

    ~KafkaProducer() { stop(); }

    /// 启动 producer + 后台 poll 线程
    bool start() {
        if (running_.exchange(true)) return true;

        std::string err;
        std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));

        // 持久 + 顺序 + 幂等
        conf->set("bootstrap.servers", brokerList_, err);
        conf->set("client.id", clientId_, err);
        conf->set("acks", "all", err);
        conf->set("enable.idempotence", "true", err);
        conf->set("max.in.flight.requests.per.connection", "5", err);
        conf->set("retries", "10", err);
        conf->set("delivery.timeout.ms", "120000", err);

        // 吞吐
        conf->set("linger.ms", "10", err);          // 攒 10ms 一批
        conf->set("batch.size", "32768", err);      // 32KB
        conf->set("compression.type", "snappy", err);

        // 投递回调
        deliveryCb_ = std::make_unique<DeliveryReportCb>(this);
        conf->set("dr_cb", deliveryCb_.get(), err);

        producer_.reset(RdKafka::Producer::create(conf.get(), err));
        if (!producer_) {
            std::cerr << "[kafka] create producer fail: " << err << "\n";
            running_ = false;
            return false;
        }

        pollThread_ = std::thread([this]() { pollLoop(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (producer_) {
            // flush 最多等 5s
            producer_->flush(5000);
        }
        if (pollThread_.joinable()) pollThread_.join();
        producer_.reset();
        deliveryCb_.reset();
    }

    /**
     * @brief 异步投递一条消息
     * @param topic    Kafka topic 名
     * @param key      partition 路由 key（保证同 key 同序）。空串则随机
     * @param value    payload（任意字节流）
     * @param cb       投递结果回调；可选，nullptr 则忽略 ack
     * @return true 入队成功；false 队列满（调用方自行决定丢弃 or 阻塞重试）
     */
    bool produce(const std::string& topic, const std::string& key,
                 const std::string& value, DeliveryCb cb = nullptr) {
        if (!producer_) return false;

        // 把 cb 放堆上，msg_opaque 透传给 deliveryCb，回调里 delete
        auto* op = cb ? new DeliveryCb(std::move(cb)) : nullptr;

        RdKafka::ErrorCode rc = producer_->produce(
            topic, RdKafka::Topic::PARTITION_UA,
            RdKafka::Producer::RK_MSG_COPY,
            const_cast<char*>(value.data()), value.size(),
            key.empty() ? nullptr : key.data(),
            key.size(),
            0,    // timestamp = 自动
            op);  // msg_opaque

        if (rc != RdKafka::ERR_NO_ERROR) {
            // QUEUE_FULL 是最常见，调用方处理
            delete op;
            droppedCount_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // poll 单独线程做，但偶尔在 produce 路径上也帮忙触发 delivery（可选）
        producer_->poll(0);
        return true;
    }

    /// 累计被丢弃（QUEUE_FULL 或其它错误）的本地入队失败次数
    uint64_t droppedCount() const { return droppedCount_.load(std::memory_order_relaxed); }

private:
    void pollLoop() {
        while (running_.load()) {
            if (producer_) producer_->poll(100);
        }
        // 退出前再 poll 一段，确保最后的 dr_cb 都跑完
        if (producer_) producer_->poll(500);
    }

    /// 内部 dr_cb 适配器：把 librdkafka 回调拆派到调用方传入的 lambda
    class DeliveryReportCb : public RdKafka::DeliveryReportCb {
    public:
        explicit DeliveryReportCb(KafkaProducer* parent) : parent_(parent) {}
        void dr_cb(RdKafka::Message& msg) override {
            auto* op = static_cast<DeliveryCb*>(msg.msg_opaque());
            bool ok = (msg.err() == RdKafka::ERR_NO_ERROR);
            if (op) {
                try {
                    (*op)(ok, ok ? std::string() : RdKafka::err2str(msg.err()));
                } catch (const std::exception& e) {
                    std::cerr << "[kafka] dr_cb user callback threw: " << e.what() << "\n";
                }
                delete op;
            }
            if (!ok) {
                parent_->failedDeliveries_.fetch_add(1, std::memory_order_relaxed);
            } else {
                parent_->successDeliveries_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    private:
        KafkaProducer* parent_;
    };

    std::string brokerList_;
    std::string clientId_;
    std::atomic<bool> running_{false};
    std::unique_ptr<RdKafka::Producer> producer_;
    std::unique_ptr<DeliveryReportCb> deliveryCb_;
    std::thread pollThread_;
    std::atomic<uint64_t> droppedCount_{0};
    std::atomic<uint64_t> successDeliveries_{0};
    std::atomic<uint64_t> failedDeliveries_{0};

public:
    uint64_t successDeliveries() const { return successDeliveries_.load(); }
    uint64_t failedDeliveries() const { return failedDeliveries_.load(); }
};
