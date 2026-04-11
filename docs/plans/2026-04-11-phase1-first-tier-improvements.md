# Phase 1: mymuduo-http 第一梯队改进 实施计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 修复 WebSocketServer 链接错误，新增 MySQLPool/RedisPool 连接池，集成自动心跳和空闲超时，实现 HTTP 限流中间件。

**Architecture:** 所有新模块保持 header-only 风格（与现有代码一致）。连接池基于已有的 `ConnectionPool.h` 模式，新增 MySQL/Redis 特化版本。心跳利用 `EventLoop::runEvery()` 定时器。限流器作为 `HttpServer::use()` 中间件集成。

**Tech Stack:** C++17, libmysqlclient, hiredis, OpenSSL (已有), epoll + timerfd (已有)

---

## Task 1: WebSocketServer 修复 — 内联缺失方法

**Files:**
- Modify: `src/websocket/WebSocketServer.h:250-275` (8 个声明无定义的方法)
- Test: `tests/test_websocket_server.cpp` (新建)

**问题:** `handleHandshake`, `handleWsFrames`, `parseHeaders`, `getHeader`, `strToLower`, `trim`, `sendBadRequest`, `sendForbidden` 这 8 个方法只声明未定义，导致 websocket_server 示例链接错误。

### Step 1: 写测试

创建 `tests/test_websocket_server.cpp`:

```cpp
// test_websocket_server.cpp - WebSocketServer 编译链接测试
#include <iostream>
#include <cassert>
#include "src/websocket/WebSocketServer.h"

using namespace std;

void testCompilation() {
    cout << "=== Testing WebSocketServer Compilation ===" << endl;

    // 验证 WebSocketServer 类可以实例化（不启动）
    EventLoop loop;
    InetAddress addr(0);  // 端口 0，不实际监听
    WebSocketServer server(&loop, addr, "TestWsServer");

    // 验证配置方法
    WebSocketConfig config;
    config.maxMessageSize = 1024;
    config.idleTimeoutMs = 5000;
    config.enablePingPong = true;
    config.pingIntervalMs = 3000;
    server.setConfig(config);

    // 验证回调设置
    server.setConnectionHandler([](const WsSessionPtr&) {});
    server.setMessageHandler([](const WsSessionPtr&, const WsMessage&) {});
    server.setCloseHandler([](const WsSessionPtr&) {});
    server.setErrorHandler([](const WsSessionPtr&, const std::string&) {});
    server.setHandshakeValidator([](const TcpConnectionPtr&, const std::string&,
                                    const std::map<std::string, std::string>&) {
        return true;
    });

    // 验证查询方法
    assert(server.sessionCount() == 0);
    assert(server.getAllSessions().empty());

    cout << "WebSocketServer compilation test passed!" << endl;
}

int main() {
    cout << "Starting WebSocketServer Tests..." << endl << endl;
    testCompilation();
    cout << endl << "All WebSocketServer tests passed!" << endl;
    return 0;
}
```

### Step 2: 运行测试验证失败

```bash
cd build && cmake .. && make test_websocket_server 2>&1
```

Expected: 链接错误 `undefined reference to 'WebSocketServer::handleHandshake'` 等。

### Step 3: 实现缺失方法

将 `WebSocketServer.h` 中 8 个方法声明替换为内联定义。替换 line 250 的 `handleHandshake` 声明：

```cpp
    /// 处理 HTTP 握手
    bool handleHandshake(const TcpConnectionPtr& conn, const WsSessionPtr& session, Buffer* buf) {
        std::string data(buf->peek(), buf->readableBytes());

        // 查找 HTTP 请求头结束标志
        size_t headerEnd = data.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            // 数据不完整，等待更多数据
            if (buf->readableBytes() > 8192) {
                sendBadRequest(conn, "Request Header Too Large");
                conn->shutdown();
            }
            return false;
        }

        // 消费已读取的数据
        std::string headerStr = data.substr(0, headerEnd);
        buf->retrieve(headerEnd + 4);

        // 解析请求行
        size_t firstLine = headerStr.find("\r\n");
        if (firstLine == std::string::npos) {
            sendBadRequest(conn);
            conn->shutdown();
            return false;
        }

        std::string requestLine = headerStr.substr(0, firstLine);
        std::string headerBody = headerStr.substr(firstLine + 2);

        // 验证请求行: GET /path HTTP/1.1
        if (requestLine.find("GET ") != 0) {
            sendBadRequest(conn, "Method Not Allowed");
            conn->shutdown();
            return false;
        }

        // 提取 path
        size_t pathStart = 4;  // "GET " 后
        size_t pathEnd = requestLine.find(' ', pathStart);
        std::string path = (pathEnd != std::string::npos)
            ? requestLine.substr(pathStart, pathEnd - pathStart) : "/";

        // 解析 HTTP 头
        auto headers = parseHeaders(headerBody);

        // 验证 WebSocket 握手必要字段
        std::string upgrade = getHeader(headers, "Upgrade");
        std::string connection = getHeader(headers, "Connection");
        std::string wsKey = getHeader(headers, "Sec-WebSocket-Key");
        std::string wsVersion = getHeader(headers, "Sec-WebSocket-Version");

        if (strToLower(upgrade) != "websocket" ||
            strToLower(connection).find("upgrade") == std::string::npos ||
            wsKey.empty()) {
            sendBadRequest(conn, "Invalid WebSocket Handshake");
            conn->shutdown();
            return false;
        }

        // 自定义握手验证
        if (handshakeValidator_ && !handshakeValidator_(conn, path, headers)) {
            sendForbidden(conn);
            conn->shutdown();
            return false;
        }

        // 计算 Accept Key
        std::string acceptKey = WebSocketFrameCodec::computeAcceptKey(wsKey);

        // 发送握手响应
        std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + acceptKey + "\r\n"
            "\r\n";

        conn->send(response);

        // 更新会话状态
        session->setState(WsState::Open);
        session->setContext("path", path);
        session->updateActivity();

        // 通知连接回调
        if (connectionHandler_) {
            connectionHandler_(session);
        }

        return true;
    }
```

