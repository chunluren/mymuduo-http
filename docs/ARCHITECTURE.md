# mymuduo-http 架构设计文档

## 一、项目概述

mymuduo-http 是一个基于 Reactor 模式的高性能网络框架，采用 C++17 实现。设计灵感来源于陈硕的 muduo 网络库，在此基础上扩展了 HTTP、RPC、WebSocket、服务注册中心等功能模块。

### 核心特性

- **Reactor 模式**：基于事件驱动的非阻塞 I/O
- **One Loop Per Thread**：每个线程一个事件循环，充分利用多核
- **模块化设计**：网络库、HTTP、RPC、WebSocket 等模块独立且可组合
- **高性能**：时间轮定时器、双缓冲日志、连接池等优化

### 架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                           应用层                                     │
├──────────────┬──────────────┬──────────────┬────────────────────────┤
│  HttpServer  │  HttpClient  │  WsServer    │  WsClient             │
│  RpcServer   │ReactorRpcClient│RegistryServer│RegistryClient        │
├──────────────┴──────────────┴──────────────┴────────────────────────┤
│                           服务层                                     │
├─────────────┬─────────────┬─────────────┬─────────────┬─────────────┤
│ ConnectionPool│ TimerQueue │ HealthChecker│ LoadBalancer│  ...       │
├─────────────┴─────────────┴─────────────┴─────────────┴─────────────┤
│                      核心网络层（服务端 + 客户端）                      │
├─────────────────────────────────────────────────────────────────────┤
│  TcpServer  │  TcpClient  │  TcpConnection (共用)  │  Connector    │
│  Acceptor   │             │                        │  (非阻塞connect)│
├─────────────────────────────────────────────────────────────────────┤
│  EventLoop (timerfd 集成) │ Channel │ Poller (epoll) │ Buffer │ Socket│
├─────────────────────────────────────────────────────────────────────┤
│                           基础设施层                                 │
├─────────────┬─────────────┬─────────────┬─────────────┬─────────────┤
│  Timestamp  │  Thread     │  Logger     │  noncopyable│  TimerId    │
└─────────────┴─────────────┴─────────────┴─────────────┴─────────────┘
```

### 服务端 vs 客户端路径

```
服务端: TcpServer → Acceptor → accept()  → TcpConnection
客户端: TcpClient → Connector → connect() → TcpConnection
                                              ↑
                           连接建立后完全相同，共用 TcpConnection

Connector 特性:
  - 非阻塞 connect: EINPROGRESS → EPOLLOUT → getsockopt(SO_ERROR)
  - 指数退避重连: 500ms → 1s → 2s → ... → 30s
  - TcpClient.enableRetry() 开启断线自动重连
```

### 定时器集成

```
EventLoop 内置 timerfd + TimerQueue (时间轮):
  loop->runAfter(delaySec, callback)   // 延迟执行
  loop->runEvery(intervalSec, callback) // 周期执行
  loop->cancel(timerId)                 // 取消定时器

timerfd_create(CLOCK_MONOTONIC) → 注册到 epoll → 周期触发 tick()
```

---

## 二、核心网络层设计

### 2.1 Reactor 模式

Reactor 模式是本项目的核心设计模式，用于处理并发的 I/O 事件。

#### 设计原理

```
┌────────────────────────────────────────────────────┐
│                    EventLoop                       │
│  ┌──────────────┐    ┌──────────────────────────┐ │
│  │   Poller     │───▶│   ChannelList            │ │
│  │  (epoll)     │    │  [Channel, Channel, ...] │ │
│  └──────────────┘    └──────────────────────────┘ │
│         │                       │                 │
│         ▼                       ▼                 │
│    poll() 等待事件       handleEvent() 分发事件   │
└────────────────────────────────────────────────────┘
```

#### 三大核心组件

| 组件 | 职责 | 设计要点 |
|------|------|----------|
| **EventLoop** | 事件循环主体 | One Loop Per Thread，wakeup 机制实现跨线程调用 |
| **Channel** | 事件分发器 | 封装 fd + 事件回调，每个 fd 对应一个 Channel |
| **Poller** | I/O 多路复用 | 抽象接口，目前实现 EPollPoller |

#### 事件循环流程

```cpp
void EventLoop::loop() {
    while (!quit_) {
        // 1. 等待事件
        activeChannels_.clear();
        pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);

        // 2. 分发事件
        for (Channel* channel : activeChannels_) {
            channel->handleEvent(pollReturnTime_);
        }

        // 3. 执行跨线程任务
        doPendingFunctors();
    }
}
```

### 2.2 One Loop Per Thread 模型

#### 设计原理

每个线程最多运行一个 EventLoop，通过线程局部存储确保：

```cpp
__thread EventLoop* t_loopInThisThread = nullptr;

