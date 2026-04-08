# mymuduo-http 设计原理

> 本文档从设计者的视角，深入讲解每个模块的设计思路和原理，帮助你理解"为什么这样做"。

---

## 目录

1. [设计理念](#1-设计理念)
2. [Reactor 模式：为什么用事件驱动？](#2-reactor-模式为什么用事件驱动)
3. [线程模型：为什么是 One Loop Per Thread？](#3-线程模型为什么是-one-loop-per-thread)
4. [时间轮：为什么比红黑树更好？](#4-时间轮为什么比红黑树更好)
5. [双缓冲区：为什么不是循环队列？](#5-双缓冲区为什么不是循环队列)
6. [负载均衡：为什么需要 5 种策略？](#6-负载均衡为什么需要-5-种策略)
7. [服务注册：为什么需要心跳？](#7-服务注册为什么需要心跳)
8. [设计权衡：性能 vs 复杂度](#8-设计权衡性能-vs-复杂度)
9. [HTTP 模块：如何处理粘包？](#9-http-模块如何处理粘包)
10. [连接池：为什么需要连接复用？](#10-连接池为什么需要连接复用)
11. [epoll：LT vs ET 模式](#11-epolllt-vs-et-模式)
12. [epoll vs select vs poll](#12-epoll-vs-select-vs-poll)
13. [惊群问题](#13-惊群问题)

---

## 1. 设计理念

### 1.1 核心原则

本项目遵循以下设计原则：

| 原则 | 说明 | 体现 |
|------|------|------|
| **高性能** | 减少 CPU、内存、I/O 开销 | epoll、时间轮 O(1)、零拷贝 |
| **可扩展** | 易于添加新功能 | 策略模式、模块化 |
| **简洁** | 不过度设计 | 避免过早优化 |
| **正确性** | 宁可慢，也不要错 | 线程安全、资源管理 |

### 1.2 从问题出发

设计不是凭空想象，而是为了解决问题。每个模块都有它存在的理由：

```
问题                          解决方案
─────────────────────────────────────────
如何处理大量连接？        →   Reactor + epoll
如何利用多核 CPU？        →   One Loop Per Thread
如何管理大量定时器？      →   时间轮
如何避免日志阻塞业务？    →   双缓冲区异步日志
如何分发请求到多服务器？  →   负载均衡
如何知道服务是否存活？    →   心跳 + 过期清理
```

---

## 2. Reactor 模式：为什么用事件驱动？

### 2.1 传统阻塞模型的问题

假设我们要写一个 echo 服务器：

```cpp
// 传统阻塞模型
void handleClient(int fd) {
    char buf[1024];
    while (true) {
        int n = read(fd, buf, sizeof(buf));  // 阻塞等待数据
        if (n <= 0) break;
        write(fd, buf, n);  // 阻塞写入
    }
    close(fd);
}

// 为每个连接创建一个线程
void main() {
    int listenfd = createServer(8080);
    while (true) {
        int connfd = accept(listenfd, ...);  // 阻塞等待连接
        std::thread t(handleClient, connfd);  // 新线程处理
        t.detach();
    }
}
```

**问题在哪？**

| 连接数 | 线程数 | 问题 |
|--------|--------|------|
| 10 | 10 | 还好 |
| 1000 | 1000 | 内存爆炸（每线程 8MB 栈 = 8GB） |
| 10000 | 10000 | 系统崩溃 |

**根本原因**：线程是昂贵的资源，不能一个连接一个线程。

### 2.2 Reactor 的思路

**核心思想**：不要等待 I/O，让 I/O 完成后来通知你。

```cpp
// Reactor 模型
void main() {
    int listenfd = createServer(8080);
    int epfd = epoll_create1(0);

    // 监听 listenfd 的可读事件
    epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev);

    while (true) {
        int n = epoll_wait(epfd, events, MAX, timeout);  // 等待事件
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listenfd) {
                // 新连接
                int connfd = accept(listenfd, ...);
                epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, ...);
            } else {
                // 数据到达
                handleRead(events[i].data.fd);
            }
        }
    }
}
```

**一个线程，处理无数连接**：线程不阻塞在 read()，而是阻塞在 epoll_wait()，只有当有事件发生时才处理。

### 2.3 为什么选择 Reactor 而不是 Proactor？

| 模式 | 工作方式 | 平台支持 |
|------|----------|----------|
| **Reactor** | 可读了通知我，我自己读 | Linux (epoll), 跨平台 |
| **Proactor** | 帮我读完，通知我 | Windows (IOCP), Linux 支持 |

**选择 Reactor 的原因**：

1. **Linux 上 epoll 更成熟**：Proactor 在 Linux 上需要额外模拟
2. **代码更可控**：自己控制读写时机，便于处理边界情况
3. **跨平台**：epoll/kqueue/select 都能用 Reactor 模式

---

## 3. 线程模型：为什么是 One Loop Per Thread？

### 3.1 线程模型对比

| 模型 | 优点 | 缺点 | 典型场景 |
|------|------|------|----------|
| 单线程 | 简单，无锁 | 无法利用多核 | 简单代理 |
| 连接/线程 | 简单 | 线程开销大 | 传统服务器 |
| **One Loop Per Thread** | 高效，无锁 | 实现复杂 | 高性能服务器 |
| Leader-Follower | 无需分发 | 实现最复杂 | 特殊场景 |

### 3.2 One Loop Per Thread 的核心思想

```
┌─────────────────────────────────────────────────────────────┐
│                         进程                                 │
│                                                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐        │
│  │  Thread 1   │  │  Thread 2   │  │  Thread 3   │        │
│  │  (mainLoop) │  │  (subLoop)  │  │  (subLoop)  │        │
│  │             │  │             │  │             │        │
│  │  只负责     │  │  处理连接   │  │  处理连接   │        │
│  │  accept     │  │  conn1,2,3  │  │  conn4,5,6  │        │
│  └─────────────┘  └─────────────┘  └─────────────┘        │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

**关键设计**：

1. **每个 EventLoop 绑定一个线程**：创建时记录线程 ID，后续操作检查是否在同一线程
2. **连接只在一个线程处理**：一个 TcpConnection 属于一个 EventLoop，不需要锁
3. **mainLoop 分发连接**：新连接通过轮询分配给 subLoop

### 3.3 为什么这样设计？

**无锁并发**：

```cpp
// TcpConnection 的操作都在同一个 EventLoop 线程
void TcpConnection::send(const string& msg) {
    if (loop_->isInLoopThread()) {
        sendInLoop(msg);  // 同一线程，直接发送
    } else {
        loop_->runInLoop([this, msg] { sendInLoop(msg); });  // 跨线程，调度过去
    }
}
```

**连接只属于一个 Loop，不需要锁保护**。这就是 One Loop Per Thread 的精髓。

### 3.4 跨线程通信：wakeup 机制

**问题**：mainLoop 如何通知 subLoop 有新任务？

**方案**：eventfd + epoll

```
mainLoop 想让 subLoop 处理新连接:
1. 把连接信息放入 subLoop 的队列
2. 写 eventfd 唤醒 subLoop
3. subLoop 从 epoll_wait 返回
4. subLoop 处理队列中的任务
```

```cpp
// 唤醒机制
void EventLoop::wakeup() {
    uint64_t one = 1;
    ::write(wakeupFd_, &one, sizeof(one));  // 写入 eventfd
}

// EventLoop 处理唤醒
void EventLoop::handleRead() {
    uint64_t one;
    ::read(wakeupFd_, &one, sizeof(one));  // 消费事件
}

// 执行跨线程任务
void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);  // swap 减少临界区
    }
    for (auto& f : functors) {
        f();
    }
}
```

**为什么用 swap？**

```cpp
// 不好的写法：临界区时间长
void doPendingFunctors() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& f : pendingFunctors_) {
        f();  // 执行回调时还持有锁！
    }
}

// 好的写法：swap 后释放锁
void doPendingFunctors() {
    std::vector<Functor> functors;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);  // O(1) swap
    }  // 锁释放
    for (auto& f : functors) {
        f();  // 锁外执行，不会死锁
    }
}
```

---

## 4. 时间轮：为什么比红黑树更好？

### 4.1 定时器的本质

定时器需要支持三个操作：
- **添加**：设置一个 future 任务
- **删除**：取消任务
- **触发**：执行到期的任务

### 4.2 常见实现方式对比

| 实现方式 | 添加 | 删除 | 触发 | 适用场景 |
|---------|------|------|------|---------|
| 有序链表 | O(n) | O(1) | O(1) | 定时器很少 |
| 最小堆 | O(log n) | O(n) | O(1) | 定时器少 |
| 红黑树 | O(log n) | O(log n) | O(1) | 定时器中等 |
| **时间轮** | **O(1)** | **O(1)** | O(n/桶数) | **定时器很多** |

### 4.3 时间轮原理

**想象一个时钟**：
- 秒针每秒走一格
- 每个格子里的任务到期就执行

```
时间轮 (60 个桶，每桶 1 秒):

    [桶0] ── [桶1] ── [桶2] ── ... ── [桶59]
       │
       └── 当前指针 currentBucket_

每秒 tick 一次：
    1. 处理当前桶里的定时器
    2. 指针移动到下一桶
```

**添加定时器**：

```cpp
int64_t addTimer(TimerCallback cb, int delayMs) {
    // 计算应该放入哪个桶
    size_t ticks = (delayMs + tickMs_ - 1) / tickMs_;  // 向上取整
    size_t bucket = (currentBucket_ + ticks) % buckets_;

    auto timer = std::make_shared<Timer>(cb, delayMs);
    wheel_[bucket].push_back(timer);  // O(1) 插入链表尾部

    return timer->id();
}
```

### 4.4 为什么选择时间轮？

**场景分析**：服务端定时器特点
- 数量多（每个连接可能有超时定时器）
- 删除频繁（超时后要删除）
- 精度要求不高（秒级即可）

**时间轮正好适合**：
- O(1) 添加：直接放入对应桶
- O(1) 删除：用 hash map 辅助快速定位
- 批量触发：一次 tick 处理一个桶的所有定时器

### 4.5 时间轮的局限

**问题**：时间轮有覆盖范围（如 60 秒），超过范围的定时器怎么办？

**本项目方案**：取模放桶，可能提前触发
- 对于服务端场景，超时稍微提前触发是可接受的
- 如果需要精确的长超时，可以用层级时间轮（像时钟的时/分/秒）

---

## 5. 双缓冲区：为什么不是循环队列？

### 5.1 异步日志的需求

```
业务线程                    后台线程
    │                          │
    │  写日志 (不能阻塞)        │  刷磁盘 (很慢)
    │         │                │
    │         ▼                ▼
    │    [缓冲区] ──────────▶ [写文件]
    │                          │
    │    立即返回              慢慢写
```

**核心需求**：业务线程写入要快，不能被刷盘阻塞。

### 5.2 方案对比

**方案 1：单缓冲区 + 锁**

```
业务线程 A ──┐
业务线程 B ──┼──▶ [Buffer] ──▶ 后台线程写文件
业务线程 C ──┘
        ↑
      同一把锁
```

**问题**：写入和刷盘竞争同一把锁，刷盘慢会阻塞写入。

**方案 2：循环队列**

```
生产者写入 ──▶ [0][1][2][3][4][5]... ──▶ 消费者读取
                   ↑
              一条一条取
```

**问题**：
- 数据交接需要逐条复制
- 队列满时要丢弃或阻塞

**方案 3：双缓冲区（本项目选择）**

```
业务线程                    后台线程
    │                          │
    ▼                          ▼
[Buffer A]  ←── swap ──→  [Buffer B]
 (正在写)      (零拷贝)    (正在刷)

写完后交换指针，业务线程继续写 A，后台线程刷 B
```

### 5.3 双缓冲区的精髓

**为什么是两个物理 buffer？**

```cpp
// 物理存储：两块大内存
std::vector<LogEntry> bufferA_;  // 实际存储
std::vector<LogEntry> bufferB_;  // 实际存储

// 指针：指向哪块内存
std::vector<LogEntry>* currentBuffer_;  // 指向正在写的 buffer
std::vector<LogEntry>* flushBuffer_;    // 指向正在刷的 buffer
```

**为什么用指针？**

```
如果不用指针，每次交换要复制数据：
    bufferA.swap(bufferB);  // 复制所有 LogEntry，O(n)

用指针，交换只要改指针：
    std::swap(currentBuffer_, flushBuffer_);  // 交换 8 字节，O(1)
```

### 5.4 swap 时机

**条件 1：buffer 满了**

```cpp
if (currentBuffer_->size() >= kFlushThreshold) {
    cv_.notify_one();  // 通知后台线程
}
```

**条件 2：超时（最多 100ms）**

```cpp
cv_.wait_for(lock, 100ms, predicate);
// 即使 buffer 没满，100ms 后也会 swap 刷盘
```

**为什么无条件 swap？**

```cpp
// 后台线程
void writerLoop() {
    while (running_) {
        cv_.wait_for(lock, 100ms, predicate);
        std::swap(currentBuffer_, flushBuffer_);  // 总是 swap
        // 写文件...
    }
}
```

**原因**：
1. **超时刷盘**：保证日志最多延迟 100ms
2. **优雅退出**：`running_ = false` 时，把剩余日志刷完再退出
3. **代码简洁**：不需要复杂的条件判断

---

## 6. 负载均衡：为什么需要 5 种策略？

### 6.1 不同场景的需求

| 场景 | 需求 | 适用策略 |
|------|------|----------|
| 服务器性能相近 | 均匀分配 | 轮询 |
| 服务器性能不均 | 按能力分配 | 加权轮询 |
| 长连接服务 | 按连接数分配 | 最小连接数 |
| 缓存集群 | 同 key 命中同一服务器 | 一致性哈希 |
| 简单场景 | 随机即可 | 随机 |

### 6.2 策略模式的设计

**为什么用策略模式？**

```cpp
// 不好的设计：硬编码
BackendServer* select(int strategy) {
    switch (strategy) {
        case 0: /* 轮询逻辑 */ break;
        case 1: /* 加权轮询逻辑 */ break;
        // ...
    }
}
// 问题：新增策略要改这个函数，违反开闭原则

// 好的设计：策略模式
class ILoadBalanceStrategy {
    virtual BackendServerPtr select(...) = 0;
};

class LoadBalancer {
    std::unique_ptr<ILoadBalanceStrategy> strategy_;

    void setStrategy(StrategyType type) {
        switch (type) {
            case RoundRobin:
                strategy_ = std::make_unique<RoundRobinStrategy>();
                break;
            // ...
        }
    }
};
```

**优势**：
- **开闭原则**：新增策略不影响现有代码
- **单一职责**：每个策略独立实现和测试

### 6.3 核心策略详解

**平滑加权轮询（Nginx 算法）**

普通加权轮询问题：
```
服务器 A 权重 5，B 权重 1
请求分布: AAAAA B  (连续 5 个 A，不够平滑)
```

平滑加权轮询：
```
每个服务器维护 currentWeight
每请求: currentWeight += weight
选 currentWeight 最大的
被选中后: currentWeight -= totalWeight

结果: AABAAA (分布更均匀)
```

**一致性哈希**

```
问题：缓存服务器 A 挂了，如果不做特殊处理，
     原本路由到 A 的请求全部打到 B，缓存全部失效

解决：一致性哈希 + 虚拟节点

哈希环:
        Server A#1
       /          \
   Server B#1    Server C#1
       \          /
        Server A#2

A 挂了，只影响 A 附近的请求，其他不变
```

---

## 7. 服务注册：为什么需要心跳？

### 7.1 分布式系统的不确定性

```
服务实例                    注册中心
    │                          │
    │ ─── register ──────────▶ │ 记录: user-service @ 192.168.1.1
    │                          │
    │        (服务挂了)         │
    │           ✕              │
    │                          │ 如果没有心跳，注册中心不知道服务挂了
    │                          │ 继续把流量发给挂掉的服务
    │                          │
    │                     ❌ 用户请求失败
```

### 7.2 心跳机制

```
服务实例                    注册中心
    │                          │
    │ ─── register ──────────▶ │
    │                          │
    │ ─── heartbeat ─────────▶ │ 更新 lastHeartbeat
    │ ─── heartbeat ─────────▶ │
    │ ─── heartbeat ─────────▶ │
    │                          │
    │        (服务挂了)         │
    │           ✕              │
    │                          │ 后台线程检查:
    │                          │ now - lastHeartbeat > 30s
    │                          │ 标记为 DOWN 或删除
    │                          │
    │                     ✅ 不再分发流量
```

### 7.3 实现要点

**心跳间隔 vs 超时时间**：

```
心跳间隔: 10 秒
超时时间: 30 秒

为什么要留余量？
- 网络抖动可能导致心跳延迟
- 避免误判服务不可用
```

**过期清理**：

```cpp
int cleanExpiredInstances() {
    int64_t now = time(nullptr);
    for (auto& [key, instances] : catalog_) {
        for (auto it = instances.begin(); it != instances.end(); ) {
            if (now - (*it)->lastHeartbeat > 30) {
                (*it)->status = "DOWN";
                it = instances.erase(it);
            } else {
                ++it;
            }
        }
    }
}
```

---

## 8. 设计权衡：性能 vs 复杂度

### 8.1 为什么用 mutex 而不是无锁？

| 方案 | 优点 | 缺点 |
|------|------|------|
| mutex | 简单可靠，不易出 bug | 可能阻塞 |
| 无锁 | 无阻塞 | 实现复杂，调试困难，可能有 ABA 问题 |

**本项目的选择**：

- **大部分场景用 mutex**：代码简单，性能够用
- **少数场景用 atomic**：如 `RoundRobinStrategy::index_`

```cpp
// 轮询策略：atomic 无锁计数
class RoundRobinStrategy {
    std::atomic<size_t> index_;

    BackendServerPtr select(...) {
        size_t idx = index_.fetch_add(1) % healthy.size();  // 无锁
        return healthy[idx];
    }
};
```

### 8.2 为什么用 Protobuf 也保留 JSON？

| 协议 | 优点 | 缺点 |
|------|------|------|
| JSON | 可读，易调试 | 慢，体积大 |
| Protobuf | 快，体积小 | 二进制，不可读 |

**选择策略**：

```
开发调试阶段 → 用 JSON-RPC (方便排查问题)
生产环境    → 用 Protobuf-RPC (高性能)
```

### 8.3 为什么不用协程？

**协程的优势**：
- 同步写法，异步执行
- 比回调更易读

**本项目不用协程的原因**：
- C++20 协程支持还不够成熟
- 回调模式更适合理解 Reactor 原理
- 学习目的，优先展示核心概念

---

## 总结

### 设计决策速查表

| 决策 | 选择 | 原因 |
|------|------|------|
| I/O 模型 | epoll | Linux 上最高效 |
| 并发模型 | One Loop Per Thread | 无锁并发，可扩展 |
| 定时器 | 时间轮 | O(1)，适合大量定时器 |
| 异步日志 | 双缓冲区 | 零拷贝交接，业务不阻塞 |
| 负载均衡 | 策略模式 | 易扩展，符合开闭原则 |
| 线程安全 | mutex + atomic | 简单可靠，够用 |
| 序列化 | JSON + Protobuf | 兼顾调试和性能 |

### 学习建议

1. **先理解问题**：每个模块是为了解决什么问题
2. **再看解决方案**：为什么这样设计能解决问题
3. **对比其他方案**：为什么不选其他方案
4. **动手实践**：修改代码，观察行为

### 推荐阅读

- 《Linux 多线程服务端编程》- 陈硕
- 《Unix 网络编程》- W. Richard Stevens
- muduo 源码 - https://github.com/chenshuo/muduo

---

## 9. HTTP 模块：如何处理粘包？

### 9.1 什么是粘包？

TCP 是字节流协议，没有消息边界：

```
客户端发送:
    请求1: "GET /a HTTP/1.1\r\n\r\n"
    请求2: "GET /b HTTP/1.1\r\n\r\n"

服务端可能收到:
    "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n"  (粘在一起)
    或
    "GET /a HTTP/1.1\r\n\r" + "\nGET /b HTTP/1.1\r\n\r\n"  (被拆开)
```

### 9.2 本项目的解决方案：先 peek，确认完整再消费

```cpp
ParseResult parseRequest(Buffer* buf, HttpRequest& request) {
    // 1. 先 peek，不消费数据
    const char* data = buf->peek();
    size_t len = buf->readableBytes();

    // 2. 找请求头结束位置
    const char* headerEnd = memmem(data, len, "\r\n\r\n", 4);
    if (!headerEnd) {
        return ParseResult::Incomplete;  // 请求头不完整
    }

    // 3. 解析 Content-Length
    size_t contentLen = request.contentLength();
    size_t totalLen = headerLen + contentLen;

    if (len < totalLen) {
        return ParseResult::Incomplete;  // 请求体不完整
    }

    // 4. 确认完整，才消费数据
    buf->retrieve(headerLen);
    request.body.assign(buf->peek(), contentLen);
    buf->retrieve(contentLen);

    return ParseResult::Complete;
}
```

**为什么先 peek？**

```
如果不 peek，直接读：
    读了一半发现数据不完整 → 已经消费了数据 → 无法恢复

peek 的好处：
    先检查是否完整 → 不完整就返回等待 → 完整了才消费
```

### 9.3 HTTP Pipeline 处理

```
一个 TCP 连接中可能包含多个请求：

while (buf->readableBytes() > 0) {
    ParseResult result = parseRequest(buf, request);

    if (result == Incomplete) {
        return;  // 等待更多数据
    }

    handleRequest(request, response);
    conn->send(response.toString());
    // 继续循环处理下一个请求
}
```

---

## 10. 连接池：为什么需要连接复用？

### 10.1 TCP 连接的开销

```
建立一次 TCP 连接:
1. 三次握手: 客户端 SYN → 服务端 SYN-ACK → 客户端 ACK
2. 时间: 本地 < 1ms，跨机房 10-100ms
3. 资源: 两端都要分配 socket 缓冲区

如果每次请求都新建连接:
    1000 QPS = 1000 次握手/秒 = 大量开销
```

### 10.2 连接池原理

```
┌─────────────────────────────────────────────────────────────┐
│                     ConnectionPool                           │
│                                                             │
│   空闲队列: [conn1] → [conn2] → [conn3] → ...              │
│              ↑                                              │
│           acquire() 取出                                    │
│              │                                              │
│              ▼                                              │
│   使用中:   [conn4] → [conn5]                               │
│              ↑                                              │
│           release() 归还                                    │
│              │                                              │
│              ▼                                              │
│   空闲队列: [conn4] → [conn5] → [conn1] → ...              │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 10.3 非阻塞 connect + 超时

**问题**：阻塞 connect 会卡住，如果服务器不响应，一直等待。

**解决方案**：非阻塞 connect + select 超时

```cpp
int createConnection(const string& host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    // 设置非阻塞
    fcntl(fd, F_SETFL, O_NONBLOCK);

    // 非阻塞 connect，立即返回
    connect(fd, ...);  // 返回 -1，errno = EINPROGRESS

    // 用 select 等待连接完成，带超时
    fd_set writefds;
    FD_SET(fd, &writefds);
    struct timeval tv = {5, 0};  // 5 秒超时

    int result = select(fd + 1, nullptr, &writefds, nullptr, &tv);
    if (result <= 0) {
        close(fd);
        return -1;  // 超时
    }

    // 检查连接是否成功
    int error;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, ...);
    if (error != 0) {
        close(fd);
        return -1;  // 连接失败
    }

    // 恢复阻塞模式（可选）
    fcntl(fd, F_SETFL, 0);

    return fd;
}
```

---

## 11. epoll：LT vs ET 模式

### 11.1 两种触发模式

| 模式 | 触发条件 | 特点 |
|------|----------|------|
| **LT (Level Trigger)** | 只要 fd 可读/可写，就触发 | 安全，不会丢事件 |
| **ET (Edge Trigger)** | fd 状态变化时才触发 | 高效，但需一次读完 |

### 11.2 代码对比

```cpp
// LT 模式（默认）
// 假设 socket 有 100 字节，读了 50 字节
// 下次 epoll_wait 仍然返回该 fd（因为还有 50 字节可读）

// ET 模式
// 假设 socket 有 100 字节，读了 50 字节
// 下次 epoll_wait 不返回（状态没变化）
// 必须循环读直到 EAGAIN
void handleReadET(int fd) {
    while (true) {
        int n = read(fd, buf, sizeof(buf));
        if (n == -1) {
            if (errno == EAGAIN) {
                break;  // 读完了
            }
            // 错误处理
        } else if (n == 0) {
            // 对端关闭
            break;
        }
        // 处理数据
    }
}
```

### 11.3 本项目选择：LT 模式

**原因**：

1. **实现简单**：不需要循环读
2. **更安全**：不会因为漏读而丢失事件
3. **性能足够**：对于大多数场景，LT 性能已经足够

---

## 12. epoll vs select vs poll

| 特性 | select | poll | epoll |
|------|--------|------|-------|
| 最大连接数 | 1024 (FD_SETSIZE) | 无限制 | 无限制 |
| 时间复杂度 | O(n) 每次遍历 | O(n) 每次遍历 | O(1) 只返回活跃 |
| 数据结构 | 位图 | 数组 | 红黑树 + 就绪链表 |
| 工作模式 | LT | LT | LT + ET |
| 跨平台 | 是 | 是 | 仅 Linux |

**为什么 epoll 更快？**

```
select/poll:
    每次调用都要:
    1. 把所有 fd 从用户态复制到内核态
    2. 遍历所有 fd 检查状态
    3. 把结果从内核态复制到用户态

    10000 个连接，只有 10 个活跃 → 也要遍历 10000 个

epoll:
    1. epoll_ctl: 只在添加/删除时复制
    2. epoll_wait: 只返回活跃的 fd
    3. 内核维护事件表，不需要每次传入

    10000 个连接，只有 10 个活跃 → 只处理 10 个
```

---

## 13. 惊群问题

### 13.1 什么是惊群？

```
多个进程/线程同时等待同一个 listenfd:

Thread 1 ──┐
Thread 2 ──┼──▶ epoll_wait(listenfd)
Thread 3 ──┘

新连接到来:
    所有线程都被唤醒
    但只有一个能 accept 成功
    其他线程白醒了一次
```

### 13.2 本项目的解决方案

**One Loop Per Thread 模型天然避免惊群**：

```
mainLoop: 只负责 accept
    epoll_wait(listenfd)
    有新连接 → accept → 分发给 subLoop

subLoop 1, 2, 3: 只处理已建立连接
    epoll_wait(connfds...)

每个 fd 只在一个线程监听，不会惊群
```

**如果多进程监听同一端口**：

- Linux 3.9+ 支持 `SO_REUSEPORT`，内核自动负载均衡
- 或者用锁保护 accept

---

## 14. 与其他网络库的设计对比

### 14.1 与 muduo 原版的区别

| 维度 | muduo 原版 | mymuduo-http |
|------|-----------|--------------|
| C++ 标准 | C++11 | C++17（结构化绑定、inline static） |
| 定时器 | 红黑树（std::set），O(log n) | 时间轮，O(1) |
| 日志 | 同步日志（LogStream） | 双缓冲异步日志（AsyncLogger） |
| HTTP | 简单 HTTP 解析 | 完整 HTTP/1.1（路由、中间件、静态文件） |
| RPC | 无内置 RPC | JSON-RPC 2.0 + Protobuf-RPC |
| WebSocket | 无 | 完整 RFC 6455 实现 |
| 服务发现 | 无 | 注册中心 + 健康检查 + 心跳 |
| 负载均衡 | 无 | 5 种策略（轮询/加权/最少连接/随机/一致性哈希） |
| 连接池 | 无 | TCP 连接池（min/max/超时/健康检查） |
| 头文件结构 | 头文件 + 实现文件分离 | 核心网络层分离，上层模块 header-only |

### 14.2 与 libevent 的设计差异

| 维度 | libevent | mymuduo-http |
|------|----------|--------------|
| 语言 | C | C++17 |
| 编程范式 | 回调函数指针 | std::function + lambda |
| 线程模型 | 单线程（需自行多线程化） | 内置 One Loop Per Thread |
| 内存管理 | 手动 malloc/free | RAII + shared_ptr |
| 跨平台 | 支持 select/poll/epoll/kqueue | 仅 Linux epoll |
| 定时器 | 最小堆 O(log n) | 时间轮 O(1) |
| HTTP | 内置简单 HTTP（evhttp） | 完整路由 + 中间件 |

**选择 epoll-only 的原因**：
- 项目定位为 Linux 高性能服务端，不需要跨平台
- 单一后端实现更简单，减少抽象层开销
- Poller 抽象接口保留了扩展到 kqueue 的可能性

### 14.3 与 Boost.Asio 的设计差异

| 维度 | Boost.Asio | mymuduo-http |
|------|-----------|--------------|
| 依赖 | Boost 库（重量级） | 仅依赖 Protobuf + OpenSSL + nlohmann/json |
| I/O 模型 | Proactor（异步 I/O） | Reactor（同步非阻塞 I/O） |
| 线程模型 | io_context + strand | EventLoop + Channel |
| 学习曲线 | 复杂（模板重、概念多） | 简单（API 直观） |
| 协程支持 | C++20 协程集成 | 无（可通过回调 + future 模拟） |

**选择 Reactor 而非 Proactor 的原因**：
- Linux 的 AIO 支持不完善（io_uring 是较新方案）
- Reactor + 非阻塞 I/O 在 Linux 上性能足够
- 实现更简单，调试更方便

### 14.4 设计取舍总结

本项目的设计哲学：**在够用的范围内追求简单**。不做跨平台、不做协程、不做 HTTP/2，但把 Reactor + 多线程 + 常用协议做到位。

---

## 总结

### 设计决策速查表

| 决策 | 选择 | 原因 |
|------|------|------|
| I/O 模型 | epoll | Linux 上最高效 |
| 并发模型 | One Loop Per Thread | 无锁并发，可扩展 |
| 定时器 | 时间轮 | O(1)，适合大量定时器 |
| 异步日志 | 双缓冲区 | 零拷贝交接，业务不阻塞 |
| 负载均衡 | 策略模式 | 易扩展，符合开闭原则 |
| 线程安全 | mutex + atomic | 简单可靠，够用 |
| 序列化 | JSON + Protobuf | 兼顾调试和性能 |
| epoll 模式 | LT | 简单安全，性能足够 |
| HTTP 粘包 | peek + 确认后消费 | 不丢数据，支持 pipeline |

### 学习建议

1. **先理解问题**：每个模块是为了解决什么问题
2. **再看解决方案**：为什么这样设计能解决问题
3. **对比其他方案**：为什么不选其他方案
4. **动手实践**：修改代码，观察行为