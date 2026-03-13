// AsyncLogger.h - 异步日志系统
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

// 日志级别
enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

// 日志条目
struct LogEntry {
    LogLevel level;
    std::string timestamp;
    std::string threadName;
    std::string file;
    int line;
    std::string message;
};

// 异步日志器 - 双缓冲技术
class AsyncLogger {
public:
    static AsyncLogger& instance() {
        static AsyncLogger logger;
        return logger;
    }
    
    void setLogFile(const std::string& filename) {
        filename_ = filename;
    }
    
    void setLogLevel(LogLevel level) {
        level_ = level;
    }
    
    void start() {
        running_ = true;
        writerThread_ = std::thread(&AsyncLogger::writerLoop, this);
    }
    
    void stop() {
        running_ = false;
        cv_.notify_one();
        if (writerThread_.joinable()) {
            writerThread_.join();
        }
    }
    
    // 主日志接口（非阻塞）
    void log(LogLevel level, const char* file, int line, const char* fmt, ...) {
        if (level < level_) return;
        
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
            
            if (currentBuffer_->size() >= kFlushThreshold) {
                cv_.notify_one();
            }
        }
    }

private:
    AsyncLogger()
        : level_(LogLevel::INFO)
        , running_(false)
        , currentBuffer_(&bufferA_)
        , flushBuffer_(&bufferB_)
    {
        bufferA_.reserve(kFlushThreshold);
        bufferB_.reserve(kFlushThreshold);
    }
    
    ~AsyncLogger() {
        stop();
    }
    
    void writerLoop() {
        std::ofstream file(filename_, std::ios::app);
        
        while (running_ || !currentBuffer_->empty()) {
            // 等待缓冲区有数据
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return !running_ || currentBuffer_->size() >= kFlushThreshold;
                });
                
                // 交换缓冲区
                std::swap(currentBuffer_, flushBuffer_);
            }
            
            // 写入文件（无锁）
            for (const auto& entry : *flushBuffer_) {
                formatEntry(file, entry);
            }
            file.flush();
            flushBuffer_->clear();
        }
    }
    
    void formatEntry(std::ofstream& file, const LogEntry& entry) {
        const char* levelStr[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
        
        file << '[' << entry.timestamp << "] "
             << '[' << levelStr[static_cast<int>(entry.level)] << "] "
             << entry.file << ':' << entry.line << ' '
             << entry.message << '\n';
    }
    
    std::string getTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&tt));
        
        return std::string(buf) + '.' + std::to_string(ms.count());
    }
    
    static constexpr size_t kFlushThreshold = 1000;
    
    LogLevel level_;
    std::string filename_ = "/tmp/mymuduo.log";
    std::atomic<bool> running_;
    
    std::vector<LogEntry> bufferA_;
    std::vector<LogEntry> bufferB_;
    std::vector<LogEntry>* currentBuffer_;
    std::vector<LogEntry>* flushBuffer_;
    
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread writerThread_;
};

// 日志宏
#define LOG_DEBUG(fmt, ...) \
    AsyncLogger::instance().log(LogLevel::DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    AsyncLogger::instance().log(LogLevel::INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    AsyncLogger::instance().log(LogLevel::WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    AsyncLogger::instance().log(LogLevel::ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_FATAL(fmt, ...) \
    AsyncLogger::instance().log(LogLevel::FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)