EventLoop::EventLoop() {
    if (t_loopInThisThread) {
        // 当前线程已有 EventLoop，触发断言
        abort();
    }
    t_loopInThisThread = this;
}
```

#### 跨线程调用机制

通过 `eventfd` 实现 wakeup：

```cpp
void EventLoop::wakeup() {
    uint64_t one = 1;
    ::write(wakeupFd_, &one, sizeof(one));
}

void EventLoop::handleRead() {
    uint64_t one = 1;
    ::read(wakeupFd_, &one, sizeof(one));
}
```

当其他线程调用 `runInLoop()` 时：

1. 如果在 EventLoop 线程中 → 直接执行
2. 如果在其他线程中 → 加入队列，wakeup EventLoop

### 2.3 TcpServer 架构

#### mainReactor + subReactors 模型

```
                    ┌─────────────────┐
                    │   mainReactor   │
                    │   (baseLoop)    │
                    │   Acceptor      │
                    └────────┬────────┘
                             │ accept()
                             ▼
          ┌──────────────────┼──────────────────┐
          │                  │                  │
          ▼                  ▼                  ▼
    ┌──────────┐      ┌──────────┐      ┌──────────┐
    │subReactor│      │subReactor│      │subReactor│
    │(IO线程1) │      │(IO线程2) │      │(IO线程3) │
    │ TcpConn  │      │ TcpConn  │      │ TcpConn  │
    │ TcpConn  │      │ TcpConn  │      │ TcpConn  │
    └──────────┘      └──────────┘      └──────────┘
```

#### 连接管理流程

```cpp
// 新连接到达
void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr) {
    // 1. 轮询选择一个 subLoop
    EventLoop* ioLoop = threadPool_->getNextLoop();

    // 2. 创建连接对象
    std::string connName = name_ + "-" + std::to_string(nextConnId_);
    TcpConnectionPtr conn = std::make_shared<TcpConnection>(
        ioLoop, connName, sockfd, localAddr, peerAddr);

    // 3. 设置回调
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);

    // 4. 在 subLoop 中初始化连接
    ioLoop->runInLoop([conn]() { conn->connectEstablished(); });
}
```

### 2.4 Buffer 设计

#### 缓冲区结构

```
+-------------------+------------------+------------------+
| prependable bytes |  readable bytes  | writable bytes   |
|    (已读区域)      |    (有效数据)     |   (可写区域)     |
+-------------------+------------------+------------------+
|                   |                  |                  |
0      <=      readerIndex   <=   writerIndex    <=     size
```

#### 设计要点

1. **前置空间**：用于高效添加协议头
2. **动态扩容**：自动扩容，无需手动管理
3. **readv 优化**：配合栈上缓冲区，减少系统调用

```cpp
ssize_t Buffer::readFd(int fd, int* savedErrno) {
    // 栈上辅助缓冲区，避免频繁扩容
    char extrabuf[65536];
    struct iovec vec[2];
    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len = writableBytes();
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    ssize_t n = readv(fd, vec, 2);
    // ...
}
```

---

## 三、定时器模块设计

### 3.1 时间轮算法

传统定时器（红黑树、最小堆）的插入/删除复杂度为 O(log n)。时间轮通过哈希思想，实现 O(1) 的插入和删除。

#### 数据结构

```
时间轮 (buckets=8, tickMs=1000ms)
覆盖时间范围：8 * 1000ms = 8 秒

        桶0 ──▶ [Timer1] → [Timer2] → ...
        桶1 ──▶ [Timer3] → ...
        桶2 ──▶ ...
        桶3 ──▶ [Timer4] → ...
        桶4 ──▶ ...
        桶5 ──▶ ...
        桶6 ──▶ ...
        桶7 ──▶ ...
          ▲
          │
       当前指针