替换 line 253 的 `handleWsFrames` 声明：

```cpp
    /// 处理 WebSocket 帧
    void handleWsFrames(const WsSessionPtr& session, Buffer* buf) {
        while (buf->readableBytes() > 0) {
            auto result = WebSocketFrameCodec::decode(
                reinterpret_cast<const uint8_t*>(buf->peek()),
                buf->readableBytes());

            if (result.status == WebSocketFrameCodec::DecodeResult::Incomplete) {
                break;  // 等待更多数据
            }

            if (result.status == WebSocketFrameCodec::DecodeResult::Error) {
                session->handleError("Frame decode error: " + result.error);
                session->forceClose();
                return;
            }

            buf->retrieve(result.consumed);
            session->updateActivity();

            const auto& frame = result.frame;

            // 检查消息大小
            if (static_cast<int>(frame.payloadSize()) > config_.maxMessageSize) {
                session->handleError("Message too large");
                session->close(1009, "Message Too Big");
                return;
            }

            switch (frame.opcode) {
                case WsOpcode::Text:
                case WsOpcode::Binary: {
                    WsMessage msg;
                    msg.opcode = frame.opcode;
                    msg.data = frame.payload;
                    session->handleMessage(msg);
                    break;
                }
                case WsOpcode::Ping: {
                    // 自动回复 Pong
                    session->pong(frame.payload);
                    break;
                }
                case WsOpcode::Pong: {
                    // 收到 Pong，更新活动时间（已在上面 updateActivity）
                    break;
                }
                case WsOpcode::Close: {
                    session->close();
                    break;
                }
                default:
                    break;
            }
        }
    }
```

替换 line 268-275 的工具方法声明：

```cpp
    /// 解析 HTTP 头
    std::map<std::string, std::string> parseHeaders(const std::string& header) {
        std::map<std::string, std::string> headers;
        size_t pos = 0;
        while (pos < header.size()) {
            size_t lineEnd = header.find("\r\n", pos);
            if (lineEnd == std::string::npos) lineEnd = header.size();

            std::string line = header.substr(pos, lineEnd - pos);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = trim(line.substr(0, colon));
                std::string value = trim(line.substr(colon + 1));
                headers[key] = value;
            }
            pos = lineEnd + 2;
        }
        return headers;
    }

    std::string getHeader(const std::map<std::string, std::string>& headers, const std::string& key) {
        // 大小写不敏感查找
        std::string lowerKey = strToLower(key);
        for (const auto& [k, v] : headers) {
            if (strToLower(k) == lowerKey) return v;
        }
        return "";
    }

    std::string strToLower(const std::string& s) {
        std::string result = s;
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }

    std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    void sendBadRequest(const TcpConnectionPtr& conn, const std::string& msg = "Bad Request") {
        std::string response =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: " + std::to_string(msg.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + msg;
        conn->send(response);
    }

    void sendForbidden(const TcpConnectionPtr& conn) {
        std::string msg = "Forbidden";
        std::string response =
            "HTTP/1.1 403 Forbidden\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: " + std::to_string(msg.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + msg;
        conn->send(response);
    }
```

### Step 4: 编译测试和示例，验证链接成功

```bash
cd build && cmake .. && make test_websocket_server websocket_server -j$(nproc)
./test_websocket_server
```

Expected: 编译成功，测试输出 "All WebSocketServer tests passed!"

### Step 5: Commit

```bash
git add src/websocket/WebSocketServer.h tests/test_websocket_server.cpp
git commit -m "fix: inline missing WebSocketServer methods to fix linker errors

Implement handleHandshake, handleWsFrames, parseHeaders, getHeader,
strToLower, trim, sendBadRequest, sendForbidden as inline methods.
Fixes websocket_server example link failure."
```

---

## Task 2: MySQLPool — MySQL 连接池

**Files:**
- Create: `src/pool/MySQLPool.h`
- Test: `tests/test_mysql_pool.cpp` (新建)

**依赖:** `sudo apt install libmysqlclient-dev`

**CMakeLists.txt 变更:** 需为使用 MySQLPool 的目标添加 `-lmysqlclient` 链接。本 Task 仅新建 header + 编译测试，不改 CMakeLists.txt（在 Task 3 中一起处理）。

### Step 1: 写测试

创建 `tests/test_mysql_pool.cpp`:

```cpp
// test_mysql_pool.cpp - MySQLPool 编译 + 单元测试
#include <iostream>
#include <cassert>
#include "src/pool/MySQLPool.h"

using namespace std;

void testMySQLConnectionWrapper() {
    cout << "=== Testing MySQLConnection Wrapper ===" << endl;

    // 测试无连接时的安全行为
    MySQLConnection conn(nullptr);
    assert(!conn.valid());
    assert(!conn.ping());

    cout << "MySQLConnection wrapper test passed!" << endl;
}

void testMySQLPoolConfig() {
    cout << "=== Testing MySQLPool Config ===" << endl;

    MySQLPoolConfig config;
    config.host = "127.0.0.1";
    config.port = 3306;
    config.user = "root";
    config.password = "";
    config.database = "test";
    config.minSize = 2;
    config.maxSize = 10;
    config.idleTimeoutSec = 60;

    assert(config.host == "127.0.0.1");
    assert(config.port == 3306);
    assert(config.minSize == 2);
    assert(config.maxSize == 10);

    cout << "MySQLPool config test passed!" << endl;
}

void testMySQLPoolCreation() {
    cout << "=== Testing MySQLPool Creation (no DB required) ===" << endl;

    // MySQLPool 构造不需要实际 DB 连接（预创建会失败但不崩溃）
    MySQLPoolConfig config;
    config.host = "127.0.0.1";
    config.port = 3306;
    config.user = "nonexistent";
    config.password = "wrong";
    config.database = "nodb";
    config.minSize = 0;  // 不预创建连接
    config.maxSize = 5;

    MySQLPool pool(config);
    assert(pool.available() == 0);
    assert(pool.totalCreated() == 0);
    assert(!pool.isClosed());

    cout << "MySQLPool creation test passed!" << endl;
}

int main() {
    cout << "Starting MySQLPool Tests..." << endl << endl;

    testMySQLConnectionWrapper();
    testMySQLPoolConfig();
    testMySQLPoolCreation();

    cout << endl << "All MySQLPool tests passed!" << endl;
    return 0;
}
```

