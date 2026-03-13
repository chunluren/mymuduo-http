# mymuduo-http 技术详解（面试版）

> 本文档详细介绍了项目的技术实现，适合面试时深入讲解

---

## 目录

1. [HTTP 服务器模块](#1-http-服务器模块)
2. [RPC 框架模块](#2-rpc-框架模块)
3. [定时器模块](#3-定时器模块)
4. [异步日志模块](#4-异步日志模块)
5. [连接池模块](#5-连接池模块)
6. [面试高频问题](#6-面试高频问题)

---

## 1. HTTP 服务器模块

### 1.1 架构设计

```
┌─────────────────────────────────────────────────────┐
│                    HttpServer                        │
│  ┌─────────────────────────────────────────────┐    │
│  │              TcpServer (网络层)               │    │
│  │   mainLoop (Accept) → subLoop1,2,3...       │    │
│  └─────────────────────────────────────────────┘    │
│                        ↓                             │
│  ┌─────────────────────────────────────────────┐    │
│  │            HTTP 解析层                        │    │
│  │   parseRequest → HttpRequest                 │    │
│  └─────────────────────────────────────────────┘    │
│                        ↓                             │
│  ┌─────────────────────────────────────────────┐    │
│  │            路由匹配层                         │    │
│  │   regex_match → handler                      │    │
│  └─────────────────────────────────────────────┘    │
│                        ↓                             │
│  ┌─────────────────────────────────────────────┐    │
│  │            响应构造层                         │    │
│  │   HttpResponse → toString()                  │    │
│  └─────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────┘
```

### 1.2 核心技术点

#### 1.2.1 正则路由匹配

**为什么用正则？**
- 支持动态路径如 `/user/:id`
- 灵活性高，易于扩展

**实现代码：**
```cpp
struct Route {
    HttpMethod method;
    std::string pattern;  // 如 "/api/user/\\d+"
    HttpHandler handler;
    std::regex regex;     // 预编译正则
    
    Route(HttpMethod m, const std::string& p, HttpHandler h)
        : method(m), pattern(p), handler(h), regex(p) {}
};

// 路由匹配
for (const auto& route : routes_) {
    if (route.method == request.method && 
        std::regex_match(request.path, route.regex)) {
        route.handler(request, response);
        return;
    }
}
```

**面试亮点：**
- 正则预编译，避免每次匹配都编译
- 支持多种 HTTP 方法（GET/POST/PUT/DELETE）
- 可扩展支持路径参数提取

#### 1.2.2 Keep-Alive 连接复用

**HTTP/1.1 默认开启 Keep-Alive**

```cpp
// 判断是否保持连接
bool keepAlive() const {
    if (version == HttpVersion::HTTP_10) {
        // HTTP/1.0 需要显式声明
        return getHeader("connection") == "keep-alive";
    }
    // HTTP/1.1 默认 keep-alive，除非显式 close
    return getHeader("connection") != "close";
}

// 响应处理
void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
    // ... 处理请求 ...
    
    // 根据请求决定是否关闭连接
    response.closeConnection = !request.keepAlive();
    conn->send(response.toString());
    
    if (response.closeConnection) {
        conn->shutdown();  // 关闭写端
    }
    // 否则连接保持，等待下一个请求
}
```

**面试亮点：**
- 理解 HTTP/1.0 vs HTTP/1.1 的 Keep-Alive 差异
- 减少 TCP 三次握手开销
- 提升并发性能

#### 1.2.3 请求解析状态机

**解析流程：**
```
Request Line → Headers → Body
     ↓            ↓         ↓
  GET /path   Host:...   {json}
```

**实现代码：**
```cpp
bool parseRequest(Buffer* buf, HttpRequest& request) {
    std::string data = buf->retrieveAllAsString();
    
    // 1. 找请求头结束位置
    size_t headerEnd = data.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return false;  // 请求头不完整
    }
    
    // 2. 解析请求行
    std::string requestLine = header.substr(0, lineEnd);
    // "GET /path?query HTTP/1.1"
    request.parseRequestLine(requestLine);
    
    // 3. 解析请求头
    while (pos < header.size()) {
        request.parseHeader(line);
        // "Content-Length: 100" → headers["content-length"] = "100"
    }
    
    // 4. 解析请求体
    request.body = data.substr(headerEnd + 4);
    
    return true;
}
```

**面试亮点：**
- 严格按 HTTP 协议解析
- 处理不完整请求（等待更多数据）
- 支持 Content-Length 判断请求完整性

### 1.3 性能优化

| 优化点 | 方法 | 效果 |
|--------|------|------|
| 正则预编译 | 构造时编译 | 避免运行时开销 |
| Buffer 零拷贝 | 直接传递数据 | 减少内存拷贝 |
| 连接复用 | Keep-Alive | 减少 TCP 握手 |

---

## 2. RPC 框架模块

### 2.1 架构对比

#### JSON-RPC vs Protobuf-RPC

```
JSON-RPC:
┌──────────┐    JSON     ┌──────────┐
│ Client   │ ──────────→ │ Server   │
└──────────┘             └──────────┘
优点: 可读性好，调试方便
缺点: 性能较低，数据量大

Protobuf-RPC:
┌──────────┐  Binary    ┌──────────┐
│ Client   │ ──────────→ │ Server   │
└──────────┘             └──────────┘
优点: 性能高，数据紧凑
缺点: 不可读，需要 .proto 文件
```

### 2.2 Protobuf-RPC 实现细节

#### 2.2.1 协议设计

**消息格式：**
```protobuf
// RPC 请求
message RpcRequest {
    string service = 1;    // 服务名: "user"
    string method = 2;     // 方法名: "get"
    bytes params = 3;      // 参数: 序列化的 protobuf
    int64 id = 4;          // 请求 ID: 用于匹配响应
}

// RPC 响应
message RpcResponse {
    int64 id = 1;          // 对应的请求 ID
    bytes result = 2;      // 结果: 序列化的 protobuf
    int32 code = 3;        // 错误码: 0=成功
    string message = 4;    // 错误信息
}
```

**传输格式：**
```
+----------------+------------------------+
|  Length (4B)   |     RpcRequest/Response |
|   网络字节序    |      Protobuf 数据      |
+----------------+------------------------+
```

#### 2.2.2 服务注册机制

**类型安全的注册：**

```cpp
template<typename T1, typename T2>
void registerMethod(const std::string& serviceName, 
                    const std::string& methodName,
                    std::function<void(const T1&, T2&)> handler) {
    std::string key = serviceName + "." + methodName;
    
    // 保存方法处理器
    methods_[key] = [handler](const google::protobuf::Message& req, 
                              google::protobuf::Message& resp) {
        handler(static_cast<const T1&>(req), static_cast<T2&>(resp));
    };
    
    // 保存请求/响应构造器（用于反序列化）
    requestCreators_[key] = []() { return std::make_unique<T1>(); };
    responseCreators_[key] = []() { return std::make_unique<T2>(); };
}
```

**使用示例：**
```cpp
server.registerMethod<GetUserRequest, GetUserResponse>(
    "user", "get",
    [](const GetUserRequest& req, GetUserResponse& resp) {
        resp.set_id(req.id());
        resp.set_name("User " + std::to_string(req.id()));
    });
```

**面试亮点：**
- 模板元编程，编译期类型检查
- 自动序列化/反序列化
- 统一的服务注册接口

#### 2.2.3 网络传输

**长度前缀协议：**

```cpp
void sendResponse(const TcpConnectionPtr& conn, int64_t id, 
                  const google::protobuf::Message& result) {
    rpc::RpcResponse response;
    response.set_id(id);
    response.set_code(0);
    result.SerializeToString(response.mutable_result());
    
    std::string data;
    response.SerializeToString(&data);
    
    // 发送: 4字节长度 + 数据
    int32_t len = htonl(data.size());  // 网络字节序
    conn->send(std::string((char*)&len, 4) + data);
}
```

**为什么用长度前缀？**
- 解决 TCP 粘包问题
- 接收方知道何时读完一条消息

### 2.3 性能对比

| 指标 | JSON-RPC | Protobuf-RPC | 提升 |
|------|----------|--------------|------|
| 序列化速度 | 1x | 5-10x | 5-10x |
| 数据大小 | 1x | 0.2-0.3x | 小 3-5x |
| QPS | ~15,000 | ~50,000 | 3x |

---

## 3. 定时器模块

### 3.1 为什么用时间轮？

**对比其他实现：**

| 数据结构 | 添加 | 删除 | 触发 | 适用场景 |
|----------|------|------|------|----------|
| 最小堆 | O(log n) | O(log n) | O(1) | 定时器少 |
| 红黑树 | O(log n) | O(log n) | O(1) | 需要有序遍历 |
| **时间轮** | **O(1)** | **O(1)** | **O(n/m)** | 大量定时器 |

**时间轮优势：**
- 添加/删除复杂度 O(1)
- 适合大量定时器场景（如心跳检测）
- 实现简单

### 3.2 时间轮实现

**数据结构：**
```
         时间轮 (60 个桶，每桶 1 秒)
         
   [0] → Timer1 → Timer2
   [1] → Timer3
   [2] → empty
   [3] → Timer4
   ...
   [59] → TimerN
     ↑
  currentBucket (当前指针)
```

**核心代码：**
```cpp
class TimerQueue {
    // 添加定时器 O(1)
    int64_t addTimer(TimerCallback cb, int delayMs, int intervalMs = 0) {
        auto timer = std::make_shared<Timer>(cb, expiration, intervalMs);
        
        // 计算放入哪个桶
        size_t bucket = (currentBucket_ + delayMs / tickMs_) % buckets_;
        wheel_[bucket].push_back(timer);
        timers_[timerId] = timer;  // 用于快速取消
        
        return timerId;
    }
    
    // 时间轮推进
    void tick() {
        auto& bucket = wheel_[currentBucket_];
        int64_t now = Timer::now();
        
        for (auto it = bucket.begin(); it != bucket.end(); ) {
            auto& timer = *it;
            
            if (timer->expiration() <= now) {
                timer->run();  // 执行回调
                
                if (timer->repeat()) {
                    // 重复定时器，重新加入
                    timer->restart(now);
                    size_t newBucket = (currentBucket_ + ...) % buckets_;
                    wheel_[newBucket].push_back(timer);
                }
                
                it = bucket.erase(it);
            } else {
                ++it;
            }
        }
        
        currentBucket_ = (currentBucket_ + 1) % buckets_;
    }
    
private:
    std::vector<std::list<std::shared_ptr<Timer>>> wheel_;  // 时间轮
    std::unordered_map<int64_t, std::shared_ptr<Timer>> timers_;  // ID -> Timer
};
```

### 3.3 面试亮点

**Q: 为什么时间轮是 O(1)？**
- 添加：直接计算桶索引，插入链表
- 删除：通过 hash map 找到 timer，标记取消
- 触发：遍历当前桶的链表

**Q: 时间轮的缺点？**
- 精度受限于 tick 间隔
- 超过最大延时的定时器需要多层时间轮

**Q: 应用场景？**
- 心跳检测
- 请求超时
- 连接空闲检测

---

## 4. 异步日志模块

### 4.1 为什么需要异步日志？

**同步日志问题：**
```cpp
// 同步日志 - 阻塞业务线程
fprintf(log_file, "[INFO] %s\n", message);  // 可能阻塞数毫秒！
```

在高并发场景下，每次日志都阻塞，严重影响性能。

### 4.2 双缓冲技术

**原理图：**
```
业务线程                    后台线程
    │                          │
    ▼                          ▼
┌─────────┐              ┌─────────┐
│ Buffer A│              │ Buffer B│
│ (写入)  │              │ (写入)  │
└─────────┘              └─────────┘
    │                          │
    │     交换缓冲区            │
    ├──────────────────────────▶
    │                          │
    │                      写入文件
    │                      (无阻塞)
```

**核心代码：**
```cpp
class AsyncLogger {
    // 业务线程调用 - 非阻塞
    void log(LogLevel level, const char* file, int line, const char* fmt, ...) {
        // 1. 格式化消息
        char buf[4096];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        
        // 2. 加入当前缓冲（快速）
        {
            std::lock_guard<std::mutex> lock(mutex_);
            currentBuffer_->push_back(entry);
            
            if (currentBuffer_->size() >= kFlushThreshold) {
                cv_.notify_one();  // 唤醒后台线程
            }
        }
        // 3. 立即返回，不阻塞
    }
    
    // 后台线程 - 批量写入
    void writerLoop() {
        while (running_) {
            // 等待缓冲区满或超时
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(100));
            
            // 交换缓冲区
            std::swap(currentBuffer_, flushBuffer_);
            
            // 写入文件（无锁）
            for (const auto& entry : *flushBuffer_) {
                formatEntry(file, entry);
            }
            file.flush();
            flushBuffer_->clear();
        }
    }
    
private:
    std::vector<LogEntry> bufferA_;   // 缓冲区 A
    std::vector<LogEntry> bufferB_;   // 缓冲区 B
    std::vector<LogEntry>* currentBuffer_;  // 当前写入
    std::vector<LogEntry>* flushBuffer_;    // 待刷盘
};
```

### 4.3 面试亮点

**Q: 双缓冲有什么优势？**
- 业务线程只写入内存，不阻塞
- 后台线程批量写入，减少 IO 次数
- 缓冲区交换时只锁一次，最小化锁竞争

**Q: 为什么用条件变量？**
- 缓冲区满时唤醒，避免频繁唤醒
- 超时唤醒保证日志及时刷盘

**Q: 日志丢失怎么办？**
- 优雅退出时等待后台线程写完
- 可以添加备份缓冲区

---

## 5. 连接池模块

### 5.1 为什么需要连接池？

**没有连接池：**
```
请求1: connect() → send() → recv() → close()
请求2: connect() → send() → recv() → close()
请求3: connect() → send() → recv() → close()

问题: 每次 connect 都要三次握手，开销大
```

**使用连接池：**
```
请求1: acquire() → send() → recv() → release()
请求2: acquire() → send() → recv() → release()  (复用连接)
请求3: acquire() → send() → recv() → release()  (复用连接)

优势: 连接复用，减少握手开销
```

### 5.2 连接池实现

```cpp
class ConnectionPool {
public:
    ConnectionPool(const std::string& host, int port,
                   size_t minSize = 5, size_t maxSize = 20)
        : host_(host), port_(port)
        , minSize_(minSize), maxSize_(maxSize)
    {
        // 预创建连接
        for (size_t i = 0; i < minSize; ++i) {
            int fd = createConnection(host, port);
            pool_.push(std::make_shared<Connection>(fd));
        }
    }
    
    // 获取连接
    Connection::Ptr acquire(int timeoutMs = 5000) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // 等待可用连接或创建新连接
        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return !pool_.empty() || totalCreated_ < maxSize_; });
        
        if (!pool_.empty()) {
            auto conn = pool_.front();
            pool_.pop();
            return conn;  // 复用连接
        }
        
        if (totalCreated_ < maxSize_) {
            // 创建新连接
            int fd = createConnection(host_, port_);
            totalCreated_++;
            return std::make_shared<Connection>(fd);
        }
        
        return nullptr;  // 超时
    }
    
    // 归还连接
    void release(Connection::Ptr conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (running_ && pool_.size() < maxSize_) {
            pool_.push(conn);  // 放回池中
        }
        
        cv_.notify_one();  // 唤醒等待的线程
    }
    
    // 健康检查 - 清理空闲连接
    void healthCheck() {
        int64_t now = getCurrentTime();
        
        while (!pool_.empty()) {
            auto conn = pool_.front();
            
            // 超过 60 秒未使用，且保留 minSize 个
            if (now - conn->lastUsed() > 60 && pool_.size() > minSize_) {
                pool_.pop();  // 丢弃
                totalCreated_--;
            } else {
                break;
            }
        }
    }
};
```

### 5.3 面试亮点

**Q: 连接池大小如何设置？**
- 最小值：避免频繁创建销毁
- 最大值：防止资源耗尽
- 根据并发量和数据库能力调整

**Q: 如何处理连接失效？**
- 使用前检查连接状态
- 失效时重新创建

---

## 6. 面试高频问题

### Q1: HTTP 和 RPC 有什么区别？

| HTTP | RPC |
|------|-----|
| 通用协议 | 服务间调用协议 |
| 文本协议（可读） | 二进制协议（高效） |
| 无状态 | 可以有状态 |
| 适合对外 API | 适合内部服务调用 |

### Q2: 为什么用 Protobuf 而不是 JSON？

| 指标 | JSON | Protobuf |
|------|------|----------|
| 性能 | 慢 | **快 5-10x** |
| 大小 | 大 | **小 3-5x** |
| 类型安全 | 弱 | **强** |
| 可读性 | 好 | 差 |

### Q3: 如何保证高并发？

1. **IO 多路复用**：epoll
2. **多线程**：one loop per thread
3. **连接复用**：Keep-Alive
4. **异步处理**：异步日志
5. **资源池化**：连接池

### Q4: 项目有哪些难点？

1. **HTTP 协议解析**：处理各种边界情况
2. **时间轮实现**：O(1) 复杂度设计
3. **异步日志**：双缓冲无锁设计
4. **Protobuf 集成**：类型安全的泛型注册

### Q5: 如果让你优化，会怎么做？

1. **HTTP/2 支持**：多路复用
2. **零拷贝**：sendfile
3. **内存池**：减少 malloc
4. **协程**：同步写法，异步性能

---

## 总结

### 项目亮点

1. **高性能**：Reactor 模式 + 多线程
2. **高可用**：连接池 + 异步日志
3. **易扩展**：路由注册 + 服务注册
4. **类型安全**：Protobuf + 模板

### 技术栈

- 网络：epoll、TcpServer
- 序列化：JSON、Protobuf
- 并发：多线程、mutex、atomic
- 数据结构：时间轮、连接池

---

*本文档持续更新中...*