```

#### 核心操作

```cpp
// 添加定时器 - O(1)
int64_t addTimer(callback, delayMs, intervalMs) {
    size_t ticks = delayMs / tickMs_;
    size_t bucket = (currentBucket_ + ticks) % buckets_;
    wheel_[bucket].push_back(timer);
}

// 推进时间轮
void tick() {
    // 处理当前桶中的所有定时器
    for (auto& timer : wheel_[currentBucket_]) {
        if (timer.expired()) {
            timer.run();
            // 周期性定时器重新计算位置
        }
    }
    currentBucket_ = (currentBucket_ + 1) % buckets_;
}
```

#### 局限性与改进

**局限**：最大延迟时间受 `buckets * tickMs` 限制

**改进方向**：
- 多级时间轮（类似时钟的秒、分、时）
- 哈希表辅助快速删除

---

## 四、连接池模块设计

### 4.1 设计目标

1. **复用连接**：减少创建/销毁开销
2. **控制资源**：限制最大连接数，防止资源耗尽
3. **自动维护**：健康检查，清理空闲连接

### 4.2 架构设计

```
┌─────────────────────────────────────────────────────┐
│                  ConnectionPool                      │
│                                                      │
│   ┌─────────┐     ┌─────────────────────────────┐  │
│   │  pool_  │     │       Connection             │  │
│   │ (queue) │────▶│  fd, host, port, lastUsed   │  │
│   └─────────┘     └─────────────────────────────┘  │
│                                                      │
│   minSize_ = 5        最小连接数                     │
│   maxSize_ = 20       最大连接数                     │
│   totalCreated_       已创建总数                     │
│                                                      │
│   mutex_ + cv_        线程安全同步                   │
└─────────────────────────────────────────────────────┘
```

### 4.3 获取连接流程

```cpp
Connection::Ptr acquire(int timeoutMs) {
    // 1. 等待可用连接或池未满
    cv_.wait_for(lock, timeout, [] {
        return !pool_.empty() || totalCreated_ < maxSize_;
    });

    // 2. 优先复用现有连接
    if (!pool_.empty()) {
        auto conn = pool_.front();
        pool_.pop();
        return conn;
    }

    // 3. 创建新连接
    if (totalCreated_ < maxSize_) {
        totalCreated_++;
        return createConnection();
    }
}
```

### 4.4 健康检查

定期清理超过 60 秒未使用的空闲连接：

```cpp
void healthCheck() {
    for (auto& conn : pool_) {
        if (now - conn->lastUsed() > 60 && valid.size() >= minSize_) {
            // 清理空闲连接
        }
    }
}
```

---

## 五、HTTP 模块设计

### 5.1 设计原则

- **简洁 API**：通过路由注册快速构建服务
- **中间件机制**：支持日志、认证等横切关注点
- **安全防护**：请求大小限制、路径遍历防护

### 5.2 请求处理流程

```
┌──────────────────────────────────────────────────────────┐
│                      HttpServer                          │
│                                                          │
│  1. 接收数据 ──▶ 2. 解析请求 ──▶ 3. 处理请求 ──▶ 4. 响应  │
│                                                          │
│  ┌─────────┐   ┌─────────────┐   ┌───────────────┐      │
│  │ Buffer  │──▶│ HttpRequest │──▶│ Middlewares   │      │
│  └─────────┘   └─────────────┘   └───────┬───────┘      │
│                                          │              │
│                                          ▼              │
│                                  ┌───────────────┐      │
│                                  │ Route Match   │      │
│                                  └───────┬───────┘      │
│                                          │              │
│                         ┌────────────────┼────────────┐ │
│                         ▼                ▼            ▼ │
│                   ┌──────────┐    ┌──────────┐  ┌─────┐│
│                   │ Handler  │    │ Static   │  │ 404 ││
│                   └──────────┘    └──────────┘  └─────┘│
└──────────────────────────────────────────────────────────┘
```

### 5.3 路由设计

支持正则表达式路由：

```cpp
// 精确匹配
server.GET("/users", listUsers);

