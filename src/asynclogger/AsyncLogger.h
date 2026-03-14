/**
 * @file AsyncLogger.h
 * @brief 异步日志系统
 *
 * 本文件定义了 AsyncLogger 类，实现了高性能的异步日志系统。
 * 使用双缓冲技术，将日志写入操作从 I/O 线程移到专门的日志线程。
 *
 * 设计特点:
 * - 双缓冲技术: 使用两个缓冲区交替写入，减少锁竞争
 * - 非阻塞写入: 日志接口立即返回，不阻塞业务线程
 * - 批量写入: 累积多条日志后批量写入文件
 * - 线程安全: 支持多线程并发写入日志
 *
 * @example 使用示例
 * @code
 * // 启动日志系统
 * AsyncLogger::instance().setLogFile("/var/log/app.log");
 * AsyncLogger::instance().setLogLevel(LogLevel::INFO);
 * AsyncLogger::instance().start();
 *
 * // 记录日志
 * LOG_INFO("Server started on port %d", 8080);
 * LOG_ERROR("Failed to connect to %s", host.c_str());
 * LOG_DEBUG("Request processed in %d ms", duration);
 *
 * // 停止日志系统 (通常在程序退出时)
 * AsyncLogger::instance().stop();
 * @endcode
 */

#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iostream>
#include <cstdarg>

/**
 * @enum LogLevel
 * @brief 日志级别枚举
 */
enum class LogLevel {
    DEBUG,   ///< 调试信息，最详细
    INFO,    ///< 普通信息
    WARN,    ///< 警告信息
    ERROR,   ///< 错误信息
    FATAL    ///< 致命错误，通常会终止程序
};

/**
 * @struct LogEntry
 * @brief 单条日志记录
 *
 * 存储一条日志的所有信息:
 * - level: 日志级别
 * - timestamp: 时间戳字符串
 * - threadName: 线程名称
 * - file: 源文件名
 * - line: 源代码行号
 * - message: 日志消息
 */
struct LogEntry {
    LogLevel level;           ///< 日志级别
    std::string timestamp;    ///< 时间戳
    std::string threadName;   ///< 线程名
    std::string file;         ///< 源文件名
    int line;                 ///< 行号
    std::string message;      ///< 日志消息
};

/**
 * @class AsyncLogger
 * @brief 异步日志器
 *
 * 使用双缓冲技术实现高性能异步日志:
 * - currentBuffer_: 当前线程写入的缓冲区
 * - flushBuffer_: 后台线程刷新的缓冲区
 *
 * 工作流程:
 * 1. 业务线程调用 log()，将日志写入 currentBuffer_
 * 2. 当 currentBuffer_ 满或超时，交换两个缓冲区
 * 3. 后台线程将 flushBuffer_ 写入文件
 *
 * 线程安全:
 * - 使用 mutex 保护缓冲区交换操作
 * - 日志级别使用原子变量，无需加锁
 *
 * 单例模式:
 * - 使用静态 instance() 方法获取全局实例
 */
class AsyncLogger {
public:
    /**
     * @brief 获取全局日志实例
     * @return AsyncLogger 单例引用
     *
     * 使用静态局部变量实现线程安全的单例
     */
    static AsyncLogger& instance() {
        static AsyncLogger logger;
        return logger;
    }

    /**
     * @brief 设置日志文件路径
     * @param filename 日志文件完整路径
     */
    void setLogFile(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mutex_);
        filename_ = filename;
    }

    /**
     * @brief 设置日志级别
     * @param level 最低日志级别，低于此级别的日志不会被记录
     */
    void setLogLevel(LogLevel level) {
        level_.store(level, std::memory_order_relaxed);
    }

    /**
     * @brief 启动日志线程
     *
     * 启动后台日志写入线程。
     * 幂等操作，多次调用不会创建多个线程。
     */
    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load()) return;  // 幂等：防止重复启动

        running_.store(true);
        writerThread_ = std::thread(&AsyncLogger::writerLoop, this);
    }

    /**
     * @brief 停止日志线程
     *
     * 等待所有日志写入完成后停止日志线程。
     */
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_.load()) return;
            running_.store(false);
        }
        cv_.notify_one();

        if (writerThread_.joinable()) {
            writerThread_.join();
        }
    }

    /**
     * @brief 记录日志 (主日志接口)
     * @param level 日志级别
     * @param file 源文件名
     * @param line 源代码行号
     * @param fmt printf 格式的消息
     * @param ... 可变参数
     *
     * 此方法是非阻塞的，日志会被加入缓冲区，
     * 由后台线程异步写入文件。
     *
     * @note 通过 LOG_* 宏调用，不直接调用此方法
     */
    void log(LogLevel level, const char* file, int line, const char* fmt, ...) {
        // 检查日志级别
        if (level < level_.load(std::memory_order_relaxed)) return;

        // 格式化消息
        char buf[4096];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        // 构造日志条目
        LogEntry entry;
        entry.level = level;
        entry.timestamp = getTimestamp();
        entry.file = file;
        entry.line = line;
        entry.message = buf;

        // 加入当前缓冲
        {
            std::lock_guard<std::mutex> lock(mutex_);
            currentBuffer_->push_back(entry);

            // 缓冲区满时通知后台线程
            if (currentBuffer_->size() >= kFlushThreshold) {
                cv_.notify_one();
            }
        }
    }

