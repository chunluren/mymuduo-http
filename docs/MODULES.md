# mymuduo-http 模块详解

> 本文档详细介绍每个模块的实现原理、关键技术点和面试亮点

---

## 目录

1. [HTTP 服务器模块](#1-http-服务器模块)
2. [RPC 框架模块](#2-rpc-框架模块)
3. [定时器模块](#3-定时器模块)
4. [异步日志模块](#4-异步日志模块)
5. [连接池模块](#5-连接池模块)
6. [配置管理模块](#6-配置管理模块)
7. [负载均衡模块](#7-负载均衡模块)
8. [服务注册中心模块](#8-服务注册中心模块)
9. [WebSocket 模块](#9-websocket-模块)

---

## 1. HTTP 服务器模块

### 文件结构
```
src/http/
├── HttpRequest.h    # HTTP 请求解析
├── HttpResponse.h   # HTTP 响应构造
└── HttpServer.h     # HTTP 服务器核心
```

### 核心实现

#### 1.1 请求解析状态机

**问题：** HTTP 请求可能分多个 TCP 包到达，需要正确处理粘包和流水线请求。

**解决方案：** 三态解析模型

```cpp
enum class ParseResult {
    Complete,    // 解析完成
    Incomplete,  // 数据不完整，等待更多数据
    Error        // 解析错误
};

ParseResult parseRequest(Buffer* buf, HttpRequest& request) {
    // 1. 先 peek 数据，不消费
    const char* data = buf->peek();
    
    // 2. 找请求头结束位置
    const char* headerEnd = memmem(data, len, "\r\n\r\n", 4);
    if (!headerEnd) {
        return ParseResult::Incomplete;  // 等待更多数据
    }
    
    // 3. 检查请求体是否完整
    size_t contentLen = request.contentLength();
    if (len < headerLen + contentLen) {
        return ParseResult::Incomplete;
    }
    
    // 4. 确认完整后才消费数据
    buf->retrieve(totalLen);
    return ParseResult::Complete;
}
```

**面试亮点：**
- 正确处理 TCP 粘包问题
- 支持 HTTP 流水线（Pipelining）
- 不丢失未消费数据

#### 1.2 Keep-Alive 连接复用

```cpp
bool keepAlive() const {
    if (version == HttpVersion::HTTP_10) {
        // HTTP/1.0 需要显式声明
        return getHeader("connection") == "keep-alive";
    }
    // HTTP/1.1 默认 keep-alive
    return getHeader("connection") != "close";
}
```

**面试亮点：**
- 理解 HTTP/1.0 vs HTTP/1.1 的 Keep-Alive 差异
- 减少 TCP 三次握手开销
- 提升并发性能

#### 1.3 安全防护

```cpp
// 请求体大小限制
if (contentLen > kMaxBodySize) {
    return HttpResponse::badRequest("Request body too large");
}

// 路径遍历防护
if (filename.find("..") != std::string::npos || filename[0] == '/') {
    return HttpResponse::badRequest("Invalid path");
}
```

---

## 2. RPC 框架模块

### 文件结构
```
src/rpc/
├── RpcServer.h      # JSON-RPC 服务端
├── RpcClient.h      # JSON-RPC 客户端
├── RpcServerPb.h    # Protobuf-RPC 服务端
├── RpcClientPb.h    # Protobuf-RPC 客户端
└── proto/rpc.proto  # Protobuf 协议定义
```

### 核心实现

#### 2.1 JSON-RPC vs Protobuf-RPC

| 特性 | JSON-RPC | Protobuf-RPC |
|------|----------|--------------|
| 序列化速度 | 基准 | **快 5-10x** |
| 数据大小 | 基准 | **小 3-5x** |
| 可读性 | 好 | 差 |
| 类型安全 | 弱 | **强** |

#### 2.2 类型安全的服务注册

```cpp
template<typename T1, typename T2>
void registerMethod(const std::string& serviceName, 
                    const std::string& methodName,
                    std::function<void(const T1&, T2&)> handler) {
    std::string key = serviceName + "." + methodName;
    
    std::lock_guard<std::mutex> lock(mutex_);  // 线程安全
    methods_[key] = [handler](const Message& req, Message& resp) {
        handler(static_cast<const T1&>(req), static_cast<T2&>(resp));
    };
}
```

**面试亮点：**
- 模板元编程，编译期类型检查
- 线程安全的服务注册
- 自动序列化/反序列化

#### 2.3 帧边界与安全防护

```cpp
// 帧长度校验
if (len <= 0 || static_cast<size_t>(len) > kMaxFrameSize) {
    sendError(conn, 0, -32600, "Invalid frame size");
    conn->shutdown();
    return;
}

// 粘包处理
while (buf->readableBytes() >= 4) {
    // 循环消费多帧
}
```

---

## 3. 定时器模块

### 文件结构
```
src/timer/
├── Timer.h        # 定时器对象
└── TimerQueue.h   # 时间轮队列
```

### 核心实现

#### 3.1 时间轮算法

**数据结构：**
```
时间轮 (60 个桶，每桶 1 秒)

  [0] → Timer1 → Timer2
  [1] → Timer3
  [2] → empty
  ...
  [59] → TimerN
    ↑
 currentBucket
```

**复杂度对比：**

| 操作 | 最小堆 | 红黑树 | **时间轮** |
|------|--------|--------|------------|
| 添加 | O(log n) | O(log n) | **O(1)** |
| 删除 | O(log n) | O(log n) | **O(1)** |
| 触发 | O(1) | O(1) | O(n/m) |

#### 3.2 避免死锁设计

```cpp
void tick() {
    std::vector<...> expiredTimers;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 收集待执行回调
        for (auto& timer : bucket) {
            expiredTimers.push_back(timer);
        }
    }
    
    // 锁外执行回调，避免死锁
    for (auto& [timer, repeat] : expiredTimers) {
        timer->run();
    }
}
```

**面试亮点：**
- O(1) 添加/删除复杂度
- 回调锁外执行，避免死锁
- 支持海量定时器（心跳检测）

---

## 4. 异步日志模块

### 文件结构
```
src/asynclogger/
└── AsyncLogger.h  # 异步日志器
```

### 核心实现

#### 4.1 双缓冲技术

```
业务线程                    后台线程
    │                          │
    ▼                          ▼
┌─────────┐              ┌─────────┐
│ Buffer A│              │ Buffer B│
│ (写入)  │              │ (刷盘)  │
└─────────┘              └─────────┘
    │                          │
    │     交换缓冲区            │
    ├──────────────────────────▶
    │                          │
    │                      写入文件
```

#### 4.2 线程安全实现

```cpp
// 原子变量
std::atomic<LogLevel> level_;
std::atomic<bool> running_;

// 线程安全的时间戳
std::string getTimestamp() {
    struct tm tm_result;
    localtime_r(&tt, &tm_result);  // POSIX 线程安全版本
    // ...
}

// 幂等启动
void start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load()) return;  // 防止重复启动
    running_.store(true);
    writerThread_ = std::thread(...);
}
```

**面试亮点：**
- 双缓冲，不阻塞业务线程
- 批量写入，减少 IO 次数
- 线程安全设计

---

## 5. 连接池模块

### 文件结构
```
src/pool/
└── ConnectionPool.h  # TCP 连接池
```

### 核心实现

#### 5.1 连接复用机制

```cpp
Connection::Ptr acquire(int timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // 等待可用连接
    cv_.wait_for(lock, ...);
    
    // 复用现有连接
    if (!pool_.empty()) {
        auto conn = pool_.front();
        pool_.pop();
        return conn;
    }
    
    // 创建新连接（锁外执行）
    if (totalCreated_ < maxSize_) {
        totalCreated_++;  // 先占位
        lock.unlock();
        int fd = createConnection(...);  // 锁外执行
        // ...
    }
}
```

#### 5.2 生命周期管理

```cpp
~ConnectionPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    cv_.notify_all();  // 唤醒所有等待线程
    
    // 安全清空
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pool_.empty()) pool_.pop();
}
```

**面试亮点：**
- 锁外创建连接，避免阻塞
- 安全的生命周期管理
- 健康检查机制

---

## 6. 配置管理模块

### 文件结构
```
src/config/
└── Config.h  # 配置管理器
config/
└── server.conf  # 配置文件示例
```

### 核心实现

#### 6.1 INI 格式解析

```ini
[server]
port = 8080
threads = 4

[log]
level = INFO
file = /var/log/app.log
```

#### 6.2 类型安全的访问

```cpp
int port = CONFIG_INT("server.port");
std::string logFile = CONFIG_STRING("log.file");
bool debug = CONFIG_BOOL("debug.enabled");
```

---

## 面试常见问题

### Q1: HTTP 和 RPC 有什么区别？

| HTTP | RPC |
|------|-----|
| 通用协议 | 服务间调用协议 |
| 文本协议 | 二进制协议（高效） |
| 无状态 | 可以有状态 |
| 适合对外 API | 适合内部服务调用 |

### Q2: 为什么用 Protobuf 而不是 JSON？

| 指标 | JSON | Protobuf |
|------|------|----------|
| 性能 | 慢 | **快 5-10x** |
| 大小 | 大 | **小 3-5x** |
| 类型安全 | 弱 | **强** |

### Q3: 时间轮为什么是 O(1)？

- **添加：** 直接计算桶索引，插入链表
- **删除：** 通过 hash map 找到 timer，标记取消
- **触发：** 遍历当前桶的链表

### Q4: 异步日志为什么用双缓冲？

- 业务线程只写入内存，不阻塞
- 后台线程批量写入，减少 IO
- 缓冲区交换只锁一次

### Q5: 连接池的最大连接数怎么确定？

- **最小值：** 避免频繁创建销毁
- **最大值：** 根据并发量和数据库能力调整
- **经验值：** CPU 核心数 × 2 + 有效磁盘数

---

## 7. 负载均衡模块

### 文件结构
```
src/balancer/
└── LoadBalancer.h  # 负载均衡策略
```

### 核心实现

#### 7.1 策略模式设计

```cpp
// 策略接口
class ILoadBalanceStrategy {
public:
    virtual BackendServerPtr select(const std::vector<BackendServerPtr>& servers) = 0;
    virtual std::string name() const = 0;
};

// 策略工厂
class LoadBalancer {
    enum class Strategy {
        RoundRobin,
        WeightedRoundRobin,
        LeastConnections,
        Random,
        ConsistentHash
    };
    // ...
};
```

#### 7.2 平滑加权轮询（Nginx 算法）

```cpp
BackendServerPtr select() {
    int totalWeight = 0;
    BackendServerPtr selected = nullptr;

    for (auto& server : servers) {
        server->currentWeight += server->weight;
        totalWeight += server->weight;

        if (!selected || server->currentWeight > selected->currentWeight) {
            selected = server;
        }
    }

    selected->currentWeight -= totalWeight;
    return selected;
}
```

**面试亮点：**
- 策略模式，易扩展
- 平滑加权轮询，避免请求分布不均
- 一致性哈希，支持缓存场景

---

## 8. 服务注册中心模块

### 文件结构
```
src/registry/
├── ServiceMeta.h      # 服务元数据
├── ServiceCatalog.h   # 服务目录（内存索引）
├── HealthChecker.h    # 健康检查器
├── RegistryServer.h   # 注册中心服务器
└── RegistryClient.h   # 客户端 SDK
```

### 核心实现

#### 8.1 服务标识

```cpp
struct ServiceKey {
    std::string namespace_;   // 命名空间
    std::string serviceName;  // 服务名
    std::string version;      // 版本号

    std::string key() const {
        return namespace_ + ":" + serviceName + ":" + version;
    }
};

struct InstanceMeta {
    std::string instanceId;
    std::string host;
    int port;
    int weight;
    int64_t lastHeartbeatMs;
    int ttlSeconds;
    std::string status;  // UP, DOWN
};
```

#### 8.2 健康检查机制

```cpp
class HealthChecker {
    void checkLoop() {
        while (running_) {
            checkOnce();  // 标记过期实例为 DOWN
            cv_.wait_for(lock, std::chrono::milliseconds(checkIntervalMs_));
        }
    }
};
```

#### 8.3 服务发现流程

```
服务提供者                    注册中心                    服务消费者
    │                          │                          │
    │──── 注册实例 ────────────▶│                          │
    │                          │                          │
    │──── 心跳 ───────────────▶│                          │
    │                          │                          │
    │                          │◀──── 发现服务 ───────────│
    │                          │──── 返回实例列表 ────────▶│
    │                          │                          │
    │     (TTL 过期)           │                          │
    │◀────────────────────── 标记 DOWN                   │
```

**面试亮点：**
- 分布式服务发现
- TTL 心跳机制
- 自动健康检查
- 与负载均衡集成

---

## 9. WebSocket 模块

### 文件结构
```
src/websocket/
├── WebSocketFrame.h     # 帧编解码
├── WsSession.h          # 会话管理
└── WebSocketServer.h    # WebSocket 服务器
```

### 核心实现

#### 9.1 帧格式（RFC 6455）

```
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-------+-+-------------+-------------------------------+
 |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
 |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
 |N|V|V|V|       |S|             |   (if payload len==126/127)   |
 | |1|2|3|       |K|             |                               |
 +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
 |     Extended payload length continued, if payload len == 127  |
 + - - - - - - - - - - - - - - - +-------------------------------+
 |                               |Masking-key, if MASK set to 1  |
 +-------------------------------+-------------------------------+
 | Masking-key (continued)       |          Payload Data         |
 +-------------------------------- - - - - - - - - - - - - - - - +
```

#### 9.2 握手协议

```cpp
// 客户端请求
GET /chat HTTP/1.1
Host: server.example.com
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13

// 服务端响应
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=

// Accept Key 计算
acceptKey = BASE64(SHA1(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
```

#### 9.3 会话管理

```cpp
class WsSession {
    void sendText(const std::string& text);
    void sendBinary(const std::vector<uint8_t>& data);
    void ping(const std::vector<uint8_t>& data = {});
    void close(uint16_t code = 1000, const std::string& reason = "");

    // 状态管理
    enum class WsState { Connecting, Open, Closing, Closed };
};
```

**面试亮点：**
- 完整的 RFC 6455 实现
- 握手协议正确性（Accept Key 计算）
- 掩码编解码
- 控制帧处理（Ping/Pong/Close）

---

## 总结

### 项目亮点

| 模块 | 核心技术 | 面试价值 |
|------|----------|----------|
| HTTP | 状态机解析、Keep-Alive | 高 |
| RPC | Protobuf、类型安全注册 | 很高 |
| 定时器 | 时间轮 O(1) | 高 |
| 异步日志 | 双缓冲、线程安全 | 高 |
| 连接池 | 锁外创建、生命周期 | 中 |
| 负载均衡 | 策略模式、一致性哈希 | 高 |
| 服务注册中心 | TTL 心跳、服务发现 | 很高 |
| WebSocket | RFC 6455、帧编解码 | 高 |

### 技术栈

- **语言：** C++17
- **网络：** epoll、Reactor 模式
- **序列化：** JSON、Protobuf
- **并发：** 多线程、mutex、atomic
- **安全：** OpenSSL (SHA1)

---

*最后更新: 2026-03-14*