### Step 2: 实现 MySQLPool

创建 `src/pool/MySQLPool.h`:

```cpp
/**
 * @file MySQLPool.h
 * @brief MySQL 连接池
 *
 * 基于 libmysqlclient 实现的线程安全 MySQL 连接池。
 * 支持预创建连接、超时获取、自动健康检查、空闲回收。
 *
 * @example
 * @code
 * MySQLPoolConfig config;
 * config.host = "127.0.0.1";
 * config.port = 3306;
 * config.user = "root";
 * config.password = "password";
 * config.database = "mydb";
 *
 * MySQLPool pool(config);
 *
 * auto conn = pool.acquire(3000);
 * if (conn && conn->valid()) {
 *     auto result = conn->query("SELECT * FROM users LIMIT 10");
 *     if (result) {
 *         while (auto row = mysql_fetch_row(result.get())) {
 *             printf("id=%s name=%s\n", row[0], row[1]);
 *         }
 *     }
 *     pool.release(std::move(conn));
 * }
 * @endcode
 */

#pragma once

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <functional>

/// MySQL 连接池配置
struct MySQLPoolConfig {
    std::string host = "127.0.0.1";
    int port = 3306;
    std::string user = "root";
    std::string password;
    std::string database;
    std::string charset = "utf8mb4";
    size_t minSize = 5;
    size_t maxSize = 20;
    int idleTimeoutSec = 60;
    int connectTimeoutSec = 5;
};

/// 单个 MySQL 连接封装
class MySQLConnection {
public:
    using Ptr = std::shared_ptr<MySQLConnection>;
    using ResultPtr = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>;

    explicit MySQLConnection(MYSQL* conn)
        : conn_(conn), lastUsed_(nowSec()) {}

    ~MySQLConnection() {
        if (conn_) {
            mysql_close(conn_);
        }
    }

    // 禁止拷贝
    MySQLConnection(const MySQLConnection&) = delete;
    MySQLConnection& operator=(const MySQLConnection&) = delete;

    bool valid() const { return conn_ != nullptr; }

    /// 执行 Ping 检查连接存活
    bool ping() {
        if (!conn_) return false;
        return mysql_ping(conn_) == 0;
    }

    /// 执行查询（返回结果集）
    ResultPtr query(const std::string& sql) {
        if (!conn_) return ResultPtr(nullptr, mysql_free_result);
        if (mysql_query(conn_, sql.c_str()) != 0) {
            return ResultPtr(nullptr, mysql_free_result);
        }
        return ResultPtr(mysql_store_result(conn_), mysql_free_result);
    }

    /// 执行更新（INSERT/UPDATE/DELETE）
    int execute(const std::string& sql) {
        if (!conn_) return -1;
        if (mysql_query(conn_, sql.c_str()) != 0) return -1;
        return static_cast<int>(mysql_affected_rows(conn_));
    }

    /// 获取最后插入 ID
    uint64_t lastInsertId() const {
        if (!conn_) return 0;
        return mysql_insert_id(conn_);
    }

    /// 获取最后错误信息
    std::string lastError() const {
        if (!conn_) return "No connection";
        return mysql_error(conn_);
    }

    /// 转义字符串（防 SQL 注入）
    std::string escape(const std::string& str) {
        if (!conn_) return "";
        std::string result(str.size() * 2 + 1, '\0');
        unsigned long len = mysql_real_escape_string(conn_, &result[0], str.c_str(), str.size());
        result.resize(len);
        return result;
    }

    /// 底层 MYSQL* 指针
    MYSQL* raw() { return conn_; }

    int64_t lastUsed() const { return lastUsed_; }
    void markUsed() { lastUsed_ = nowSec(); }

private:
    static int64_t nowSec() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    MYSQL* conn_;
    int64_t lastUsed_;
};

/// MySQL 连接池
class MySQLPool {
public:
    explicit MySQLPool(const MySQLPoolConfig& config)
        : config_(config), totalCreated_(0), closed_(false)
    {
        // 预创建连接
        for (size_t i = 0; i < config.minSize; ++i) {
            auto conn = createConnection();
            if (conn && conn->valid()) {
                std::lock_guard<std::mutex> lock(mutex_);
                pool_.push(std::move(conn));
                totalCreated_++;
            }
        }
    }

    ~MySQLPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
        std::lock_guard<std::mutex> lock(mutex_);
        while (!pool_.empty()) pool_.pop();
    }

    /// 获取连接
    MySQLConnection::Ptr acquire(int timeoutMs = 5000) {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return closed_ || !pool_.empty() || totalCreated_ < config_.maxSize; });

        if (closed_) return nullptr;

        // 复用已有连接
        if (!pool_.empty()) {
            auto conn = std::move(pool_.front());
            pool_.pop();
            // Ping 检查连接是否存活
            if (conn->ping()) {
                conn->markUsed();
                return conn;
            }
            // 连接已断开，丢弃并创建新的
            if (totalCreated_ > 0) totalCreated_--;
        }

        // 创建新连接
        if (totalCreated_ < config_.maxSize) {
            totalCreated_++;
            lock.unlock();
            auto conn = createConnection();
            lock.lock();
            if (!conn || !conn->valid()) {
                totalCreated_--;
                return nullptr;
            }
            conn->markUsed();
            return conn;
        }

        return nullptr;
    }

    /// 归还连接
    void release(MySQLConnection::Ptr conn) {
        if (!conn) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || !conn->valid()) {
            if (totalCreated_ > 0) totalCreated_--;
            cv_.notify_one();
            return;
        }
        conn->markUsed();
        pool_.push(std::move(conn));
        cv_.notify_one();
    }

    /// 健康检查：清理空闲连接
    void healthCheck() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;

        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        std::queue<MySQLConnection::Ptr> valid;
        while (!pool_.empty()) {
            auto conn = std::move(pool_.front());
            pool_.pop();
            if (now - conn->lastUsed() > config_.idleTimeoutSec &&
                valid.size() >= config_.minSize) {
                if (totalCreated_ > 0) totalCreated_--;
            } else {
                valid.push(std::move(conn));
            }
        }
        pool_ = std::move(valid);
    }

    size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pool_.size();
    }

    size_t totalCreated() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return totalCreated_;
    }

    bool isClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    MySQLConnection::Ptr createConnection() {
        MYSQL* mysql = mysql_init(nullptr);
        if (!mysql) return std::make_shared<MySQLConnection>(nullptr);

        // 设置连接超时
        unsigned int timeout = config_.connectTimeoutSec;
        mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

        // 自动重连
        bool reconnect = true;
        mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect);

        // 字符集
        mysql_options(mysql, MYSQL_SET_CHARSET_NAME, config_.charset.c_str());

        MYSQL* conn = mysql_real_connect(mysql,
            config_.host.c_str(),
            config_.user.c_str(),
            config_.password.c_str(),
            config_.database.c_str(),
            config_.port, nullptr, 0);

        if (!conn) {
            mysql_close(mysql);
            return std::make_shared<MySQLConnection>(nullptr);
        }

        return std::make_shared<MySQLConnection>(conn);
    }

    MySQLPoolConfig config_;
    size_t totalCreated_;
    bool closed_;

    std::queue<MySQLConnection::Ptr> pool_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};
```

