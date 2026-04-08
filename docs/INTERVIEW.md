# mymuduo-http 面试问答手册

> 本文档整理了面试中可能被问到的问题和标准答案。

---

## 目录

### Reactor 模式相关
1. [什么是 Reactor 模式？为什么用 Reactor 而不是 Proactor？](#1-什么是-reactor-模式为什么用-reactor-而不是-proactor)
2. [One Loop Per Thread 的优势是什么？](#2-one-loop-per-thread-的优势是什么)
3. [如何实现跨线程的任务调度？](#3-如何实现跨线程的任务调度)
4. [EventLoop 如何保证线程安全？](#4-eventloop-如何保证线程安全)

### 时间轮相关
5. [时间轮和红黑树定时器的区别？](#5-时间轮和红黑树定时器的区别)
6. [时间轮如何处理超过覆盖范围的定时器？](#6-时间轮如何处理超过覆盖范围的定时器)
7. [时间轮的精度由什么决定？](#7-时间轮的精度由什么决定)

### 异步日志相关
8. [双缓冲区和循环队列的区别？](#8-双缓冲区和循环队列的区别)
9. [为什么用 swap 指针而不是数据拷贝？](#9-为什么用-swap-指针而不是数据拷贝)
10. [如何保证日志不丢失？](#10-如何保证日志不丢失)
11. [为什么无条件 swap？](#11-为什么无条件-swap)

### 负载均衡相关
12. [加权轮询和平滑加权轮询的区别？](#12-加权轮询和平滑加权轮询的区别)
13. [一致性哈希为什么需要虚拟节点？](#13-一致性哈希为什么需要虚拟节点)
14. [最小连接数策略的实现有什么坑？](#14-最小连接数策略的实现有什么坑)

### 线程安全相关
15. [RoundRobinStrategy 的 index_ 为什么用 atomic？](#15-roundrobinstrategy-的-index_-为什么用-atomic)
16. [BackendServer 的 healthy 成员如何保证线程安全？](#16-backendserver-的-healthy-成员如何保证线程安全)
17. [ServiceCatalog 的锁粒度如何设计？](#17-servicecatalog-的锁粒度如何设计)

### WebSocket 相关
18. [WebSocket 握手如何验证？](#18-websocket-握手如何验证)
19. [WebSocket 为什么客户端发送要掩码？](#19-websocket-为什么客户端发送要掩码)

### RPC 相关
20. [Protobuf 比 JSON 快多少？为什么？](#20-protobuf-比-json-快多少为什么)

### HTTP 模块相关
21. [如何处理 HTTP 粘包问题？](#21-如何处理-http-粘包问题)
22. [HTTP Keep-Alive 如何实现？](#22-http-keep-alive-如何实现)
23. [如何解析 HTTP 请求？](#23-如何解析-http-请求)

### 连接池相关
24. [连接池如何实现超时获取？](#24-连接池如何实现超时获取)
25. [如何处理连接失效？](#25-如何处理连接失效)

### epoll 相关
26. [epoll LT 和 ET 模式区别？](#26-epoll-lt-和-et-模式区别)
27. [什么是惊群问题？如何解决？](#27-什么是惊群问题如何解决)

### 深入问题
28. [wait_for 超时唤醒后会发生什么？](#28-wait_for-超时唤醒后会发生什么)
29. [什么是虚假唤醒？如何避免？](#29-什么是虚假唤醒如何避免)

---

## 问答详解

### 1. 什么是 Reactor 模式？为什么用 Reactor 而不是 Proactor？

**Reactor 模式**是一种事件驱动的设计模式，核心思想是"不要等待 I/O，让 I/O 完成后通知你"。

**Reactor vs Proactor**：

| 模式 | 工作方式 | 通知时机 |
|------|----------|----------|
| Reactor | 可读了通知我 | I/O 就绪时 |
| Proactor | 帮我读完通知我 | I/O 完成后 |

**为什么选择 Reactor**：

1. **Linux 上 epoll 更成熟**：Proactor 在 Linux 上需要额外模拟
2. **代码更可控**：自己控制读写时机，便于处理边界情况
3. **跨平台**：Reactor 可以用 epoll/kqueue/select 实现

---

### 2. One Loop Per Thread 的优势是什么？

**核心优势：无锁并发**

```
传统模型：多线程共享数据，需要锁
┌─────────┐
│ Thread1 │──┐
│ Thread2 │──┼──▶ 共享数据 ◀── 需要锁保护
│ Thread3 │──┘
└─────────┘

One Loop Per Thread：每个连接只在一个线程处理
┌─────────┐     ┌─────────┐     ┌─────────┐
│ Loop 1  │     │ Loop 2  │     │ Loop 3  │
│ conn1,2 │     │ conn3,4 │     │ conn5,6 │
│ 无锁    │     │ 无锁    │     │ 无锁    │
└─────────┘     └─────────┘     └─────────┘
```

**具体优势**：

1. **无锁设计**：每个 TcpConnection 属于一个 EventLoop
2. **缓存友好**：数据局部性好
3. **可扩展**：加线程就能提升性能

---

### 3. 如何实现跨线程的任务调度？

**机制**：eventfd + epoll + 任务队列

```
Thread A (mainLoop)              Thread B (subLoop)
       │                              │
       │ queueInLoop(task)            │
       │       │                      │
       │       ▼                      │
       │  push 到队列                 │
       │       │                      │
       │       ▼                      │
       │  write(eventfd) ────────────▶│ epoll_wait 返回
       │                              │
       │                              ▼
       │                         handleRead()
       │                              │
       │                              ▼
       │                         doPendingFunctors()
```

**关键代码**：

```cpp
void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();  // 同一线程，直接执行
    } else {
        queueInLoop(cb);  // 跨线程，放入队列
    }
}
```

---

### 4. EventLoop 如何保证线程安全？

**两个层面**：

1. **数据结构的线程安全**：用 mutex 保护 pendingFunctors_
2. **线程归属检查**：isInLoopThread() 确保操作在正确线程

**核心原则**：一个 EventLoop 的核心数据只在一个线程访问，跨线程操作通过队列调度。

---

### 5. 时间轮和红黑树定时器的区别？

| 特性 | 时间轮 | 红黑树 |
|------|--------|--------|
| 添加 | O(1) | O(log n) |
| 删除 | O(1) | O(log n) |
| 触发 | O(n/桶数) | O(1) |
| 适用场景 | 定时器多 | 定时器少 |

**时间轮更适合服务端**：连接多，定时器多，O(1) 更重要。

---

### 6. 时间轮如何处理超过覆盖范围的定时器？

**方案 1（本项目）**：取模放桶，可能提前触发
- 适用场景：超时稍微提前可接受

**方案 2**：层级时间轮（类似时钟的时/分/秒）
- 适用场景：需要精确的长超时

---

### 7. 时间轮的精度由什么决定？

**精度 = tick 间隔**

```
tickMs = 1000ms → 精度 1 秒
tickMs = 100ms  → 精度 0.1 秒
```

**trade-off**：精度越高，tick 开销越大。服务端通常秒级精度足够。

---

### 8. 双缓冲区和循环队列的区别？

| 操作 | 双缓冲区 | 循环队列 |
|------|----------|----------|
| 数据交接 | swap 指针 O(1) | 逐条复制 O(n) |
| 内存分配 | 预分配两块 | 动态或预分配 |

**双缓冲区更适合批量场景**：一次交接所有数据，零拷贝。

---

### 9. 为什么用 swap 指针而不是数据拷贝？

```cpp
// 数据拷贝：复制所有 LogEntry，O(n)
bufferA_ = bufferB_;

// 指针交换：只改指针，O(1)
std::swap(currentBuffer_, flushBuffer_);
```

**精髓**：指针指向不同的物理 buffer，交换指针就是交换角色，零拷贝。

---

### 10. 如何保证日志不丢失？

**三个层面**：

1. **内存层面**：buffer 满了立即刷盘
2. **时间层面**：最多 100ms 延迟
3. **退出层面**：优雅退出，刷完再走

---

### 11. 为什么无条件 swap？

**三个原因**：

1. **超时刷盘**：即使 buffer 没满，100ms 后也要刷
2. **优雅退出**：running_ = false 时，把剩余日志刷完
3. **代码简洁**：不需要复杂的条件判断

---

### 12. 加权轮询和平滑加权轮询的区别？

**普通加权轮询**：
```
服务器 A 权重 5，B 权重 1
请求分布：A A A A A B A A A A A B ...
问题：连续 5 个 A，分布不均匀
```

**平滑加权轮询（Nginx 算法）**：
```
每个服务器维护 currentWeight
每请求: currentWeight += weight
选 currentWeight 最大的
被选中后: currentWeight -= totalWeight

分布：A A B A A A B A A A B ... 更均匀！
```

---

### 13. 一致性哈希为什么需要虚拟节点？

**没有虚拟节点**：
1. 数据倾斜：某服务器可能只负责很小的范围
2. 扩缩容影响大

**有虚拟节点**（每服务器 150 个）：
1. 负载均匀
2. 扩缩容影响小

---

### 14. 最小连接数策略的实现有什么坑？

**坑 1：连接数更新时机**
- 选中后要 `connections++`
- 还要提供 `release()` 方法

**坑 2：健康服务器变化时的分配不均**
- 改进方案：遍历找最小，不用全局 index

---

### 15. RoundRobinStrategy 的 index_ 为什么用 atomic？

**原因**：多线程并发调用 select()

**为什么不用 mutex？**
- atomic 是 CPU 指令级别，比 mutex 快很多
- fetch_add 是无锁操作

---

### 16. BackendServer 的 healthy 成员如何保证线程安全？

**解决方案**：

1. 用 atomic<bool>
2. 加锁保护
3. 接受 TOCTOU（选到不健康服务器时重试）

本项目选择：调用者加锁保护。

---

### 17. ServiceCatalog 的锁粒度如何设计？

**当前设计**：每个操作对整个 catalog_ 加锁（粗粒度）

**为什么不用细粒度锁？**
1. 锁管理复杂
2. 服务注册/发现频率不高，粗粒度锁够用

---

### 18. WebSocket 握手如何验证？

**Accept Key 计算**：

```cpp
string computeAcceptKey(string clientKey) {
    const string GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    string combined = clientKey + GUID;
    
    // SHA-1 哈希 + Base64 编码
    return base64Encode(SHA1(combined));
}
```

---

### 19. WebSocket 为什么客户端发送要掩码？

**原因**：防止缓存污染攻击

**RFC 6455 规定**：
- 客户端发送到服务器：必须掩码
- 服务器发送到客户端：不掩码

---

### 20. Protobuf 比 JSON 快多少？为什么？

| 指标 | JSON | Protobuf |
|------|------|----------|
| 序列化速度 | 基准 | 快 5-10x |
| 数据大小 | 基准 | 小 3-5x |

**原因**：

1. **二进制格式**：直接内存布局，无文本解析
2. **类型确定**：编译时确定，无需运行时判断
3. **紧凑编码**：varint 等技术减少数据量

---

## 总结

### 必背知识点

| 模块 | 核心考点 |
|------|----------|
| Reactor | 事件驱动、epoll、One Loop Per Thread |
| 时间轮 | O(1)、覆盖范围、精度 |
| 异步日志 | 双缓冲区、零拷贝 swap、超时刷盘 |
| 负载均衡 | 平滑加权轮询、虚拟节点、最小连接数 |
| 线程安全 | atomic vs mutex、锁粒度、TOCTOU |
| WebSocket | 握手验证、掩码原因 |
| RPC | Protobuf vs JSON、性能差异原因 |

---

## 新增问答

### 21. 如何处理 HTTP 粘包问题？

**粘包问题**：TCP 是字节流，多个请求可能粘在一起，或一个请求被拆开。

**解决方案**：先 peek 确认完整，再消费数据

```cpp
ParseResult parseRequest(Buffer* buf, HttpRequest& request) {
    // 1. peek 数据，不消费
    const char* data = buf->peek();
    size_t len = buf->readableBytes();

    // 2. 找请求头结束位置
    const char* headerEnd = memmem(data, len, "\r\n\r\n", 4);
    if (!headerEnd) return Incomplete;  // 请求头不完整

    // 3. 检查请求体是否完整
    size_t contentLen = request.contentLength();
    if (len < headerLen + contentLen) return Incomplete;

    // 4. 确认完整，才消费
    buf->retrieve(headerLen);
    request.body.assign(buf->peek(), contentLen);
    buf->retrieve(contentLen);

    return Complete;
}
```

**关键点**：
- peek 不移动读指针，下次还能读到
- 确认数据完整后才 retrieve（消费）
- 支持 HTTP Pipeline（一个连接多个请求）

---

### 22. HTTP Keep-Alive 如何实现？

**Keep-Alive 作用**：复用 TCP 连接，避免重复握手。

**实现要点**：

```cpp
// 1. 解析请求时检查 Connection 头
bool keepAlive() const {
    auto it = headers.find("Connection");
    if (version == HTTP_1_1) {
        // HTTP/1.1 默认 Keep-Alive
        return it == headers.end() ||
               strcasecmp(it->second.c_str(), "close") != 0;
    } else {
        // HTTP/1.0 默认关闭
        return it != headers.end() &&
               strcasecmp(it->second.c_str(), "keep-alive") == 0;
    }
}

// 2. 响应时设置 Connection 头
if (request.keepAlive()) {
    response.setHeader("Connection", "keep-alive");
    response.setHeader("Keep-Alive", "timeout=60, max=100");
} else {
    response.closeConnection = true;
}

// 3. 关闭连接
if (response.closeConnection) {
    conn->shutdown();
}
```

---

### 23. 如何解析 HTTP 请求？

**解析流程**：

```
1. 找 \r\n\r\n 分隔请求头和请求体
2. 解析请求行: GET /path?query HTTP/1.1
3. 解析请求头: 每行是 Key: Value
4. 解析请求体: 根据 Content-Length
```

**关键代码**：

```cpp
bool parseRequestLine(const string& line) {
    // GET /path HTTP/1.1
    size_t pos1 = line.find(' ');  // 方法结束
    size_t pos2 = line.rfind(' '); // 版本开始

    method = parseMethod(line.substr(0, pos1));
    path = line.substr(pos1 + 1, pos2 - pos1 - 1);
    version = parseVersion(line.substr(pos2 + 1));

    // 解析 query string
    size_t q = path.find('?');
    if (q != string::npos) {
        query = path.substr(q + 1);
        path = path.substr(0, q);
    }

    return true;
}
```

---

### 24. 连接池如何实现超时获取？

**实现**：condition_variable + wait_for

```cpp
Connection::Ptr acquire(int timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);

    // 等待：有可用连接 或 池未满 或 超时
    bool success = cv_.wait_for(lock,
        std::chrono::milliseconds(timeoutMs),
        [this] {
            return closed_ || !pool_.empty() || totalCreated_ < maxSize_;
        });

    if (!success || closed_) return nullptr;  // 超时或已关闭

    // 有空闲连接
    if (!pool_.empty()) {
        auto conn = pool_.front();
        pool_.pop();
        conn->markUsed();
        return conn;
    }

    // 创建新连接（锁外执行避免阻塞）
    if (totalCreated_ < maxSize_) {
        totalCreated_++;
        lock.unlock();
        int fd = createConnection(host_, port_);
        lock.lock();
        if (fd < 0) {
            totalCreated_--;
            return nullptr;
        }
        return std::make_shared<Connection>(fd, host_, port_);
    }

    return nullptr;
}
```

---

### 25. 如何处理连接失效？

**方案 1：归还时检查**

```cpp
void release(Connection::Ptr conn) {
    if (!conn || !conn->valid()) {
        // 无效连接，丢弃并减少计数
        std::lock_guard<std::mutex> lock(mutex_);
        if (totalCreated_ > 0) totalCreated_--;
        cv_.notify_one();
        return;
    }
    // 有效连接，归还
    pool_.push(conn);
    cv_.notify_one();
}
```

**方案 2：定期健康检查**

```cpp
void healthCheck() {
    int64_t now = time(nullptr);
    // 清理超过 60 秒未使用的连接
    // 但保留 minSize 个
}
```

**方案 3：使用时检测**

```cpp
// 获取连接后，发送前检测
auto conn = pool.acquire();
if (conn) {
    // 可以发送心跳包检测
    if (!isAlive(conn)) {
        pool.release(conn);  // 无效会丢弃
        conn = pool.acquire();  // 重新获取
    }
}
```

---

### 26. epoll LT 和 ET 模式区别？

| 模式 | 触发条件 | 代码要求 |
|------|----------|----------|
| **LT** | 只要可读/可写就触发 | 正常读写即可 |
| **ET** | 状态变化时触发 | 必须循环读直到 EAGAIN |

**ET 模式必须循环读**：

```cpp
// ET 模式
void handleReadET(int fd) {
    while (true) {
        int n = read(fd, buf, sizeof(buf));
        if (n == -1) {
            if (errno == EAGAIN) break;  // 读完了
            perror("read");
            break;
        } else if (n == 0) {
            // 对端关闭
            close(fd);
            break;
        }
        process(buf, n);
    }
}
```

**本项目选择 LT**：
- 实现简单，不用循环读
- 更安全，不会漏事件
- 性能足够

---

### 27. 什么是惊群问题？如何解决？

**惊群问题**：

```
多个线程监听同一个 listenfd:
Thread 1 ──┐
Thread 2 ──┼──▶ epoll_wait(listenfd)
Thread 3 ──┘

新连接到来 → 所有线程都被唤醒 → 但只有一个能 accept 成功
```

**解决方案**：

**方案 1：One Loop Per Thread（本项目）**

```
mainLoop: 只负责 accept
    新连接 → 分发给 subLoop

subLoop: 只处理已建立连接
    不监听 listenfd

每个 fd 只在一个线程监听，天然避免惊群
```

**方案 2：SO_REUSEPORT（Linux 3.9+）**

```cpp
int opt = 1;
setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
// 内核自动负载均衡，多个进程可以绑定同一端口
```

**方案 3：加锁**

```cpp
// accept 前加锁
lock();
int connfd = accept(listenfd, ...);
unlock();
```

---

### 28. wait_for 超时唤醒后会发生什么？

**wait_for 行为**：

```cpp
cv_.wait_for(lock, 100ms, predicate);
```

| 情况 | 返回值 | 谓词状态 | 后续行为 |
|------|--------|----------|----------|
| notify + 谓词满足 | true | true | 继续执行 |
| notify + 谓词不满足 | 继续 wait | - | 不返回 |
| **超时** | **false** | **可能 false** | **继续执行** |

**关键**：超时后，无论谓词是否满足，都会返回执行后续代码。

**代码示例**：

```cpp
// 双缓冲区日志
void writerLoop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 超时或谓词满足都会返回
        cv_.wait_for(lock, 100ms, []{
            return !running_ || currentBuffer_->size() >= 1000;
        });

        // 无条件 swap（超时也会 swap）
        std::swap(currentBuffer_, flushBuffer_);
    }
    // 写文件...
}
```

---

### 29. 什么是虚假唤醒？如何避免？

**虚假唤醒**：condition_variable 在没有 notify 的情况下也可能唤醒。

**原因**：
- 操作系统实现细节
- 信号中断
- 多核 CPU 竞争

**必须用循环检查**：

```cpp
// 错误写法
cv_.wait(lock);
if (queue_.empty()) return;  // 可能为空！

// 正确写法 1：循环
while (queue_.empty()) {
    cv_.wait(lock);
}

// 正确写法 2：谓词
cv_.wait(lock, []{ return !queue_.empty(); });
```

**带谓词的 wait 内部实现**：

```cpp
// 等价于
while (!predicate()) {
    wait(lock);
}
```

---

## 面试回答技巧

### 回答结构

1. **先说结论**（1-2 句）
2. **展开细节**（代码、图表）
3. **说 trade-off**（为什么这样选，有什么代价）

### 常见错误

| 错误 | 正确做法 |
|------|----------|
| 只回答"是什么" | 还要回答"为什么" |
| 只说优点 | 还要说缺点和 trade-off |
| 背诵概念 | 用自己的话解释，举代码例子 |
| 回答太长 | 先说核心，再展开 |

### 示例回答

**问：为什么用双缓冲区？**

**答**：

双缓冲区实现了写入和刷盘的完全并行，核心是 swap 指针的零拷贝交接。

（展开）
- 两个物理 buffer，用指针指向
- swap 只改指针，O(1)
- 写入和刷盘完全不竞争

（trade-off）
- 相比循环队列，双缓冲区更适合批量交接
- 缺点是内存占用稍大（两块 buffer）

---

## 源码深度追问

以下问题是面试官在基础回答后可能的深度追问，回答需要精确到源码文件和实现细节。

### 追问链 1：TcpConnection 生命周期

**Q1: TcpConnection 为什么用 shared_ptr 管理？**

**答**：TcpConnection 的生命周期跨越多个线程（mainLoop 创建、subLoop 使用、mainLoop 销毁），且回调函数可能在连接即将关闭时被调用。shared_ptr 通过引用计数保证在所有使用者释放后才析构。

**源码**（`TcpServer.cc:170`）：
```cpp
TcpConnectionPtr conn(new TcpConnection(ioLoop, connName, sockfd, localAddr, peerAddr));
connections_[connName] = conn;  // 引用计数 +1
```

**追问 Q2: Channel 为什么不直接持有 shared_ptr？**

**答**：如果 Channel 持有 TcpConnection 的 shared_ptr，而 TcpConnection 又持有 Channel（通过 unique_ptr），会形成循环引用，shared_ptr 永远不会归零，造成内存泄漏。所以 Channel 使用 `weak_ptr`（通过 `tie()` 机制）。

**源码**（`TcpConnection.cc:80`）：
```cpp
void TcpConnection::connectEstablished() {
    channel_->tie(shared_from_this());  // weak_ptr，不增加引用计数
}
```

**追问 Q3: handleEvent 时如何确保 TcpConnection 还活着？**

**答**：`Channel::handleEvent()` 中先对 `tie_` 做 `lock()`，如果返回有效的 shared_ptr（引用计数 +1），说明 TcpConnection 还活着，安全执行回调。如果 lock() 返回空，说明 TcpConnection 已析构，跳过回调。

**追问 Q4: removeConnection 为什么是两阶段的？**

**答**：
- 阶段一（mainLoop 线程）：从 `connections_` map 中移除，但 conn 的 shared_ptr 被 `bind` 到 `connectDestroyed` 回调中，引用计数不归零
- 阶段二（subLoop 线程）：执行 `connectDestroyed()`，`channel_->disableAll()` 从 Poller 移除，然后 bind 的 shared_ptr 析构，引用计数归零

**源码**（`TcpServer.cc:201-232`）：
```cpp
void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    connections_.erase(conn->name());       // 阶段一：map 移除
    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop(                    // 阶段二：转到 subLoop 执行
        std::bind(&TcpConnection::connectDestroyed, conn)  // conn 被 bind 捕获
    );
}
```

---

### 追问链 2：EventLoop 跨线程任务

**Q1: runInLoop 和 queueInLoop 有什么区别？**

**答**：
- `runInLoop`：如果在 EventLoop 线程中调用，立即执行；否则调用 `queueInLoop`
- `queueInLoop`：无论在哪个线程，都是加入队列等待执行

**追问 Q2: queueInLoop 里为什么有时不需要 wakeup？**

**答**：只有两种情况需要 wakeup：
1. 调用者不在 EventLoop 线程（需要唤醒 epoll_wait）
2. 当前正在执行 `doPendingFunctors()`（`callingPendingFunctors_ == true`），说明新任务要等下一轮才能执行，需要唤醒

如果在 EventLoop 线程且不在执行回调，新任务会在本轮 loop 的 `doPendingFunctors()` 中自然被执行，不需要额外唤醒。

**源码**（`EventLoop.cc:257-261`）：
```cpp
if (!isInLoopThread() || callingPendingFunctors_) {
    wakeup();
}
```

**追问 Q3: doPendingFunctors 里为什么用 swap 而不是 copy？**

**答**：两个原因：
1. **减少锁持有时间**：swap 是 O(1)，锁只需要持有极短时间。如果 copy 或遍历，锁持有时间和队列长度成正比
2. **避免死锁**：functors 执行过程中可能再调用 `queueInLoop` 加入新任务，如果此时还持有锁就死锁了

**源码**（`EventLoop.cc:345-363`）：
```cpp
void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);  // O(1)，释放锁后在锁外执行
    }
    for (const Functor& functor : functors) {
        functor();  // 执行时不持有锁
    }
    callingPendingFunctors_ = false;
}
```

---

### 追问链 3：Buffer 设计

**Q1: readFd 为什么用栈上 65536 字节的临时缓冲区？**

**答**：解决一个矛盾：Buffer 初始大小只有 1024 字节（节省内存），但单次 read 可能读到大量数据。用 `readv` 同时读到 Buffer 的可写区域和栈上缓冲区，一次系统调用就能读到 `writableBytes() + 64KB` 的数据。如果溢出到栈缓冲区，再 append 到 Buffer（触发扩容）。

**源码**（`Buffer.h`）：
```cpp
ssize_t Buffer::readFd(int fd, int* savedErrno) {
    char extrabuf[65536];              // 栈上 64KB
    struct iovec vec[2];
    vec[0] = { beginWrite(), writableBytes() };  // 先填 Buffer
    vec[1] = { extrabuf, sizeof(extrabuf) };     // 溢出到栈
    ssize_t n = readv(fd, vec, 2);               // 一次系统调用
}
```

**追问 Q2: makeSpace 什么时候移动数据，什么时候扩容？**

**答**：判断 `writableBytes() + prependableBytes()` 是否够用：
- **够用**：把可读数据移动到 `kCheapPrepend`（8字节）位置，复用已读空间。O(n) 数据复制，但不分配新内存
- **不够**：直接 `resize`，可能触发内存重新分配

**源码**（`Buffer.h:82-100`）：
```cpp
if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
    buffer_.resize(writerIndex_ + len);     // 扩容
} else {
    size_t readable = readableBytes();
    std::copy(begin() + readerIndex_,       // 移动数据到前端
              begin() + writerIndex_,
              begin() + kCheapPrepend);
    readerIndex_ = kCheapPrepend;
    writerIndex_ = readerIndex_ + readable;
}
```

---

### 追问链 4：HTTP 解析

**Q1: 为什么先 peek 再 retrieve，而不是直接 retrieveAllAsString？**

**答**：TCP 是字节流，可能只收到了半个请求。如果直接 retrieve 消费了数据，后面发现请求不完整就无法回退。peek 只读不消费，确认完整后才 retrieve。

**追问 Q2: 如何检测请求头是否完整？**

**答**：用 `memmem()` 在 Buffer 中搜索 `"\r\n\r\n"` 标记。HTTP 协议规定请求头和请求体之间以空行分隔。

**追问 Q3: 请求体不完整怎么办？**

**答**：解析完请求头后，从 `Content-Length` 获取请求体长度。如果 `readableBytes() < headerLen + contentLen`，返回 `ParseResult::Incomplete`，等下次 epoll 触发再继续。Buffer 中的数据不会丢失（没有 retrieve）。

**追问 Q4: HTTP Pipeline 是怎么工作的？**

**答**：`onMessage` 中用 `while (buf->readableBytes() > 0)` 循环。一次 `readFd` 可能读入多个完整请求（Pipeline），循环逐个解析并响应。如果最后剩下的数据不完整，返回 `Incomplete`，等待下次数据到来拼接完整。

---

### 追问链 5：RPC 粘包处理

**Q1: Protobuf-RPC 如何解决 TCP 粘包？**

**答**：使用长度前缀协议。每条消息前面有 4 字节（网络字节序）表示后续 Protobuf 数据的长度。接收方先读 4 字节拿到长度，再判断是否收到了完整数据。

**追问 Q2: 为什么用网络字节序（big-endian）？**

**答**：TCP 协议规定传输层使用 big-endian。不同机器可能是 big-endian 或 little-endian，统一使用 `htonl()`/`ntohl()` 转换可以保证互操作性。

**追问 Q3: 如果收到恶意的超大 length 怎么办？**

**答**：有帧长度校验。服务端限制 `kMaxFrameSize = 10MB`，客户端限制 `RPC_MAX_FRAME_LENGTH = 64MB`。超过限制直接断开连接。

**源码**（`RpcServerPb.h:74-78`）：
```cpp
if (len <= 0 || static_cast<size_t>(len) > kMaxFrameSize) {
    sendError(conn, 0, -32600, "Invalid frame size");
    conn->shutdown();
    return;
}
```

---

### 追问链 6：异步日志

**Q1: 双缓冲区什么时候交换？**

**答**：两种触发条件（`AsyncLogger.h:243-246`）：
1. `currentBuffer_->size() >= kFlushThreshold`（缓冲区满，1000 条）
2. `wait_for` 超时 100ms

**追问 Q2: 交换时其他线程还能写日志吗？**

**答**：交换只需要 `std::swap(currentBuffer_, flushBuffer_)`，这是一个 O(1) 的指针交换操作，锁持有时间极短。交换后 `currentBuffer_` 指向空的 buffer，其他线程立即可以写入，不会被后台刷盘阻塞。

**追问 Q3: 优雅退出时如何保证日志不丢？**

**答**：`stop()` 方法设置 `running_ = false`，后台线程的循环条件是 `while (running_.load() || !currentBuffer_->empty())`，即使停止了也会把当前缓冲区中的日志刷完才退出。

---

### 追问链 7：WebSocket

**Q1: WebSocket 握手的 Accept Key 怎么计算？**

**答**：将客户端的 `Sec-WebSocket-Key` 拼接 RFC 6455 规定的固定 GUID `"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"`，做 SHA-1 哈希后 Base64 编码。这不是为了安全，而是为了证明服务端理解 WebSocket 协议。

**追问 Q2: 为什么客户端发送数据要掩码，服务端不用？**

**答**：RFC 6455 规定客户端→服务端必须掩码，服务端→客户端不掩码。目的是防止中间代理（如 HTTP 代理）误将 WebSocket 数据当作 HTTP 缓存，造成缓存投毒攻击。掩码 XOR 操作使得 WebSocket 数据不会被误匹配为有效 HTTP 响应。

**追问 Q3: 帧长度为什么有三种编码方式？**

**答**：为了紧凑编码：
- ≤125 字节：直接用 7 bit（1 字节头）
- ≤65535 字节：用 126 标记 + 2 字节扩展长度
- 更大：用 127 标记 + 8 字节扩展长度

绝大多数消息在 125 字节以内，只需要 2 字节帧头（1 字节 FIN+opcode + 1 字节 length），非常高效。