// 正则匹配
server.GET("/users/([0-9]+)", [](const HttpRequest& req, HttpResponse& resp) {
    // 匹配 /users/123
});
```

### 5.4 安全防护

#### 路径遍历防护

```cpp
void serveFile(...) {
    // 先解码，再检查
    std::string decoded = urlDecode(filename);

    // 检查危险路径
    if (decoded.find("..") != std::string::npos ||
        decoded[0] == '/' ||
        decoded.find('\\') != std::string::npos) {
        return badRequest();
    }
}
```

#### 请求大小限制

```cpp
// 最大请求体 10MB
static constexpr size_t kMaxBodySize = 10 * 1024 * 1024;

// 最大请求行 8KB
static constexpr size_t kMaxRequestLine = 8192;
```

### HttpCore 分层

```
HttpServer         HttpsServer
     ↓                  ↓
  TcpServer        SSL + BIO
     ↓                  ↓
     └──── HttpCore ────┘
               ↓
        Router + Middleware + Gzip + Static Files
```

HttpServer 和 HttpsServer 只负责传输层，HTTP 协议处理统一委托给 HttpCore。

---

## 六、RPC 模块设计

### 6.1 支持两种协议

| 协议 | 特点 | 适用场景 |
|------|------|----------|
| **JSON-RPC** | 人类可读，调试方便 | 开发测试、简单服务 |
| **Protobuf-RPC** | 二进制高效，跨语言 | 生产环境、高性能场景 |

### 6.2 JSON-RPC 实现

```cpp
// 请求格式
{
    "jsonrpc": "2.0",
    "method": "add",
    "params": [1, 2],
    "id": 1
}

// 响应格式
{
    "jsonrpc": "2.0",
    "result": 3,
    "id": 1
}
```

### 6.3 Protobuf-RPC 实现

#### 协议帧格式

```
┌────────────┬────────────────────────────┐
│  4 bytes   │       N bytes              │
│  长度(len) │   RpcRequest/RpcResponse   │
└────────────┴────────────────────────────┘
```

#### 方法注册

```cpp
// 类型安全的注册
server.registerMethod<AddRequest, AddResponse>(
    "Calculator", "Add",
    [](const AddRequest& req, AddResponse& resp) {
        resp.set_result(req.a() + req.b());
    });
```

---

## 七、WebSocket 模块设计

### 7.1 帧格式

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

### 7.2 握手过程

```cpp
// 客户端请求
GET /chat HTTP/1.1
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==

// 服务器响应
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

#### Accept Key 计算

```cpp
std::string computeAcceptKey(const std::string& clientKey) {
    const std::string GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = clientKey + GUID;

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(combined, hash);

    return base64Encode(hash);
}
```

---

## 八、服务注册中心设计

### 8.1 核心概念

```
┌─────────────────────────────────────────────────────┐
│                 ServiceCatalog                       │
│                                                      │
│  ServiceKey: (namespace, serviceName, version)      │
│                                                      │
│  ┌────────────────────────────────────────────────┐ │
│  │  ServiceKey ──▶ [Instance1, Instance2, ...]    │ │
│  └────────────────────────────────────────────────┘ │
│                                                      │
│  InstanceMeta:                                       │
│    - instanceId, host, port                         │
│    - weight, metadata                               │
│    - lastHeartbeat (用于健康判断)                    │
└─────────────────────────────────────────────────────┘
```

### 8.2 API 设计

| 操作 | 方法 | 路径 |
|------|------|------|
| 注册服务 | POST | `/api/v1/registry/register` |
| 注销服务 | POST | `/api/v1/registry/deregister` |
| 心跳 | POST | `/api/v1/registry/heartbeat` |
| 发现服务 | GET | `/api/v1/registry/discover` |
| 服务列表 | GET | `/api/v1/registry/services` |

### 8.3 健康检查

```cpp
void HealthChecker::check() {
    for (auto& instance : catalog_->getAllInstances()) {
        if (now - instance->lastHeartbeat > timeout) {
            catalog_->markUnhealthy(instance);
        }
    }
}
```

---

## 九、线程模型详解

### 9.1 线程职责分工