### Step 3: 更新 CMakeLists.txt

在 `# 测试` 部分之前新增 MySQL 查找逻辑：

```cmake
# 查找 MySQL (可选)
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h
    PATHS /usr/include /usr/local/include /usr/include/mysql)
find_library(MYSQL_LIBRARY mysqlclient
    PATHS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu)

if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
    set(MYSQL_FOUND TRUE)
    message(STATUS "MySQL found: ${MYSQL_LIBRARY}")
    include_directories(${MYSQL_INCLUDE_DIR})
else()
    set(MYSQL_FOUND FALSE)
    message(STATUS "MySQL not found, MySQLPool tests will be skipped")
endif()
```

修改测试部分，为 test_mysql_pool 单独链接 mysqlclient:

```cmake
# 测试
file(GLOB TEST_SOURCES tests/*.cpp)
foreach(TEST_FILE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    # 跳过需要额外依赖的测试
    if(TEST_NAME STREQUAL "test_mysql_pool" AND NOT MYSQL_FOUND)
        continue()
    endif()
    if(TEST_NAME STREQUAL "test_redis_pool" AND NOT REDIS_FOUND)
        continue()
    endif()
    add_executable(${TEST_NAME} ${TEST_FILE})
    target_link_libraries(${TEST_NAME} mymuduo nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto pthread)
    # 额外依赖
    if(TEST_NAME STREQUAL "test_mysql_pool" AND MYSQL_FOUND)
        target_link_libraries(${TEST_NAME} ${MYSQL_LIBRARY})
    endif()
    if(TEST_NAME STREQUAL "test_redis_pool" AND REDIS_FOUND)
        target_link_libraries(${TEST_NAME} ${REDIS_LIBRARY})
    endif()
endforeach()
```

### Step 4: 构建测试

```bash
cd build && cmake .. && make test_mysql_pool -j$(nproc) && ./test_mysql_pool
```

Expected: "All MySQLPool tests passed!" (无需实际数据库)

### Step 5: Commit

```bash
git add src/pool/MySQLPool.h tests/test_mysql_pool.cpp CMakeLists.txt
git commit -m "feat: add MySQLPool connection pool with thread-safe acquire/release

Header-only MySQL connection pool based on libmysqlclient.
Supports configurable pool size, idle timeout, health check, and
automatic reconnection via mysql_ping."
```

---

## Task 3: RedisPool — Redis 连接池

**Files:**
- Create: `src/pool/RedisPool.h`
- Test: `tests/test_redis_pool.cpp` (新建)
- Modify: `CMakeLists.txt` (添加 hiredis 查找)

**依赖:** `sudo apt install libhiredis-dev`

### Step 1: 写测试

创建 `tests/test_redis_pool.cpp`:

```cpp
// test_redis_pool.cpp - RedisPool 编译 + 单元测试
#include <iostream>
#include <cassert>
#include "src/pool/RedisPool.h"

using namespace std;

void testRedisConnectionWrapper() {
    cout << "=== Testing RedisConnection Wrapper ===" << endl;

    RedisConnection conn(nullptr);
    assert(!conn.valid());
    assert(!conn.ping());

    cout << "RedisConnection wrapper test passed!" << endl;
}

void testRedisPoolConfig() {
    cout << "=== Testing RedisPool Config ===" << endl;

    RedisPoolConfig config;
    config.host = "127.0.0.1";
    config.port = 6379;
    config.password = "";
    config.db = 0;
    config.minSize = 3;
    config.maxSize = 15;

    assert(config.port == 6379);
    assert(config.db == 0);

    cout << "RedisPool config test passed!" << endl;
}

void testRedisPoolCreation() {
    cout << "=== Testing RedisPool Creation (no Redis required) ===" << endl;

    RedisPoolConfig config;
    config.host = "127.0.0.1";
    config.port = 6379;
    config.minSize = 0;
    config.maxSize = 5;

    RedisPool pool(config);
    assert(pool.available() == 0);
    assert(!pool.isClosed());

    cout << "RedisPool creation test passed!" << endl;
}

int main() {
    cout << "Starting RedisPool Tests..." << endl << endl;

    testRedisConnectionWrapper();
    testRedisPoolConfig();
    testRedisPoolCreation();

    cout << endl << "All RedisPool tests passed!" << endl;
    return 0;
}
```

### Step 2: 实现 RedisPool

创建 `src/pool/RedisPool.h`:

```cpp
/**
 * @file RedisPool.h
 * @brief Redis 连接池
 *
 * 基于 hiredis 实现的线程安全 Redis 连接池。
 * 支持基本的 GET/SET/DEL、过期时间、列表操作等。
 *
 * @example
 * @code
 * RedisPoolConfig config;
 * config.host = "127.0.0.1";
 * config.port = 6379;
 *
 * RedisPool pool(config);
 *
 * auto conn = pool.acquire();
 * if (conn && conn->valid()) {
 *     conn->set("key", "value", 300);  // TTL 300s
 *     auto val = conn->get("key");     // "value"
 *     pool.release(std::move(conn));
 * }
 * @endcode
 */

#pragma once

#include <hiredis/hiredis.h>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>

/// Redis 连接池配置
struct RedisPoolConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int db = 0;
    size_t minSize = 5;
    size_t maxSize = 20;
    int idleTimeoutSec = 60;
    int connectTimeoutSec = 5;
};

/// 单个 Redis 连接封装
class RedisConnection {
public:
    using Ptr = std::shared_ptr<RedisConnection>;

    /// RAII 包装 redisReply
    struct Reply {
        redisReply* raw;
        Reply(redisReply* r) : raw(r) {}
        ~Reply() { if (raw) freeReplyObject(raw); }
        Reply(const Reply&) = delete;
        Reply& operator=(const Reply&) = delete;
        Reply(Reply&& other) noexcept : raw(other.raw) { other.raw = nullptr; }

        bool ok() const { return raw && raw->type != REDIS_REPLY_ERROR; }
        bool isNil() const { return !raw || raw->type == REDIS_REPLY_NIL; }
        std::string str() const {
            if (!raw || raw->type != REDIS_REPLY_STRING) return "";
            return std::string(raw->str, raw->len);
        }
        long long integer() const {
            if (!raw || raw->type != REDIS_REPLY_INTEGER) return 0;
            return raw->integer;
        }
        std::string error() const {
            if (!raw || raw->type != REDIS_REPLY_ERROR) return "";
            return std::string(raw->str, raw->len);
        }
    };

    explicit RedisConnection(redisContext* ctx)
        : ctx_(ctx), lastUsed_(nowSec()) {}

    ~RedisConnection() {
        if (ctx_) redisFree(ctx_);
    }

    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;

    bool valid() const { return ctx_ != nullptr && ctx_->err == 0; }

    bool ping() {
        if (!ctx_) return false;
        auto r = command("PING");
        return r.ok();
    }

    /// 执行任意命令
    Reply command(const char* fmt, ...) {
        if (!ctx_) return Reply(nullptr);
        va_list ap;
        va_start(ap, fmt);
        auto* reply = static_cast<redisReply*>(redisvCommand(ctx_, fmt, ap));
        va_end(ap);
        return Reply(reply);
    }

    /// GET
    std::string get(const std::string& key) {
        auto r = command("GET %s", key.c_str());
        return r.str();
    }

    /// SET (可选 TTL)
    bool set(const std::string& key, const std::string& value, int ttlSec = 0) {
        Reply r = (ttlSec > 0)
            ? command("SET %s %s EX %d", key.c_str(), value.c_str(), ttlSec)
            : command("SET %s %s", key.c_str(), value.c_str());
        return r.ok();
    }

    /// DEL
    bool del(const std::string& key) {
        auto r = command("DEL %s", key.c_str());
        return r.ok();
    }

    /// EXISTS
    bool exists(const std::string& key) {
        auto r = command("EXISTS %s", key.c_str());
        return r.integer() > 0;
    }

    /// EXPIRE
    bool expire(const std::string& key, int seconds) {
        auto r = command("EXPIRE %s %d", key.c_str(), seconds);
        return r.ok();
    }

    /// INCR
    long long incr(const std::string& key) {
        auto r = command("INCR %s", key.c_str());
        return r.integer();
    }

    /// LPUSH
    long long lpush(const std::string& key, const std::string& value) {
        auto r = command("LPUSH %s %s", key.c_str(), value.c_str());
        return r.integer();
    }

    /// LRANGE
    std::vector<std::string> lrange(const std::string& key, int start, int stop) {
        std::vector<std::string> result;
        auto r = command("LRANGE %s %d %d", key.c_str(), start, stop);
        if (r.raw && r.raw->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i < r.raw->elements; ++i) {
                if (r.raw->element[i]->type == REDIS_REPLY_STRING) {
                    result.emplace_back(r.raw->element[i]->str, r.raw->element[i]->len);
                }
            }
        }
        return result;
    }

    /// LTRIM
    bool ltrim(const std::string& key, int start, int stop) {
        auto r = command("LTRIM %s %d %d", key.c_str(), start, stop);
        return r.ok();
    }

    int64_t lastUsed() const { return lastUsed_; }
    void markUsed() { lastUsed_ = nowSec(); }

private:
    static int64_t nowSec() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    redisContext* ctx_;
    int64_t lastUsed_;
};

/// Redis 连接池
class RedisPool {
public:
    explicit RedisPool(const RedisPoolConfig& config)
        : config_(config), totalCreated_(0), closed_(false)
    {
        for (size_t i = 0; i < config.minSize; ++i) {
            auto conn = createConnection();
            if (conn && conn->valid()) {
                std::lock_guard<std::mutex> lock(mutex_);
                pool_.push(std::move(conn));
                totalCreated_++;
            }
        }
    }

    ~RedisPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
        std::lock_guard<std::mutex> lock(mutex_);
        while (!pool_.empty()) pool_.pop();
    }

    RedisConnection::Ptr acquire(int timeoutMs = 5000) {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return closed_ || !pool_.empty() || totalCreated_ < config_.maxSize; });

        if (closed_) return nullptr;

        if (!pool_.empty()) {
            auto conn = std::move(pool_.front());
            pool_.pop();
            if (conn->ping()) {
                conn->markUsed();
                return conn;
            }
            if (totalCreated_ > 0) totalCreated_--;
        }

        if (totalCreated_ < config_.maxSize) {
            totalCreated_++;
            lock.unlock();
            auto conn = createConnection();
            lock.lock();
            if (!conn || !conn->valid()) {
                totalCreated_--;
                return nullptr;
            }
            conn->markUsed();
            return conn;
        }

        return nullptr;
    }

    void release(RedisConnection::Ptr conn) {
        if (!conn) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || !conn->valid()) {
            if (totalCreated_ > 0) totalCreated_--;
            cv_.notify_one();
            return;
        }
        conn->markUsed();
        pool_.push(std::move(conn));
        cv_.notify_one();
    }

    void healthCheck() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;

        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        std::queue<RedisConnection::Ptr> valid;
        while (!pool_.empty()) {
            auto conn = std::move(pool_.front());
            pool_.pop();
            if (now - conn->lastUsed() > config_.idleTimeoutSec &&
                valid.size() >= config_.minSize) {
                if (totalCreated_ > 0) totalCreated_--;
            } else {
                valid.push(std::move(conn));
            }
        }
        pool_ = std::move(valid);
    }

    size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pool_.size();
    }

    size_t totalCreated() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return totalCreated_;
    }

    bool isClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    RedisConnection::Ptr createConnection() {
        struct timeval tv;
        tv.tv_sec = config_.connectTimeoutSec;
        tv.tv_usec = 0;

        redisContext* ctx = redisConnectWithTimeout(
            config_.host.c_str(), config_.port, tv);

        if (!ctx || ctx->err) {
            if (ctx) redisFree(ctx);
            return std::make_shared<RedisConnection>(nullptr);
        }

        // 认证
        if (!config_.password.empty()) {
            auto* reply = static_cast<redisReply*>(
                redisCommand(ctx, "AUTH %s", config_.password.c_str()));
            if (!reply || reply->type == REDIS_REPLY_ERROR) {
                if (reply) freeReplyObject(reply);
                redisFree(ctx);
                return std::make_shared<RedisConnection>(nullptr);
            }
            freeReplyObject(reply);
        }

        // 选择 DB
        if (config_.db > 0) {
            auto* reply = static_cast<redisReply*>(
                redisCommand(ctx, "SELECT %d", config_.db));
            if (reply) freeReplyObject(reply);
        }

        return std::make_shared<RedisConnection>(ctx);
    }

    RedisPoolConfig config_;
    size_t totalCreated_;
    bool closed_;

    std::queue<RedisConnection::Ptr> pool_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};
```

