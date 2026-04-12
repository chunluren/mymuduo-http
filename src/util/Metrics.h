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
     * @brief 增加计数器（Counter 类型指标）
     * @param name 计数器名称（如 "http_requests_total"）
     * @param value 增量（默认 1）
     *
     * Counter 只能单调递增，适用于统计累计事件数量（如请求总数、错误数）。
     * 线程安全: 内部使用 mutex 保护 counters_ 映射表。
     */
    void increment(const std::string& name, int64_t value = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_[name] += value;
    }

    /**
     * @brief 设置 Gauge 值（Gauge 类型指标）
     * @param name Gauge 名称（如 "active_connections"）
     * @param value 当前值
     *
     * Gauge 可增可减，表示某个瞬时状态值（如当前活跃连接数、内存占用量）。
     * 每次调用直接覆盖旧值，而非累加。
     */
    void gauge(const std::string& name, int64_t value) {
        std::lock_guard<std::mutex> lock(mutex_);
        gauges_[name] = value;
    }

    /**
     * @brief 记录观测值（Histogram/Summary 类型指标）
     * @param name 指标名称（如 "request_duration_ms"）
     * @param valueMs 观测值（毫秒）
     *
     * 用于记录耗时分布等需要统计聚合的指标。
     * 内部维护 count（观测次数）和 sum（累计值），
     * 导出时以 Prometheus summary 格式输出 _count 和 _sum 两个子指标。
     * 例: request_duration_ms_count = 100, request_duration_ms_sum = 1234.5
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
     * @brief 导出所有指标为 Prometheus text exposition 格式
     * @return Prometheus exposition format 字符串
     *
     * 输出格式遵循 Prometheus text-based exposition format 规范:
     * - 每个指标先输出 "# TYPE <name> <type>" 元数据行
     * - Counter: 输出 "<name> <value>"
     * - Gauge: 输出 "<name> <value>"
     * - Summary: 输出 "<name>_count <N>" 和 "<name>_sum <total>"
     *
     * 通常挂载在 /metrics 端点供 Prometheus 定期抓取。
     */
    std::string toPrometheus() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream oss;

        /// 导出所有 Counter 类型指标
        for (const auto& [name, value] : counters_) {
            oss << "# TYPE " << name << " counter\n";
            oss << name << " " << value << "\n";
        }

        /// 导出所有 Gauge 类型指标
        for (const auto& [name, value] : gauges_) {
            oss << "# TYPE " << name << " gauge\n";
            oss << name << " " << value << "\n";
        }

        /// 导出所有 Histogram/Summary 类型指标（count + sum 两个子指标）
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