```
┌─────────────────────────────────────────────────────────────────────┐
│  mainLoop 线程                                                      │
│  ├── Acceptor::handleRead()       接受新连接                         │
│  ├── TcpServer::newConnection()   创建 TcpConnection 分配到 subLoop │
│  ├── TcpServer::removeConnectionInLoop()  从 connections_ 移除      │
│  └── EventLoop::doPendingFunctors()       处理跨线程任务             │
│                                                                      │
│  subLoop 线程（每个线程独立处理分配到的连接）                          │
│  ├── TcpConnection::handleRead()   读数据 → messageCallback         │
│  ├── TcpConnection::handleWrite()  写数据 → writeCompleteCallback   │
│  ├── TcpConnection::handleClose()  关闭 → 通知 mainLoop 移除        │
│  ├── TcpConnection::sendInLoop()   实际发送数据                      │
│  └── EventLoop::doPendingFunctors()  执行跨线程转发的任务             │
│                                                                      │
│  AsyncLogger 后台线程                                                │
│  └── writerLoop()                  交换缓冲区 → 写磁盘               │
│                                                                      │
│  HealthChecker 后台线程                                              │
│  └── checkLoop()                   定时检查实例心跳是否过期           │
└─────────────────────────────────────────────────────────────────────┘
```

### 9.2 连接的完整生命周期（跨线程协作）

```
                     mainLoop 线程                        subLoop 线程
                     ─────────────                        ────────────
1. 新连接到达         Acceptor::handleRead()
                            │
2. 创建连接          newConnection()
                     connections_[name] = conn
                            │
3. 分配到 subLoop     ioLoop->runInLoop(connectEstablished)
                            │ ─────────────────────────▶ connectEstablished()
                                                         channel_->tie(shared_from_this())
                                                         channel_->enableReading()
                                                              │
4. 处理数据                                              handleRead()
                                                         messageCallback_(...)
                                                              │
5. 发送响应                                              sendInLoop()
                            (若其他线程 send，                │
                             runInLoop 转发) ──────────▶ sendInLoop()
                                                              │
6. 关闭连接                                              handleClose()
                                                         closeCallback_(conn)
                            │ ◀─────────────────────────     │
7. 移除连接          removeConnectionInLoop()
                     connections_.erase(name)
                            │
8. 销毁连接           ioLoop->queueInLoop(connectDestroyed)
                            │ ─────────────────────────▶ connectDestroyed()
                                                         channel_->disableAll()
                                                         (conn shared_ptr 引用计数归零后析构)
```

### 9.3 跨线程任务队列的关键设计

**swap 替代 copy**（`EventLoop.cc:345-363`）：

```cpp
void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);  // O(1) 交换，最小化锁持有时间
    }
    for (const Functor& functor : functors) {
        functor();
    }
    callingPendingFunctors_ = false;
}
```

**为什么需要 `callingPendingFunctors_` 标志**（`EventLoop.cc:247-262`）：

```cpp
void EventLoop::queueInLoop(Functor cb) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(cb);
    }
    // 关键判断：正在执行回调期间又添加了新回调，需要再次唤醒
    // 否则新回调要等到下一次 poll 超时才能执行
    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();
    }
}
```

---

## 十、内存管理策略

### 10.1 TcpConnection 的 shared_ptr 生命周期

TcpConnection 是整个框架中生命周期最复杂的对象，使用 `shared_ptr` 管理：

```
                  shared_ptr 引用关系
                  ───────────────────
TcpServer::connections_  ──────▶ TcpConnection (引用计数 ≥ 1)
                                       │
Channel::tie()  ─── weak_ptr ────────▶ │ (不增加引用计数)
                                       │
回调 lambda 捕获 ───── shared_ptr ───▶ │ (回调期间保活)
```

**关键设计点**：

1. **Channel::tie() 使用 weak_ptr**（`TcpConnection.cc:80`）：
   ```cpp
   channel_->tie(shared_from_this());
   ```
   Channel 不直接持有 TcpConnection 的 shared_ptr，避免循环引用。`handleEvent()` 时先 `lock()` 确认对象存活。

2. **send() 按值捕获 shared_ptr**（`TcpConnection.cc:175`）：
   ```cpp
   loop_->runInLoop([self = shared_from_this(), msg = message]() {
       self->sendInLoop(msg.c_str(), msg.size());
   });
   ```
   lambda 捕获 `shared_from_this()` 确保跨线程执行时 TcpConnection 不会被提前析构。