### Step 3: 更新 CMakeLists.txt（添加 hiredis 查找）

在 MySQL 查找后面添加:

```cmake
# 查找 hiredis (可选)
find_path(REDIS_INCLUDE_DIR hiredis/hiredis.h
    PATHS /usr/include /usr/local/include)
find_library(REDIS_LIBRARY hiredis
    PATHS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu)

if(REDIS_INCLUDE_DIR AND REDIS_LIBRARY)
    set(REDIS_FOUND TRUE)
    message(STATUS "hiredis found: ${REDIS_LIBRARY}")
    include_directories(${REDIS_INCLUDE_DIR})
else()
    set(REDIS_FOUND FALSE)
    message(STATUS "hiredis not found, RedisPool tests will be skipped")
endif()
```

### Step 4: 构建测试

```bash
cd build && cmake .. && make test_redis_pool -j$(nproc) && ./test_redis_pool
```

Expected: "All RedisPool tests passed!"

### Step 5: Commit

```bash
git add src/pool/RedisPool.h tests/test_redis_pool.cpp CMakeLists.txt
git commit -m "feat: add RedisPool connection pool with hiredis

Header-only Redis connection pool supporting GET/SET/DEL, TTL,
list operations (LPUSH/LRANGE/LTRIM), INCR, and automatic
connection health check."
```

---

## Task 4: 自动心跳 + 空闲超时

**Files:**
- Modify: `src/websocket/WebSocketServer.h` (添加心跳定时器)
- Modify: `src/http/HttpServer.h` (添加空闲连接超时)

### Step 1: WebSocketServer 心跳 — 修改 start()

在 `WebSocketServer::start()` 中集成定时器：

```cpp
void start() {
    if (started_.exchange(true)) return;
    server_.start();

    // 自动心跳: 定期给所有连接发 Ping
    if (config_.enablePingPong && config_.pingIntervalMs > 0) {
        server_.getLoop()->runEvery(
            config_.pingIntervalMs / 1000.0,
            [this]() {
                auto sessions = getAllSessions();
                for (auto& session : sessions) {
                    if (session->isOpen()) {
                        session->ping();
                    }
                }
            });
    }

    // 空闲超时: 定期检查并关闭超时连接
    if (config_.idleTimeoutMs > 0) {
        server_.getLoop()->runEvery(
            config_.idleTimeoutMs / 2000.0,  // 检查频率 = 超时时间的一半
            [this]() {
                auto sessions = getAllSessions();
                for (auto& session : sessions) {
                    if (session->isOpen() &&
                        session->idleTimeMs() > config_.idleTimeoutMs) {
                        session->close(1000, "Idle timeout");
                    }
                }
            });
    }
}
```

### Step 2: 验证 TcpServer::getLoop() 是否存在

```bash
grep -n "getLoop\|baseLoop\|loop_" src/net/TcpServer.h | head -20
```

如果 TcpServer 没有 `getLoop()` 方法，需要通过构造时保存的 loop 指针访问。WebSocketServer 构造时已有 `loop` 参数，需保存它：

在 WebSocketServer 中添加 `loop_` 成员:

```cpp
private:
    EventLoop* loop_;  // 新增
    TcpServer server_;
    // ...
```

构造函数中初始化:

```cpp
WebSocketServer(EventLoop* loop, const InetAddress& addr, const std::string& name = "WebSocketServer")
    : loop_(loop)  // 新增
    , server_(loop, addr, name)
    // ...
```

然后 start() 中用 `loop_->runEvery(...)` 代替 `server_.getLoop()->runEvery(...)`。

### Step 3: HttpServer 空闲连接超时

在 `HttpServer` 中添加超时配置和逻辑。修改 `HttpServer::onConnection`:

