/**
 * @file MySQLPool.h
 * @brief MySQL 连接池
 *
 * 本文件定义了 MySQLConnection 和 MySQLPool 类，
 * 实现了 MySQL 数据库连接的池化管理。
 *
 * 设计目标:
 * - 复用连接，减少创建/销毁开销
 * - 控制最大连接数，防止资源耗尽
 * - 自动健康检查，清理空闲连接
 * - 线程安全
 *
 * @example 使用示例
 * @code
 * MySQLPoolConfig config;
 * config.host = "127.0.0.1";
 * config.port = 3306;
 * config.user = "root";
 * config.password = "secret";
 * config.database = "mydb";
 *
 * MySQLPool pool(config);
 *
 * auto conn = pool.acquire(5000);
 * if (conn && conn->valid()) {
 *     auto res = conn->query("SELECT * FROM users");
 *     int affected = conn->execute("UPDATE users SET active=1 WHERE id=1");
 *     pool.release(conn);
 * }
 *
 * pool.healthCheck();
 * @endcode
 */

#pragma once

#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <functional>
#include <mysql/mysql.h>

/**
 * @struct MySQLPoolConfig
 * @brief MySQL 连接池配置
 */
struct MySQLPoolConfig {
    std::string host = "127.0.0.1";     ///< 主机地址
    int port = 3306;                     ///< 端口号
    std::string user = "root";           ///< 用户名
    std::string password;                ///< 密码
    std::string database;                ///< 数据库名
    std::string charset = "utf8mb4";     ///< 字符集

    size_t minSize = 5;                  ///< 最小连接数 (预创建)
    size_t maxSize = 20;                 ///< 最大连接数
    int idleTimeoutSec = 60;             ///< 空闲超时 (秒)
    int connectTimeoutSec = 5;           ///< 连接超时 (秒)
};

/**
 * @class MySQLConnection
 * @brief 单个 MySQL 连接封装 (RAII)
 *
 * 封装 MYSQL* 句柄，析构时自动关闭连接。
 * 提供 query/execute/ping 等操作。
 */
class MySQLConnection {
public:
    using Ptr = std::shared_ptr<MySQLConnection>;
    using ResultPtr = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>;

    /**
     * @brief 构造连接
     * @param mysql MYSQL* 句柄，可为 nullptr
     */
    explicit MySQLConnection(MYSQL* mysql)
        : mysql_(mysql), lastUsed_(0)
    {}

    /**
     * @brief 析构函数，自动关闭连接
     */
    ~MySQLConnection() {
        if (mysql_) {
            mysql_close(mysql_);
            mysql_ = nullptr;
        }
    }

    // 不可拷贝
    MySQLConnection(const MySQLConnection&) = delete;
    MySQLConnection& operator=(const MySQLConnection&) = delete;

    /**
     * @brief 检查连接是否有效
     * @return mysql_ 非空为 true
     */
    bool valid() const { return mysql_ != nullptr; }

    /**
     * @brief ping 检测连接是否存活
     * @return 存活返回 true
     */
    bool ping() {
        if (!mysql_) return false;
        return mysql_ping(mysql_) == 0;
    }

    /**
     * @brief 执行查询，返回结果集
     * @param sql SQL 语句
     * @return 结果集 (ResultPtr)，失败返回 nullptr
     */
    ResultPtr query(const std::string& sql) {
        if (!mysql_) return ResultPtr(nullptr, mysql_free_result);
        if (mysql_query(mysql_, sql.c_str()) != 0) {
            return ResultPtr(nullptr, mysql_free_result);
        }
        return ResultPtr(mysql_store_result(mysql_), mysql_free_result);
    }

    /**
     * @brief 执行非查询语句 (INSERT/UPDATE/DELETE)
     * @param sql SQL 语句
     * @return 影响的行数，-1 表示失败
     */
    int execute(const std::string& sql) {
        if (!mysql_) return -1;
        if (mysql_query(mysql_, sql.c_str()) != 0) {
            return -1;
        }
        return static_cast<int>(mysql_affected_rows(mysql_));
    }

    /**
     * @brief 获取最后插入的自增 ID
     */
    uint64_t lastInsertId() {
        if (!mysql_) return 0;
        return mysql_insert_id(mysql_);
    }

    /**
     * @brief 获取最后的错误信息
     */
    std::string lastError() {
        if (!mysql_) return "connection is null";
        return mysql_error(mysql_);
    }

    /**
     * @brief 转义字符串，防止 SQL 注入
     * @param str 原始字符串
     * @return 转义后的字符串
     */
    std::string escape(const std::string& str) {
        if (!mysql_) return str;
        std::string result(str.size() * 2 + 1, '\0');
        unsigned long len = mysql_real_escape_string(mysql_, &result[0], str.c_str(), str.size());
        result.resize(len);
        return result;
    }

    /**
     * @brief 获取原始 MYSQL* 句柄
     */
    MYSQL* raw() { return mysql_; }

    /**
     * @brief 获取最后使用时间
     * @return Unix 时间戳 (秒)
     */
    int64_t lastUsed() const { return lastUsed_; }

