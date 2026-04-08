# 简历项目描述

---

## 基于 C++17 的仿 Muduo 网络库与微服务框架

**项目描述**：使用 C++17 实现仿 Muduo 网络库，基于多 Reactor 多线程模型实现网络库核心模块，并在此基础上构建 HTTP 服务器、双协议 RPC 框架、WebSocket 服务器和服务注册中心。项目总代码量 9,500+ 行（头文件 7,600+，实现文件 1,800+），包含 11 个功能模块、40 个头文件、8 个示例程序。

**主要工作**：

1. **核心网络层**：基于 Epoll LT 模式 + 非阻塞 I/O 实现 mainReactor + subReactor 多线程模型。采用 One Loop Per Thread 线程模型，每个线程独立运行 EventLoop，通过 eventfd 实现跨线程任务调度，连接通过 Round-Robin 分配到 subReactor。

2. **HTTP/1.1 服务器**：支持 GET/POST/PUT/DELETE 方法、正则表达式路由匹配（预编译）、中间件链、静态文件服务（自动 MIME 检测）。通过 Peek-Parse-Consume 三阶段状态机解决 TCP 粘包问题，支持 Keep-Alive 连接复用和 HTTP Pipeline。请求体上限 10MB，请求行上限 8KB，含路径穿越防护。

3. **双协议 RPC 框架**：实现 JSON-RPC 2.0（人类可读，调试方便）和 Protobuf-RPC（二进制高效，类型安全）。使用 4 字节网络字节序长度前缀解决 TCP 粘包，Protobuf-RPC 通过模板元编程实现编译期类型检查的服务注册。帧大小限制（服务端 10MB，客户端 64MB）防止恶意大包。

4. **WebSocket 服务器**：完整实现 RFC 6455 协议，包括 HTTP Upgrade 握手（SHA-1 + Base64 密钥验证）、二进制帧编解码（支持 3 种长度编码方式）、掩码 XOR 解码、Ping/Pong 心跳检测（默认 30s 间隔）、空闲连接超时清理。

5. **异步日志系统**：基于双缓冲区实现前后端分离的异步日志。前端线程写入 currentBuffer_，后台线程通过 swap 指针（O(1)）交接并批量刷盘。支持 5 个日志级别，缓冲区满（1000 条）或 100ms 超时自动触发刷盘，不阻塞业务线程。

6. **时间轮定时器**：采用时间轮（Timing Wheel）算法替代传统红黑树/最小堆，实现 O(1) 复杂度的定时器添加/取消。默认 60 个桶、1 秒 tick 间隔，覆盖 60 秒时间范围。通过哈希表辅助快速取消，使用 C++17 inline static + atomic 实现全局 ID 生成。

7. **服务注册与发现**：实现完整的服务注册中心，提供 6 个 REST API 端点（注册/注销/心跳/发现/列表/统计）。支持命名空间+服务名+版本三级标识、TTL 心跳过期检测、后台健康检查线程自动标记下线实例。客户端 SDK 支持自动心跳和 5 种负载均衡策略选择。

8. **负载均衡器**：实现 5 种策略——轮询、平滑加权轮询（Nginx 算法）、最少活跃连接、随机、一致性哈希（默认 150 虚拟节点）。通过策略模式（ILoadBalanceStrategy 接口）实现，符合开闭原则，易于扩展新策略。

9. **TCP 连接池**：支持 min/max 连接数控制（默认 5/20）、超时等待获取（默认 5s）、空闲连接健康检查清理。使用 mutex + condition_variable 实现线程安全的获取/归还。

10. **内存安全设计**：遵循 RAII 原则，TcpConnection 使用 shared_ptr 管理跨线程生命周期，Channel 通过 weak_ptr（tie 机制）避免循环引用，连接移除采用两阶段销毁保证回调期间对象不被提前析构。

---

## 技术要点

| 模块 | 技术点 |
|------|--------|
| I/O 复用 | Epoll LT 模式 + events_ 动态 2 倍扩容 |
| 线程模型 | mainReactor + subReactor + EventLoopThreadPool |
| 跨线程通信 | eventfd 唤醒 + swap 任务队列 + callingPendingFunctors 标志 |
| HTTP 服务器 | 正则路由预编译 + Peek-Parse-Consume 状态机 + Pipeline |
| RPC 框架 | JSON-RPC 2.0 + Protobuf-RPC + 长度前缀协议 |
| WebSocket | RFC 6455 握手 + 帧编解码 + Ping/Pong 心跳 |
| 异步日志 | 双缓冲区 swap 指针 + 条件变量 + 后端刷盘线程 |
| 定时器 | 时间轮 O(1) + atomic ID 生成 + 哈希表辅助取消 |
| 服务发现 | REST API + 心跳 TTL + 后台健康检查 |
| 负载均衡 | 策略模式 + 5 种算法（含平滑加权和一致性哈希） |
| Buffer | readv + 栈上 64KB 临时缓冲 + 移动/扩容双策略 |
| 内存管理 | shared_ptr + weak_ptr(tie) + 两阶段连接销毁 |

---

## 项目量化数据

| 指标 | 数据 |
|------|------|
| 总代码量 | 9,528 行（头文件 7,642 行 + 实现文件 1,886 行） |
| 模块数 | 11 个（net/http/rpc/websocket/registry/balancer/timer/pool/asynclogger/config/util） |
| 头文件数 | 40 个 |
| 示例程序 | 8 个 |
| 单元测试 | 3 个（负载均衡、注册中心、WebSocket 帧编解码） |
| C++ 标准 | C++17 |
| 外部依赖 | Protobuf + OpenSSL + nlohmann/json |

---

## 面试准备要点

### 高频追问方向

1. **TcpConnection 生命周期**：shared_ptr → weak_ptr(tie) → 两阶段销毁
2. **跨线程任务调度**：queueInLoop → swap → callingPendingFunctors_ → wakeup 判断
3. **HTTP 粘包解决**：Peek-Parse-Consume → Content-Length → Pipeline 循环
4. **RPC 粘包解决**：4 字节长度前缀 → ntohl/htonl → 帧大小限制
5. **Buffer readFd**：readv + 栈上 64KB → 避免预分配又保证单次读取量
6. **双缓冲日志**：swap 指针 O(1) → 锁外执行写文件 → wait_for 超时保底
7. **时间轮 vs 红黑树**：O(1) vs O(log n) → 精度/范围局限 → 适用场景