```cpp
/// 连接空闲超时（秒），0 表示不超时
void setIdleTimeout(double seconds) {
    idleTimeoutSec_ = seconds;
}
```

添加 `idleTimeoutSec_` 成员变量：

```cpp
double idleTimeoutSec_ = 60.0;  // 默认 60 秒
```

修改 `onConnection`:

```cpp
void onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected() && idleTimeoutSec_ > 0) {
        // 设置空闲超时定时器
        auto weakConn = std::weak_ptr<TcpConnection>(conn);
        conn->getLoop()->runAfter(idleTimeoutSec_, [weakConn]() {
            auto conn = weakConn.lock();
            if (conn && conn->connected()) {
                conn->shutdown();
            }
        });
    }
}
```

### Step 4: 编译验证

```bash
cd build && cmake .. && make websocket_server http_server test_websocket_server -j$(nproc)
./test_websocket_server
```

Expected: 编译成功，测试通过。

### Step 5: Commit

```bash
git add src/websocket/WebSocketServer.h src/http/HttpServer.h
git commit -m "feat: add auto heartbeat for WebSocket and idle timeout for HTTP

WebSocketServer: periodic Ping via EventLoop::runEvery(), auto-close
idle connections exceeding idleTimeoutMs.
HttpServer: configurable idle connection timeout via setIdleTimeout()."
```

---

## Task 5: 限流 Rate Limiter

**Files:**
- Create: `src/util/RateLimiter.h`
- Modify: `src/http/HttpServer.h` (添加 `useRateLimit()` 便捷方法)
- Test: `tests/test_rate_limiter.cpp` (新建)

### Step 1: 写测试

创建 `tests/test_rate_limiter.cpp`:

```cpp
// test_rate_limiter.cpp - RateLimiter 单元测试
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include "src/util/RateLimiter.h"

using namespace std;

void testTokenBucket() {
    cout << "=== Testing Token Bucket ===" << endl;

    // 每秒 10 个请求
    TokenBucketLimiter limiter(10, 10);

    // 前 10 个请求应该全部通过
    int allowed = 0;
    for (int i = 0; i < 15; ++i) {
        if (limiter.allow("client1")) allowed++;
    }
    assert(allowed == 10);

    // 等待 1 秒后应该恢复
    this_thread::sleep_for(chrono::seconds(1));
    assert(limiter.allow("client1"));

    cout << "Token bucket test passed!" << endl;
}

void testTokenBucketMultiClient() {
    cout << "=== Testing Token Bucket Multi-Client ===" << endl;

    TokenBucketLimiter limiter(5, 5);

    // 不同客户端独立计数
    for (int i = 0; i < 5; ++i) {
        assert(limiter.allow("client_a"));
        assert(limiter.allow("client_b"));
    }

    // 两个客户端都应该被限流
    assert(!limiter.allow("client_a"));
    assert(!limiter.allow("client_b"));

    cout << "Multi-client test passed!" << endl;
}

void testSlidingWindow() {
    cout << "=== Testing Sliding Window ===" << endl;

    // 每秒最多 5 个请求
    SlidingWindowLimiter limiter(5, 1);

    // 前 5 个应该通过
    for (int i = 0; i < 5; ++i) {
        assert(limiter.allow("client1"));
    }

    // 第 6 个被拒绝
    assert(!limiter.allow("client1"));

    // 等待窗口过期
    this_thread::sleep_for(chrono::milliseconds(1100));
    assert(limiter.allow("client1"));

    cout << "Sliding window test passed!" << endl;
}

void testGlobalLimiter() {
    cout << "=== Testing Global Limiter ===" << endl;

    // 全局限流（空 key）
    TokenBucketLimiter limiter(3, 3);

    assert(limiter.allow(""));
    assert(limiter.allow(""));
    assert(limiter.allow(""));
    assert(!limiter.allow(""));

    cout << "Global limiter test passed!" << endl;
}

int main() {
    cout << "Starting RateLimiter Tests..." << endl << endl;

    testTokenBucket();
    testTokenBucketMultiClient();
    testSlidingWindow();
    testGlobalLimiter();

    cout << endl << "All RateLimiter tests passed!" << endl;
    return 0;
}
```

### Step 2: 运行测试验证失败

```bash
cd build && cmake .. && make test_rate_limiter 2>&1
```

Expected: 编译失败，找不到 `RateLimiter.h`

### Step 3: 实现 RateLimiter

创建 `src/util/RateLimiter.h`:

```cpp
/**
 * @file RateLimiter.h
 * @brief HTTP 限流器
 *
 * 提供两种限流算法:
 * - TokenBucketLimiter: 令牌桶算法，支持突发流量
 * - SlidingWindowLimiter: 滑动窗口算法，精确限流
 *
 * 每个 key（通常是客户端 IP）独立限流。
 *
 * @example
 * @code
 * // 令牌桶: 每秒 100 个请求，突发最多 100
 * TokenBucketLimiter limiter(100, 100);
 *
 * // 在 HttpServer 中间件中使用
 * server.use([&limiter](const HttpRequest& req, HttpResponse& resp) {
 *     std::string ip = req.getHeader("x-real-ip");
 *     if (ip.empty()) ip = "unknown";
 *     if (!limiter.allow(ip)) {
 *         resp.setStatusCode(HttpStatusCode::TOO_MANY_REQUESTS);
 *         resp.setText("Rate limit exceeded");
 *     }
 * });
 * @endcode
 */

#pragma once

#include <string>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <chrono>

/// 令牌桶限流器
class TokenBucketLimiter {
public:
    /**
     * @brief 构造令牌桶
     * @param rate 每秒补充的令牌数
     * @param burst 令牌桶容量（允许的突发量）
     */
    TokenBucketLimiter(double rate, int burst)
        : rate_(rate), burst_(burst) {}

    /**
     * @brief 检查是否允许请求
     * @param key 客户端标识（通常是 IP）
     * @return true 允许, false 限流
     */
    bool allow(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = nowMs();

        auto it = buckets_.find(key);
        if (it == buckets_.end()) {
            buckets_[key] = {static_cast<double>(burst_ - 1), now};
            return true;
        }

        auto& bucket = it->second;
        // 补充令牌
        double elapsed = (now - bucket.lastTime) / 1000.0;
        bucket.tokens = std::min(static_cast<double>(burst_),
                                  bucket.tokens + elapsed * rate_);
        bucket.lastTime = now;

        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            return true;
        }

        return false;
    }

private:
    struct Bucket {
        double tokens;
        int64_t lastTime;  // 毫秒
    };

    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    double rate_;
    int burst_;
    std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
};

/// 滑动窗口限流器
class SlidingWindowLimiter {
public:
    /**
     * @brief 构造滑动窗口
     * @param maxRequests 窗口内最大请求数
     * @param windowSec 窗口大小（秒）
     */
    SlidingWindowLimiter(int maxRequests, int windowSec)
        : maxRequests_(maxRequests), windowMs_(windowSec * 1000) {}

    bool allow(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = nowMs();

        auto& window = windows_[key];

        // 清除过期的请求记录
        while (!window.empty() && (now - window.front()) > windowMs_) {
            window.pop_front();
        }

        if (static_cast<int>(window.size()) >= maxRequests_) {
            return false;
        }

        window.push_back(now);
        return true;
    }

private:
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    int maxRequests_;
    int64_t windowMs_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::deque<int64_t>> windows_;
};
```

