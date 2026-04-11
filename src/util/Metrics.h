#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <sstream>
#include <vector>

/**
 * @class Metrics
 * @brief 线程安全的指标收集系统，支持 Prometheus 格式导出
 *
 * 支持三种指标类型:
 * - Counter: 单调递增计数器（如 http_requests_total）
 * - Gauge: 可增可减的瞬时值（如 active_connections）
 * - Histogram/Summary: 观测值的统计（如 request_duration_ms）
 *
 * @example
 * @code
 * auto& m = Metrics::instance();
 * m.increment("http_requests_total");
 * m.gauge("active_connections", 42);
 * m.observe("request_duration_ms", 12.5);
 * std::string output = m.toPrometheus();
 * @endcode
 */
class Metrics {
public:
    /**
     * @brief 获取全局单例
     * @return Metrics 单例引用
     */
    static Metrics& instance() {
        static Metrics m;
        return m;
    }

    /**
     * @brief 增加计数器
     * @param name 计数器名称（如 "http_requests_total"）
     * @param value 增量（默认 1）
     */
    void increment(const std::string& name, int64_t value = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_[name] += value;
    }

    /**
     * @brief 设置 Gauge 值
     * @param name Gauge 名称（如 "active_connections"）
     * @param value 当前值
     */
    void gauge(const std::string& name, int64_t value) {
        std::lock_guard<std::mutex> lock(mutex_);
        gauges_[name] = value;
    }

    /**
     * @brief 记录观测值（毫秒）
     * @param name 指标名称（如 "request_duration_ms"）
     * @param valueMs 观测值（毫秒）
     */
    void observe(const std::string& name, double valueMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& h = histograms_[name];
        h.count++;
        h.sum += valueMs;
    }

    /**
     * @brief 获取计数器值
     * @param name 计数器名称
     * @return 当前计数值，不存在则返回 0
     */
    int64_t getCounter(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = counters_.find(name);
        return (it != counters_.end()) ? it->second : 0;
    }

    /**
     * @brief 获取 Gauge 值
     * @param name Gauge 名称
     * @return 当前值，不存在则返回 0
     */
    int64_t getGauge(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = gauges_.find(name);
        return (it != gauges_.end()) ? it->second : 0;
    }

    /**
     * @brief 导出所有指标为 Prometheus text 格式
     * @return Prometheus exposition format 字符串
     */
    std::string toPrometheus() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream oss;

        // Counters
        for (const auto& [name, value] : counters_) {
            oss << "# TYPE " << name << " counter\n";
            oss << name << " " << value << "\n";
        }

        // Gauges
        for (const auto& [name, value] : gauges_) {
            oss << "# TYPE " << name << " gauge\n";
            oss << name << " " << value << "\n";
        }

        // Histograms (as summary: count + sum)
        for (const auto& [name, data] : histograms_) {
            oss << "# TYPE " << name << " summary\n";
            oss << name << "_count " << data.count << "\n";
            oss << name << "_sum " << data.sum << "\n";
        }

        return oss.str();
    }

    /**
     * @brief 重置所有指标
     */
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_.clear();
        gauges_.clear();
        histograms_.clear();
    }

private:
    Metrics() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, int64_t> counters_;
    std::unordered_map<std::string, int64_t> gauges_;

    /// 直方图/摘要数据
    struct HistogramData {
        int64_t count = 0;
        double sum = 0.0;
    };
    std::unordered_map<std::string, HistogramData> histograms_;
};