    /**
     * @brief 标记为已使用
     */
    void markUsed() {
        lastUsed_ = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

private:
    MYSQL* mysql_;          ///< MySQL 句柄
    int64_t lastUsed_;      ///< 最后使用时间 (秒)
};

/**
 * @class MySQLPool
 * @brief MySQL 连接池 (线程安全版)
 *
 * 管理 MySQLConnection 对象的池:
 * - 预创建最小数量的连接
 * - 按需创建新连接，直到最大数量
 * - 自动清理空闲连接
 * - 线程安全
 */
class MySQLPool {
public:
    /**
     * @brief 构造连接池
     * @param config 连接池配置
     *
     * 预创建 minSize 个连接，失败静默跳过
     */
    explicit MySQLPool(const MySQLPoolConfig& config)
        : config_(config)
        , totalCreated_(0)
        , closed_(false)
    {
        // 预创建连接 (失败静默跳过)
        std::vector<MySQLConnection::Ptr> initialConns;
        for (size_t i = 0; i < config_.minSize; ++i) {
            auto conn = createConnection();
            if (conn && conn->valid()) {
                conn->markUsed();
                initialConns.push_back(conn);
            }
        }

        // 一次性加入池中
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& conn : initialConns) {
            pool_.push(conn);
        }
        totalCreated_ = initialConns.size();
    }

    /**
     * @brief 析构函数
     *
     * 安全关闭，唤醒所有等待的线程
     */
    ~MySQLPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();

        // 清空连接
        std::lock_guard<std::mutex> lock(mutex_);
        while (!pool_.empty()) {
            pool_.pop();
        }
    }

    /**
     * @brief 获取连接
     * @param timeoutMs 超时时间 (毫秒)
     * @return 连接指针，nullptr 表示失败
     *
     * 优先复用现有连接 (带 ping 检查)，否则创建新连接
     */
    MySQLConnection::Ptr acquire(int timeoutMs = 5000) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 等待可用连接或池未满
        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return closed_ || !pool_.empty() || totalCreated_ < config_.maxSize; });

        if (closed_) return nullptr;

        // 复用现有连接 (带 ping 检查)
        while (!pool_.empty()) {
            auto conn = pool_.front();
            pool_.pop();

            if (conn->ping()) {
                conn->markUsed();
                return conn;
            }
            // ping 失败，丢弃连接
            if (totalCreated_ > 0) totalCreated_--;
        }

        // 创建新连接 (在锁外执行，避免阻塞)
        if (totalCreated_ < config_.maxSize) {
            totalCreated_++;  // 先占位
            lock.unlock();

            auto conn = createConnection();

            lock.lock();
            if (!conn || !conn->valid()) {
                totalCreated_--;  // 创建失败，回退
                return nullptr;
            }

            conn->markUsed();
            return conn;
        }

        return nullptr;
    }

    /**
     * @brief 归还连接
     * @param conn 连接指针
     *
     * 无效连接会被丢弃
     */
    void release(MySQLConnection::Ptr conn) {
        if (!conn) return;

        if (!conn->valid()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (totalCreated_ > 0) totalCreated_--;
            cv_.notify_one();
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (closed_ || pool_.size() >= config_.maxSize) {
            if (totalCreated_ > 0) totalCreated_--;
        } else {
            pool_.push(conn);
        }

        cv_.notify_one();
    }

    /**
     * @brief 健康检查 (清理空闲连接)
     *
     * 清理超过 idleTimeoutSec 秒未使用的连接，但保留 minSize 个
     */
    void healthCheck() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (closed_) return;

        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        std::queue<MySQLConnection::Ptr> valid;

        while (!pool_.empty()) {
            auto conn = pool_.front();
            pool_.pop();

            // 清理超过 idleTimeoutSec 未使用的连接，但保留 minSize 个
            if (now - conn->lastUsed() > config_.idleTimeoutSec && valid.size() >= config_.minSize) {
                if (totalCreated_ > 0) totalCreated_--;
            } else {
                valid.push(conn);
            }
        }

        pool_ = std::move(valid);
    }

    /// 获取可用连接数
    size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pool_.size();
    }

    /// 获取已创建的总连接数
    size_t totalCreated() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return totalCreated_;
    }

    /// 检查池是否已关闭
    bool isClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    /**
     * @brief 创建新的 MySQL 连接
     * @return MySQLConnection::Ptr，失败返回含 nullptr 的连接
     */
    MySQLConnection::Ptr createConnection() {
        MYSQL* mysql = mysql_init(nullptr);
        if (!mysql) {
            return std::make_shared<MySQLConnection>(nullptr);
        }

        // 设置连接超时
        unsigned int timeout = static_cast<unsigned int>(config_.connectTimeoutSec);
        mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

        // 设置自动重连
        bool reconnect = true;
        mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect);

        // 设置字符集
        mysql_options(mysql, MYSQL_SET_CHARSET_NAME, config_.charset.c_str());

        // 连接数据库
        MYSQL* result = mysql_real_connect(
            mysql,
            config_.host.c_str(),
            config_.user.c_str(),
            config_.password.c_str(),
            config_.database.c_str(),
            static_cast<unsigned int>(config_.port),
            nullptr,  // unix_socket
            0         // client_flag
        );

        if (!result) {
            mysql_close(mysql);
            return std::make_shared<MySQLConnection>(nullptr);
        }

        return std::make_shared<MySQLConnection>(mysql);
    }

    MySQLPoolConfig config_;                    ///< 连接池配置
    size_t totalCreated_;                       ///< 已创建连接总数
    bool closed_;                               ///< 是否已关闭

    std::queue<MySQLConnection::Ptr> pool_;     ///< 空闲连接队列
    mutable std::mutex mutex_;                  ///< 保护队列
    std::condition_variable cv_;                ///< 等待条件
};