3. **removeConnection 的两阶段设计**：
   - 阶段一（mainLoop）：从 `connections_` map 中移除，但 conn 的 shared_ptr 被 `bind` 到 `connectDestroyed` 的回调中，引用计数不归零
   - 阶段二（subLoop）：`connectDestroyed()` 执行完毕，回调中的 shared_ptr 销毁，引用计数归零，TcpConnection 析构

### 10.2 Buffer 的空间管理策略

**空间不足时的决策逻辑**（`Buffer.h:82-100`）：

```
                          ensureWritableBytes(len)
                                  │
                    ┌─────────────┴─────────────┐
                    │                           │
            writableBytes() >= len     writableBytes() < len
            (空间足够，直接写入)          │
                                    ┌───┴───────────────────┐
                                    │                       │
                            writable + prepend          writable + prepend
                             >= len + 8                   < len + 8
                            (可以移动腾出空间)          (必须扩容)
                                    │                       │
                              move data to front      resize(writerIndex_ + len)
                              (O(n) 数据复制)         (可能触发内存重新分配)
```

**readFd 的栈缓冲区优化**（`Buffer.h`）：

```cpp
ssize_t Buffer::readFd(int fd, int* savedErrno) {
    char extrabuf[65536];                    // 栈上 64KB 临时缓冲
    struct iovec vec[2];
    vec[0] = { beginWrite(), writableBytes() };  // 先写 Buffer
    vec[1] = { extrabuf, sizeof(extrabuf) };     // 溢出部分写栈缓冲
    ssize_t n = readv(fd, vec, 2);               // 一次系统调用读完
    if (n > writableBytes()) {
        writerIndex_ = buffer_.size();
        append(extrabuf, n - writableBytes());   // 把栈上数据追加到 Buffer
    }
}
```

**设计意图**：
- 避免预分配大 Buffer 浪费内存
- 一次 `readv` 系统调用最多读 `writableBytes() + 64KB`
- 大多数请求在 Buffer 初始空间（1024 字节）内完成，无需扩容

### 10.3 AsyncLogger 的双缓冲零拷贝交换

```
  前端线程 (写日志)                  后端线程 (刷磁盘)
  ─────────────────                  ─────────────────
  currentBuffer_ → bufferA_          flushBuffer_ → bufferB_
       │                                  │
       │ log(entry)                       │ 等待 condition_variable
       │ bufferA_.push_back(entry)        │
       │                                  │
       │ 缓冲区满 / 定时 100ms           │
       │ ──── notify_one() ────────────▶ │
       │                                  │
       │    ┌── mutex 加锁 ──┐           │
       │    │ swap(current,  │           │
       │    │      flush)    │           │ 交换指针 (O(1))
       │    └── mutex 解锁 ──┘           │
       │                                  │
  currentBuffer_ → bufferB_          flushBuffer_ → bufferA_
       │                                  │
       │ 继续写 bufferB_                 │ 将 bufferA_ 写入文件（无锁）
       │ (无阻塞)                        │ bufferA_.clear()
```

---

## 十一、HTTP 请求完整数据流

### 11.1 从 TCP 数据到 HTTP 响应

```
网络数据 ──▶ epoll_wait 返回
                │
                ▼
        Channel::handleEvent()
                │
                ▼
        TcpConnection::handleRead()
        inputBuffer_.readFd(fd)         // 读取原始字节到 Buffer
                │
                ▼
        HttpServer::onMessage()         // messageCallback
                │
                ▼
      ┌── while (buf->readableBytes() > 0) ──┐
      │                                       │
      │   parseRequest(buf, request)          │
      │        │                               │
      │   ┌────┴─────────────────┐            │
      │   │ Incomplete → return   │  等待更多数据│
      │   │ Error → 400 + close   │            │
      │   │ Complete → 继续       │            │
      │   └──────────────────────┘            │
      │        │                               │
      │   handleRequest(request, response)     │
      │   ├── 执行中间件链                     │
      │   ├── 匹配路由                         │
      │   └── 执行 Handler                    │
      │        │                               │
      │   conn->send(response.toString())      │
      │        │                               │
      │   keepAlive? ──┬── yes → 继续循环      │
      │                └── no → shutdown()     │
      └───────────────────────────────────────┘
```