private:
    /**
     * @brief 私有构造函数 (单例模式)
     *
     * 初始化双缓冲区
     */
    AsyncLogger()
        : level_(LogLevel::INFO)
        , running_(false)
        , currentBuffer_(&bufferA_)
        , flushBuffer_(&bufferB_)
    {
        bufferA_.reserve(kFlushThreshold);
        bufferB_.reserve(kFlushThreshold);
    }

    /**
     * @brief 析构函数
     *
     * 停止日志线程
     */
    ~AsyncLogger() {
        stop();
    }

    /**
     * @brief 后台写入线程主循环
     *
     * 工作流程:
     * 1. 等待缓冲区满或超时
     * 2. 交换缓冲区
     * 3. 将 flushBuffer_ 写入文件
     * 4. 清空 flushBuffer_
     */
    void writerLoop() {
        std::ofstream file;
        bool fileOpened = false;

        while (running_.load() || !currentBuffer_->empty()) {
            // 等待缓冲区有数据
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return !running_.load() || currentBuffer_->size() >= kFlushThreshold;
                });

                // 交换缓冲区
                std::swap(currentBuffer_, flushBuffer_);
            }

            // 懒打开文件 (首次写入时才打开)
            if (!fileOpened) {
                std::string fname;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    fname = filename_;
                }
                file.open(fname, std::ios::app);
                fileOpened = file.is_open();
                if (!fileOpened) {
                    // 打开文件失败，回退到 stderr
                    for (const auto& entry : *flushBuffer_) {
                        formatEntry(std::cerr, entry);
                    }
                    std::cerr.flush();
                    flushBuffer_->clear();
                    continue;
                }
            }

            // 写入文件 (无锁，效率高)
            for (const auto& entry : *flushBuffer_) {
                formatEntry(file, entry);
            }
            file.flush();
            flushBuffer_->clear();
        }
    }

    /**
     * @brief 格式化单条日志并写入流
     * @param file 输出流
     * @param entry 日志条目
     */
    void formatEntry(std::ostream& file, const LogEntry& entry) {
        const char* levelStr[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};

        file << '[' << entry.timestamp << "] "
             << '[' << levelStr[static_cast<int>(entry.level)] << "] "
             << entry.file << ':' << entry.line << ' '
             << entry.message << '\n';
    }

    /**
     * @brief 生成线程安全的时间戳字符串
     * @return 格式化的时间戳，如 "2024-01-15 10:30:45.123"
     *
     * 使用 localtime_r (POSIX 线程安全版本)
     */
    std::string getTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        // 使用 localtime_r (POSIX 线程安全版本)
        struct tm tm_result;
        localtime_r(&tt, &tm_result);

        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_result);

        return std::string(buf) + '.' + std::to_string(ms.count());
    }

    /// 缓冲区刷新阈值
    static constexpr size_t kFlushThreshold = 1000;

    std::atomic<LogLevel> level_;  ///< 日志级别 (原子变量)
    std::string filename_ = "/tmp/mymuduo.log";  ///< 日志文件路径
    std::atomic<bool> running_;    ///< 运行标志

    std::vector<LogEntry> bufferA_;  ///< 缓冲区 A
    std::vector<LogEntry> bufferB_;  ///< 缓冲区 B
    std::vector<LogEntry>* currentBuffer_;  ///< 当前写入缓冲区
    std::vector<LogEntry>* flushBuffer_;    ///< 待刷新缓冲区

    mutable std::mutex mutex_;       ///< 保护缓冲区
    std::condition_variable cv_;     ///< 缓冲区满通知
    std::thread writerThread_;       ///< 后台写入线程
};

// ==================== 日志宏 ====================

/**
 * @brief 记录 DEBUG 级别日志
 * @param fmt printf 格式字符串
 * @param ... 可变参数
 */
#define LOG_DEBUG(fmt, ...) \
    AsyncLogger::instance().log(LogLevel::DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief 记录 INFO 级别日志
 */
#define LOG_INFO(fmt, ...) \
    AsyncLogger::instance().log(LogLevel::INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief 记录 WARN 级别日志
 */
#define LOG_WARN(fmt, ...) \
    AsyncLogger::instance().log(LogLevel::WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief 记录 ERROR 级别日志
 */
#define LOG_ERROR(fmt, ...) \
    AsyncLogger::instance().log(LogLevel::ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief 记录 FATAL 级别日志
 */
#define LOG_FATAL(fmt, ...) \
    AsyncLogger::instance().log(LogLevel::FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)