### Step 4: HttpServer 集成便捷方法

在 `HttpServer` 中添加 `useRateLimit()` 方法（在 `enableCors()` 之后）:

```cpp
    /**
     * @brief 启用 HTTP 限流
     * @param maxRequestsPerSec 每秒最大请求数（per IP）
     *
     * 使用令牌桶算法，按客户端 IP 独立限流。
     * 被限流的请求返回 429 Too Many Requests。
     */
    void useRateLimit(int maxRequestsPerSec) {
        if (started_.load()) return;

        auto limiter = std::make_shared<TokenBucketLimiter>(
            maxRequestsPerSec, maxRequestsPerSec);

        use([limiter](const HttpRequest& req, HttpResponse& resp) {
            std::string ip = req.getHeader("x-real-ip");
            if (ip.empty()) ip = req.getHeader("x-forwarded-for");
            if (ip.empty()) ip = "unknown";

            if (!limiter->allow(ip)) {
                resp.setStatusCode(HttpStatusCode::TOO_MANY_REQUESTS);
                resp.setText("Rate limit exceeded");
            }
        });
    }
```

需要在 HttpServer.h 顶部添加 include:

```cpp
#include "util/RateLimiter.h"
```

**注意:** 现有中间件执行后不会阻断后续路由匹配。需要在 `handleRequest` 中检查中间件是否已设置状态码来阻断。修改 `handleRequest`:

```cpp
void handleRequest(const HttpRequest& request, HttpResponse& response) {
    // 执行中间件
    for (auto& middleware : middlewares_) {
        middleware(request, response);
        // 如果中间件设置了错误状态码，停止处理
        if (static_cast<int>(response.statusCode) >= 400) {
            return;
        }
    }
    // ... 路由匹配（不变）
```

### Step 5: 编译运行测试

```bash
cd build && cmake .. && make test_rate_limiter -j$(nproc) && ./test_rate_limiter
```

Expected: "All RateLimiter tests passed!"

### Step 6: 验证 HttpServer 编译

```bash
cd build && make http_server -j$(nproc)
```

### Step 7: Commit

```bash
git add src/util/RateLimiter.h src/http/HttpServer.h tests/test_rate_limiter.cpp
git commit -m "feat: add rate limiter with token bucket and sliding window

TokenBucketLimiter: per-key rate limiting with burst support.
SlidingWindowLimiter: precise windowed rate limiting.
HttpServer::useRateLimit(): one-line middleware integration.
Middleware now short-circuits on error status codes (>= 400)."
```

---

## Task 6: 全量编译验证 + 更新文档

### Step 1: 全量编译

```bash
cd build && cmake .. && make -j$(nproc) 2>&1
```

Expected: 所有目标编译成功（0 errors）。

### Step 2: 运行所有测试

```bash
cd build
for test in test_websocket_server test_websocket_frame test_buffer test_config test_eventloop test_http test_timer test_load_balancer; do
    echo "--- Running $test ---"
    ./$test || echo "FAILED: $test"
done
```

如果有 test_mysql_pool 和 test_redis_pool 编译成功也运行它们。

### Step 3: 更新 CLAUDE.md 勾选完成项

将 CLAUDE.md 中阶段 1 的 `[ ]` 改为 `[x]`：

```markdown
### 阶段 1: mymuduo-http 第一梯队改进
- [x] WebSocketServer 修复（内联缺失方法）
- [x] 连接池集成（MySQLPool + RedisPool）
- [x] 自动心跳 + 空闲超时（Timer 集成）
- [x] 限流 Rate Limiter（令牌桶 + 滑动窗口）
```

### Step 4: Commit

```bash
git add CLAUDE.md
git commit -m "docs: mark Phase 1 improvements as completed"
```

---

## 总结

| Task | 文件 | 类型 |
|------|------|------|
| 1. WebSocketServer 修复 | `src/websocket/WebSocketServer.h` | 修改 |
| 2. MySQLPool | `src/pool/MySQLPool.h` | 新建 |
| 3. RedisPool | `src/pool/RedisPool.h` | 新建 |
| 4. 心跳 + 超时 | `WebSocketServer.h` + `HttpServer.h` | 修改 |
| 5. RateLimiter | `src/util/RateLimiter.h` + `HttpServer.h` | 新建 + 修改 |
| 6. 验证 + 文档 | CLAUDE.md | 修改 |

**新增文件:** 5 个（3 header + 3 test）
**修改文件:** 3 个（WebSocketServer.h, HttpServer.h, CMakeLists.txt）
**依赖:** libmysqlclient-dev, libhiredis-dev（可选，缺失时跳过对应测试）