### 11.2 HTTP 解析的 Peek-Parse-Consume 模式

```cpp
// 第一步：Peek（只看不消费）
const char* data = buf->peek();
size_t len = buf->readableBytes();

// 第二步：查找请求头边界
const char* headerEnd = memmem(data, len, "\r\n\r\n", 4);
if (!headerEnd) return ParseResult::Incomplete;

// 第三步：解析头部，计算请求体长度
size_t contentLen = request.contentLength();
if (len < headerLen + contentLen) return ParseResult::Incomplete;

// 第四步：确认完整后才消费数据
buf->retrieve(headerLen);
request.body.assign(buf->peek(), contentLen);
buf->retrieve(contentLen);
return ParseResult::Complete;
```

**为什么不直接消费**：TCP 是字节流，数据可能不完整。先 peek 判断完整性，避免消费了不完整的请求头后无法回退。

---

## 十二、性能优化策略（续）

### 9.1 减少系统调用

- **Buffer::readFd**：使用 `readv` + 栈上缓冲区
- **批量处理**：事件循环中批量处理活跃 Channel

### 9.2 减少锁竞争

- **One Loop Per Thread**：每个线程独立处理，无锁
- **双缓冲日志**：前端写 buffer A，后端刷 buffer B
- **原子操作**：使用 `std::atomic` 替代锁

### 9.3 内存优化

- **对象池**：复用 TcpConnection 等对象
- **预分配**：Buffer 初始大小合理设置
- **RAII**：自动管理资源生命周期

---

## 十三、线程安全设计

### 13.1 设计原则

1. **不可变对象**：如 Timestamp，天生线程安全
2. **线程局部**：如 CurrentThread::tid()，无竞争
3. **原子操作**：如 EventLoop::quit_
4. **互斥锁**：保护共享数据结构
5. **跨线程调用**：通过 `runInLoop` 串行化

### 13.2 典型模式

```cpp
// 跨线程安全调用
void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();  // 直接执行
    } else {
        queueInLoop(cb);  // 队列 + wakeup
    }
}
```

---

## 十四、扩展性设计

### 14.1 接口抽象

```cpp
// Poller 抽象，可扩展其他 I/O 多路复用
class Poller {
    virtual Timestamp poll(int timeoutMs, ChannelList* activeChannels) = 0;
};

// 负载均衡策略抽象
class LoadBalanceStrategy {
    virtual BackendServerPtr select(const std::vector<BackendServerPtr>&) = 0;
};
```

### 14.2 插件机制

- **中间件**：`HttpServer::use(middleware)`
- **路由处理器**：`server.GET(path, handler)`
- **RPC 方法**：`server.registerMethod(...)`

---

## 十五、总结

mymuduo-http 是一个模块化、高性能、易扩展的网络框架：

| 特性 | 实现方式 |
|------|----------|
| 高并发 | Reactor + One Loop Per Thread |
| 高性能 | 时间轮、连接池、零拷贝 |
| 易使用 | 简洁 API、中间件、路由 |
| 可扩展 | 抽象接口、插件机制 |
| 线程安全 | 原子操作、互斥锁、跨线程调用 |

适用于 Web 服务、RPC 框架、实时通信等场景。

---

## 扩展模块架构

### 中间件管道
HttpServer 请求处理流程:
Request → [RateLimit] → [CORS] → [Metrics] → Route Match → Handler → [Gzip] → Response

### 连接池子系统
MySQLPool / RedisPool: 预创建 + 按需创建 + 空闲回收
acquire(timeout) → ping检活 → 使用 → release归还

### 安全层
- HTTPS: Memory BIO + OpenSSL (TLS 1.2+)
- JWT: HMAC-SHA256 token
- RateLimiter: 每 IP 限流
- 路由线程安全: std::shared_mutex

### 可靠性
- CircuitBreaker: 三态熔断保护下游
- Graceful Shutdown: 停止接受 → 等待完成 → 关闭
- ObjectPool: 减少 new/delete 开销