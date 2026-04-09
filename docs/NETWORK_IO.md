# 网络 I/O 完全指南

> 面向面试的网络 I/O 详解，从底层系统调用到 Reactor 架构，配合 mymuduo-http 项目源码。

---

## 目录

- [一、网络 I/O 基础](#一网络-io-基础)
- [二、I/O 多路复用详解](#二io-多路复用详解)
- [三、Reactor 模式与线程模型](#三reactor-模式与线程模型)
- [四、非阻塞 I/O 与 Buffer 设计](#四非阻塞-io-与-buffer-设计)
- [五、TCP 连接管理](#五tcp-连接管理)
- [六、高性能网络编程实践](#六高性能网络编程实践)
- [七、面试高频问题速查](#七面试高频问题速查)

---

# 一、网络 I/O 基础

## 1.1 用户态与内核态

```
┌─────────────────────────────────────┐
│          用户空间 (User Space)        │
│  ┌────────┐  ┌────────┐  ┌────────┐ │
│  │ 进程 A  │  │ 进程 B  │  │ 进程 C  │ │
│  └───┬────┘  └───┬────┘  └───┬────┘ │
├──────┼───────────┼───────────┼──────┤  ← 系统调用边界
│          内核空间 (Kernel Space)       │
│  ┌────────────────────────────────┐  │
│  │    TCP/IP 协议栈                 │  │
│  │  ┌──────────┐ ┌──────────────┐ │  │
│  │  │ Socket   │ │ 内核缓冲区     │ │  │
│  │  │ 缓冲区    │ │ (sk_buff)    │ │  │
│  │  └──────────┘ └──────────────┘ │  │
│  └────────────────────────────────┘  │
│  ┌────────────────────────────────┐  │
│  │    网卡驱动 (NIC Driver)         │  │
│  └────────────────────────────────┘  │
└─────────────────────────────────────┘
```

**一次网络读操作的完整流程**：

1. 数据到达网卡 → DMA 拷贝到内核缓冲区
2. 内核协议栈处理 → 放入 Socket 接收缓冲区
3. 用户调用 `read()` → 数据从内核缓冲区拷贝到用户缓冲区

**两个等待阶段**：
- **阶段 1**：等待数据到达内核缓冲区（网络延迟）
- **阶段 2**：从内核缓冲区拷贝到用户缓冲区（CPU 拷贝）

五种 I/O 模型的区别就在于：**这两个阶段是否阻塞**。

---

## 1.2 五种 I/O 模型

### 阻塞 I/O（Blocking I/O）

```
应用进程                          内核
    │                              │
    │───── read(fd, buf) ────────▶│
    │         (阻塞等待)            │  等待数据到达...
    │                              │  数据到达！
    │                              │  拷贝数据到用户空间
    │◀──── 返回数据 ────────────── │
    │
```

```cpp
// 最简单的模型，但一个线程只能处理一个连接
char buf[1024];
int n = read(fd, buf, sizeof(buf));  // 阻塞，直到有数据
```

**缺点**：一个线程只能处理一个 fd，要处理 1 万个连接就需要 1 万个线程。

### 非阻塞 I/O（Non-blocking I/O）

```
应用进程                          内核
    │                              │
    │───── read(fd, buf) ────────▶│  数据未到达
    │◀──── EAGAIN/EWOULDBLOCK ────│
    │                              │
    │───── read(fd, buf) ────────▶│  数据未到达
    │◀──── EAGAIN/EWOULDBLOCK ────│
    │                              │
    │───── read(fd, buf) ────────▶│  数据到达！
    │                              │  拷贝数据到用户空间
    │◀──── 返回数据 ────────────── │
```

```cpp
// 设置非阻塞
fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

// 或者创建时直接指定（项目中的做法）
int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

// 非阻塞读
while (true) {
    int n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        // 有数据，处理
    } else if (n == -1 && errno == EAGAIN) {
        // 暂无数据，稍后重试
        break;
    } else if (n == 0) {
        // 对端关闭
        break;
    }
}
```

**缺点**：忙轮询（busy polling），浪费 CPU。

### I/O 多路复用（I/O Multiplexing）— 本项目使用

```
应用进程                          内核
    │                              │
    │── epoll_wait(epfd) ────────▶│  监听多个 fd
    │       (阻塞等待)              │
    │                              │  fd3 数据到达！
    │◀── 返回就绪的 fd 列表 ────── │
    │                              │
    │── read(fd3, buf) ──────────▶│  拷贝数据
    │◀── 返回数据 ────────────────│
```

```cpp
// 一个线程监听多个 fd
int nfds = epoll_wait(epfd, events, maxEvents, timeout);
for (int i = 0; i < nfds; i++) {
    if (events[i].events & EPOLLIN) {
        read(events[i].data.fd, buf, sizeof(buf));
    }
}
```

**优势**：一个线程可以监听成千上万个连接。

### 信号驱动 I/O（Signal-driven I/O）

```
应用进程                          内核
    │                              │
    │── sigaction(SIGIO) ────────▶│  注册信号
    │◀── 立即返回 ────────────────│
    │  (继续干其他事)               │
    │                              │  数据到达！
    │◀── SIGIO 信号 ──────────────│
    │── read(fd, buf) ──────────▶│  拷贝数据
    │◀── 返回数据 ────────────────│
```

**缺点**：信号处理复杂，难以扩展到多个 fd，实践中很少使用。

### 异步 I/O（Asynchronous I/O）

```
应用进程                          内核
    │                              │
    │── aio_read(fd, buf) ───────▶│  注册异步读操作
    │◀── 立即返回 ────────────────│
    │  (继续干其他事)               │  等待数据...
    │                              │  数据到达！
    │                              │  拷贝到用户空间
    │◀── 信号/回调通知 ────────── │  两个阶段都完成了
```

**特点**：两个阶段都不阻塞，是真正的异步。但 Linux 上 AIO 支持不成熟（`io_uring` 是新方案）。

### 五种模型对比

| 模型 | 阶段1(等待数据) | 阶段2(拷贝数据) | 编程复杂度 | 适用场景 |
|------|---------------|---------------|-----------|---------|
| 阻塞 I/O | 阻塞 | 阻塞 | 最简单 | 连接数少 |
| 非阻塞 I/O | 非阻塞(轮询) | 阻塞 | 中等 | 配合多路复用 |
| I/O 多路复用 | 阻塞(在select/epoll) | 阻塞 | 较复杂 | 高并发服务器 |
| 信号驱动 | 非阻塞(信号) | 阻塞 | 复杂 | 很少使用 |
| 异步 I/O | 非阻塞 | 非阻塞 | 最复杂 | 理想模型 |

> **面试关键**：前四种都是**同步 I/O**（阶段 2 都会阻塞），只有异步 I/O 两个阶段都不阻塞。

---

## 1.3 文件描述符（File Descriptor）

Linux 中一切皆文件。Socket 也是文件描述符。

```
进程文件描述符表:
┌────┬──────────────────┐
│ fd │      指向          │
├────┼──────────────────┤
│  0 │ stdin             │
│  1 │ stdout            │
│  2 │ stderr            │
│  3 │ listen socket     │
│  4 │ client socket 1   │
│  5 │ client socket 2   │
│  6 │ eventfd (wakeup)  │
│  7 │ epollfd           │
│ .. │ ...               │
└────┴──────────────────┘
```

**关键系统限制**：

```bash
# 查看进程 fd 上限
ulimit -n       # 默认通常是 1024

# 修改
ulimit -n 65535

# 查看系统级上限
cat /proc/sys/fs/file-max
```

**EMFILE 问题**：当 fd 用完时 `accept()` 返回 -1，errno = EMFILE。

```cpp
// Acceptor.cc 中的处理
if (errno == EMFILE) {
    LOG_ERROR("sockfd reached upper limit");
}
```

---

## 1.4 Socket 编程基础

### TCP 通信流程

```
        服务端                          客户端
          │                              │
    socket()                        socket()
          │                              │
     bind()                              │
          │                              │
    listen()                             │
          │                              │
    accept() ◄─── 三次握手 ──── connect()
          │                              │
     read() ◄──── 数据传输 ──── write()
     write() ────▶ 数据传输 ────▶ read()
          │                              │
    close() ◄─── 四次挥手 ──── close()
```

### 关键系统调用

```cpp
// 1. 创建 socket（项目中的非阻塞版本）
int sockfd = socket(AF_INET,
    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
// AF_INET:     IPv4
// SOCK_STREAM: TCP（字节流）
// SOCK_NONBLOCK: 非阻塞
// SOCK_CLOEXEC: exec 时自动关闭，防止 fd 泄漏

// 2. 绑定地址
struct sockaddr_in addr;
addr.sin_family = AF_INET;
addr.sin_port = htons(8080);
addr.sin_addr.s_addr = htonl(INADDR_ANY);
bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));

// 3. 监听
listen(sockfd, SOMAXCONN);  // backlog = SOMAXCONN (128/4096)

// 4. 接受连接
struct sockaddr_in peer;
socklen_t len = sizeof(peer);
int connfd = accept(sockfd, (struct sockaddr*)&peer, &len);

// 5. 读写
ssize_t n = read(connfd, buf, sizeof(buf));
ssize_t n = write(connfd, buf, len);

// 6. 关闭
close(connfd);
shutdown(connfd, SHUT_WR);  // 半关闭（只关写端）
```

### 重要的 Socket 选项

```cpp
// SO_REUSEADDR — 允许重用 TIME_WAIT 状态的地址
// 场景：服务器重启时端口还在 TIME_WAIT，不设置会 bind 失败
int optval = 1;
setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

// SO_REUSEPORT — 多进程/线程共享同一端口（Linux 3.9+）
// 场景：多个进程监听同一端口，内核负载均衡分发
setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

// TCP_NODELAY — 禁用 Nagle 算法
// Nagle: 小数据包合并发送，减少网络包数量
// 禁用场景: 低延迟要求（如实时通信、RPC）
setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

// SO_KEEPALIVE — 启用 TCP 心跳
// 内核定期发送探测包，检测死连接
setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));

// SO_LINGER — 控制 close() 行为
struct linger lg;
lg.l_onoff = 1;
lg.l_linger = 0;  // close 时直接 RST，不走四次挥手
setsockopt(sockfd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));

// SO_SNDBUF / SO_RCVBUF — 设置发送/接收缓冲区大小
int bufsize = 256 * 1024;  // 256KB
setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
```

**项目中的 Socket 选项设置**（`Socket.cc`）：

```cpp
// Acceptor 创建时设置
acceptSocket_.setReuseAddr(true);    // 允许地址重用
acceptSocket_.setReusePort(reuseport); // 可选端口重用

// TcpConnection 创建时设置
socket_->setKeepAlive(true);  // 启用 TCP 心跳检测
```

---

## 1.5 TCP 三次握手与四次挥手

### 三次握手

```
客户端                                 服务端
  │                                      │
  │─── SYN (seq=x) ────────────────────▶│  SYN_SENT
  │                                      │  SYN_RCVD
  │◀── SYN+ACK (seq=y, ack=x+1) ───────│
  │                                      │
  │─── ACK (ack=y+1) ──────────────────▶│  ESTABLISHED
  │  ESTABLISHED                         │
```

**为什么需要三次？**
- 第一次：客户端确认自己能发
- 第二次：服务端确认自己能收能发，客户端确认自己能收
- 第三次：服务端确认自己能收

> 两次不行：如果旧的 SYN 延迟到达，服务端会误以为是新连接。

### 四次挥手

```
主动关闭方                             被动关闭方
  │                                      │
  │─── FIN (seq=u) ────────────────────▶│  FIN_WAIT_1
  │                                      │  CLOSE_WAIT
  │◀── ACK (ack=u+1) ──────────────────│
  │  FIN_WAIT_2                          │
  │                                      │  (可能还有数据要发)
  │◀── FIN (seq=v) ────────────────────│  LAST_ACK
  │  TIME_WAIT                           │
  │─── ACK (ack=v+1) ──────────────────▶│  CLOSED
  │                                      │
  │  等待 2MSL ...                       │
  │  CLOSED                              │
```

**为什么需要四次？**
- TCP 是全双工的，关闭是单向的
- 收到 FIN 只是对方不发了，自己可能还有数据要发
- 所以 ACK 和 FIN 分开发（不像握手可以合并 SYN+ACK）

**TIME_WAIT 状态**：
- 持续 2MSL（通常 60 秒）
- 目的 1：确保最后的 ACK 到达（丢失可以重传）
- 目的 2：让旧连接的延迟数据包在网络中消失
- **问题**：大量 TIME_WAIT 占用端口 → 解决：`SO_REUSEADDR`

**项目中的优雅关闭**：

```cpp
// TcpConnection::shutdown() — 半关闭
void TcpConnection::shutdownInLoop() {
    if (!channel_->isWriting()) {  // 确保数据发完
        socket_->shutdownWrite();  // 只关写端，还能读
    }
}
```

---

## 1.6 TCP 状态机

```
                              ┌──────────┐
                   主动打开    │  CLOSED  │    被动打开
              ┌───────────── │          │ ──────────────┐
              │  SYN 发送     └──────────┘  收到 SYN     │
              ▼                                          ▼
        ┌──────────┐                            ┌──────────┐
        │ SYN_SENT │                            │  LISTEN  │
        └────┬─────┘                            └────┬─────┘
             │ 收到 SYN+ACK                          │ 收到 SYN
             │ 发送 ACK                              │ 发送 SYN+ACK
             ▼                                       ▼
        ┌──────────┐                            ┌──────────┐
        │ESTABLISHED│◀───────────────────────── │ SYN_RCVD │
        └────┬─────┘     收到 ACK               └──────────┘
             │
     ┌───────┴───────┐
     │主动关闭         │被动关闭
     │发送 FIN        │收到 FIN
     ▼               ▼
┌──────────┐   ┌──────────┐
│FIN_WAIT_1│   │CLOSE_WAIT│
└────┬─────┘   └────┬─────┘
     │收到 ACK       │发送 FIN
     ▼               ▼
┌──────────┐   ┌──────────┐
│FIN_WAIT_2│   │ LAST_ACK │
└────┬─────┘   └────┬─────┘
     │收到 FIN       │收到 ACK
     │发送 ACK       │
     ▼               ▼
┌──────────┐   ┌──────────┐
│TIME_WAIT │   │  CLOSED  │
└────┬─────┘   └──────────┘
     │ 2MSL 超时
     ▼
┌──────────┐
│  CLOSED  │
└──────────┘
```

---

# 二、I/O 多路复用详解

## 2.1 select

```cpp
#include <sys/select.h>

fd_set readfds;
FD_ZERO(&readfds);
FD_SET(sockfd, &readfds);

struct timeval timeout = {5, 0};  // 5 秒超时

int nready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);

for (int fd = 0; fd <= maxfd; fd++) {
    if (FD_ISSET(fd, &readfds)) {
        // fd 可读
    }
}
```

**实现原理**：

```
用户态                              内核态
┌──────────┐                   ┌──────────┐
│ fd_set   │ ── 拷贝到内核 ──▶ │ fd_set   │
│ [bitmap] │                   │          │
│ 1024 bit │                   │ 遍历每个  │
└──────────┘                   │ bit 检查  │
                               │ fd 状态   │
┌──────────┐                   │          │
│ fd_set   │ ◀── 拷贝回用户 ── │ 标记就绪  │
│ [修改后]  │                   └──────────┘
└──────────┘
```

**缺点**：
1. **fd 上限 1024**：`FD_SETSIZE` 固定为 1024
2. **每次调用都要拷贝**：用户态 → 内核态 → 用户态
3. **O(n) 遍历**：返回后需要遍历所有 fd 找到就绪的
4. **fd_set 被修改**：每次调用前都要重新设置

---

## 2.2 poll

```cpp
#include <poll.h>

struct pollfd fds[1024];
fds[0].fd = sockfd;
fds[0].events = POLLIN;  // 关注可读
// ... 设置其他 fd

int nready = poll(fds, nfds, 5000);  // 5000ms 超时

for (int i = 0; i < nfds; i++) {
    if (fds[i].revents & POLLIN) {
        // fds[i].fd 可读
    }
}
```

**相比 select 的改进**：
- 没有 1024 的 fd 上限（用链表存储）
- `events` 和 `revents` 分开，不需要每次重设

**仍然存在的问题**：
- 每次调用仍要把整个 `pollfd` 数组拷贝到内核
- 返回后仍需 O(n) 遍历

---

## 2.3 epoll（重点）

### 三个核心系统调用

```cpp
#include <sys/epoll.h>

// 1. 创建 epoll 实例
int epfd = epoll_create1(EPOLL_CLOEXEC);
// 返回一个 fd，内核中创建一个 eventpoll 结构

// 2. 注册/修改/删除 fd
struct epoll_event ev;
ev.events = EPOLLIN;            // 关注可读
ev.data.ptr = channel;          // 关联自定义数据
epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);  // 注册
epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);  // 修改
epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev);  // 删除

// 3. 等待事件
struct epoll_event events[1024];
int nready = epoll_wait(epfd, events, 1024, timeout_ms);
// 只返回就绪的 fd，不需要遍历所有 fd
```

### 内核实现原理

```
epoll 内核数据结构:

                    eventpoll
                   ┌──────────────────┐
                   │  rbr (红黑树)      │ ← 存储所有监控的 fd
                   │  ┌─────┐         │
                   │  │ fd3 │         │
                   │  ├─────┤         │
                   │  │ fd5 │ ...     │
                   │  └─────┘         │
                   │                  │
                   │  rdllist (就绪链表) │ ← 只存放就绪的 fd
                   │  ┌────┐┌────┐   │
                   │  │fd3 ││fd7 │   │
                   │  └────┘└────┘   │
                   └──────────────────┘

工作流程:
1. epoll_ctl(ADD): 在红黑树中插入节点，注册回调函数
2. 当 fd 就绪时: 内核回调函数将节点加入就绪链表
3. epoll_wait: 检查就绪链表，有就绪事件直接返回
                没有就绪事件则阻塞
```

### LT vs ET 模式

```cpp
// LT (Level Triggered) — 水平触发（默认模式，本项目使用）
ev.events = EPOLLIN;

// ET (Edge Triggered) — 边沿触发
ev.events = EPOLLIN | EPOLLET;
```

**LT 模式**：

```
缓冲区状态:  [空] → [有数据] → [还有数据] → [还有数据] → [空]
epoll 通知:         ✓ 通知     ✓ 通知       ✓ 通知
                    (只要有数据，每次 epoll_wait 都返回)
```

```cpp
// LT 模式：正常读即可
void handleRead_LT(int fd) {
    int n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        process(buf, n);
    }
    // 如果没读完，下次 epoll_wait 还会通知
}
```

**ET 模式**：

```
缓冲区状态:  [空] → [有数据] → [还有数据] → [还有数据] → [空]
epoll 通知:         ✓ 通知     ✗ 不通知     ✗ 不通知
                    (只在状态变化时通知一次！)
```

```cpp
// ET 模式：必须循环读完所有数据
void handleRead_ET(int fd) {
    while (true) {
        int n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            process(buf, n);
        } else if (n == -1) {
            if (errno == EAGAIN) break;  // 数据读完了
            // 其他错误处理
            break;
        } else {  // n == 0
            // 对端关闭
            break;
        }
    }
}
```

**对比**：

| 特性 | LT（本项目） | ET |
|------|-------------|-----|
| 触发条件 | 只要可读就触发 | 状态变化才触发 |
| 编程难度 | 简单 | 复杂（必须循环读/写） |
| 漏事件风险 | 不会 | 不循环读就会漏 |
| 系统调用次数 | 稍多 | 稍少 |
| 实际性能差距 | 几乎没有 | 几乎没有 |
| 使用者 | muduo、本项目 | Nginx、libevent |

**项目选择 LT 的理由**：
1. 更安全，不会漏事件
2. 编程简单，减少 bug
3. 性能瓶颈通常在业务逻辑，不在 LT/ET
4. muduo 原版也用 LT

### 项目中的 epoll 实现

```cpp
// EPollPoller.cc — 创建 epoll
EPollPoller::EPollPoller(EventLoop* loop)
    : Poller(loop)
    , epollfd_(::epoll_create1(EPOLL_CLOEXEC))  // CLOEXEC 防止 fd 泄漏
    , events_(kInitEventListSize)  // 初始 16 个事件槽
{}

// EPollPoller.cc — 等待事件
Timestamp EPollPoller::poll(int timeoutMs, ChannelList* activeChannels) {
    int numEvents = ::epoll_wait(epollfd_,
                                 &*events_.begin(),
                                 static_cast<int>(events_.size()),
                                 timeoutMs);

    if (numEvents > 0) {
        fillActiveChannels(numEvents, activeChannels);

        // 动态扩容：事件数等于容量时翻倍
        if (static_cast<size_t>(numEvents) == events_.size()) {
            events_.resize(events_.size() * 2);
        }
    }
    return Timestamp::now();
}

// EPollPoller.cc — 注册/修改 fd
void EPollPoller::update(int operation, Channel* channel) {
    epoll_event event;
    ::memset(&event, 0, sizeof event);
    event.events = channel->events();
    event.data.ptr = channel;  // 关键：用 data.ptr 关联 Channel 对象
    int fd = channel->fd();

    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0) {
        if (operation == EPOLL_CTL_DEL) {
            LOG_ERROR("epoll_ctl del error:%d\n", errno);
        } else {
            LOG_FATAL("epoll_ctl add/mod error:%d\n", errno);
        }
    }
}
```

---

## 2.4 三种多路复用对比

| 特性 | select | poll | epoll |
|------|--------|------|-------|
| **数据结构** | bitmap (fd_set) | 数组 (pollfd) | 红黑树 + 就绪链表 |
| **fd 上限** | 1024 (FD_SETSIZE) | 无限制 | 无限制 |
| **fd 传递** | 每次拷贝整个 fd_set | 每次拷贝整个数组 | epoll_ctl 单个操作 |
| **就绪检测** | O(n) 遍历 | O(n) 遍历 | O(1) 回调驱动 |
| **返回方式** | 修改传入的 fd_set | 设置 revents 字段 | 只返回就绪的 events |
| **触发模式** | LT | LT | LT / ET |
| **适用场景** | fd 少，跨平台 | fd 少，简单替代 select | 高并发 Linux 服务器 |

**为什么 epoll 高效？**

1. **红黑树管理 fd**：`epoll_ctl` 是 O(log n)，但只在增删时调用
2. **回调驱动**：内核在数据到达时直接把 fd 加入就绪链表，不需要遍历
3. **零拷贝 fd 集合**：fd 注册一次，不需要每次 wait 都传递
4. **只返回就绪 fd**：不需要用户遍历所有 fd

---

## 2.5 epoll 事件类型

```cpp
// 读相关
EPOLLIN      // 有数据可读（普通数据或连接到来）
EPOLLPRI     // 有紧急数据可读（带外数据 OOB）
EPOLLRDHUP   // 对端关闭连接或关闭写端 (Linux 2.6.17+)

// 写相关
EPOLLOUT     // 可以写数据（发送缓冲区有空间）

// 错误/挂起
EPOLLERR     // 发生错误
EPOLLHUP     // 挂起（读写都关闭了）

// 控制标志
EPOLLET      // 边沿触发模式
EPOLLONESHOT // 只触发一次，之后需要重新 MOD 才能继续监听
```

**项目中的事件处理** — `Channel::handleEventWithGuard()`：

```cpp
void Channel::handleEventWithGuard(Timestamp receiveTime) {
    // 1. 挂起且无数据 → 关闭
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        if (closeCallback_) closeCallback_();
    }

    // 2. 错误
    if (revents_ & EPOLLERR) {
        if (errorCallback_) errorCallback_();
    }

    // 3. 可读（数据到达 / 新连接到来 / 紧急数据）
    if (revents_ & (EPOLLIN | EPOLLPRI)) {
        if (readCallback_) readCallback_(receiveTime);
    }

    // 4. 可写（发送缓冲区有空间）
    if (revents_ & EPOLLOUT) {
        if (writeCallback_) writeCallback_();
    }
}
```

---

# 三、Reactor 模式与线程模型

## 3.1 什么是 Reactor 模式

Reactor = **I/O 多路复用** + **非阻塞 I/O** + **事件回调**

```
               Reactor 模式
┌──────────────────────────────────────┐
│              EventLoop               │
│                                      │
│    ┌──────────┐    ┌──────────────┐ │
│    │  Poller   │───▶│ ChannelList  │ │
│    │ (epoll)   │    │ [就绪的fd们]  │ │
│    └──────────┘    └──────┬───────┘ │
│                           │         │
│                    handleEvent()    │
│                           │         │
│              ┌────────────┼────┐    │
│              ▼            ▼    ▼    │
│          onRead()   onWrite() ...  │
│              │            │         │
│              ▼            ▼         │
│         用户回调     用户回调        │
└──────────────────────────────────────┘
```

**事件循环核心代码**：

```cpp
// EventLoop.cc — loop() 方法
void EventLoop::loop() {
    looping_ = true;
    quit_ = false;

    while (!quit_) {
        activeChannels_.clear();

        // 步骤 1：等待 I/O 事件
        pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);

        // 步骤 2：分发事件到各 Channel 的回调
        for (Channel *channel : activeChannels_) {
            channel->handleEvent(pollReturnTime_);
        }

        // 步骤 3：执行跨线程调度的回调
        doPendingFunctors();
    }
}
```

---

## 3.2 单 Reactor 单线程

```
┌──────────────────────────────────────┐
│            单线程 Reactor              │
│                                      │
│  EventLoop (mainLoop)                │
│  ┌──────────┐                        │
│  │  Poller   │                        │
│  │ (epoll)   │                        │
│  └──────────┘                        │
│       │                              │
│       ├── listenfd → accept          │
│       ├── connfd1  → read/write     │
│       ├── connfd2  → read/write     │
│       └── connfd3  → read/write     │
└──────────────────────────────────────┘
```

**缺点**：accept 和业务处理在同一线程，一个慢请求会阻塞所有连接。

---

## 3.3 单 Reactor 多线程

```
┌──────────────────────────────────────┐
│          单 Reactor 多线程             │
│                                      │
│  EventLoop (mainLoop)                │
│  ┌──────────┐                        │
│  │  Poller   │                        │
│  └──────────┘                        │
│       │                              │
│       ├── listenfd → accept          │
│       ├── connfd1  → read → ┐       │
│       ├── connfd2  → read → ┼→ 线程池│
│       └── connfd3  → read → ┘       │
└──────────────────────────────────────┘
```

**缺点**：Reactor 只有一个，成为瓶颈。

---

## 3.4 多 Reactor 多线程（本项目采用）

```
┌────────────────────────────────────────────────┐
│               多 Reactor 多线程                  │
│                                                │
│  mainReactor (baseLoop) — 只负责 accept         │
│  ┌──────────┐                                  │
│  │  Poller   │─── listenfd → accept            │
│  └──────────┘         │                        │
│                       │ 轮询分发新连接            │
│         ┌─────────────┼──────────────┐         │
│         ▼             ▼              ▼         │
│    subReactor    subReactor     subReactor      │
│   (IO Thread1)  (IO Thread2)  (IO Thread3)     │
│   ┌──────────┐  ┌──────────┐  ┌──────────┐    │
│   │  Poller   │  │  Poller   │  │  Poller   │    │
│   └──────────┘  └──────────┘  └──────────┘    │
│   [conn1,conn4] [conn2,conn5] [conn3,conn6]   │
│                                                │
└────────────────────────────────────────────────┘
```

**优势**：
1. mainReactor 只做 accept，不被业务阻塞
2. 每个 subReactor 独立处理各自的连接，无锁
3. 水平扩展：增加线程就能提升吞吐

### 新连接分发流程

```cpp
// TcpServer.cc — 新连接到来
void TcpServer::newConnection(int sockfd, const InetAddress &peerAddr) {
    // 1. 轮询选择一个 subLoop
    EventLoop *ioLoop = threadPool_->getNextLoop();

    // 2. 创建 TcpConnection（属于 ioLoop 线程）
    auto conn = std::make_shared<TcpConnection>(
        ioLoop, connName, sockfd, localAddr, peerAddr);

    // 3. 设置回调
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setCloseCallback(
        std::bind(&TcpServer::removeConnection, this, std::placeholders::_1));

    // 4. 在 subLoop 线程中初始化连接
    ioLoop->runInLoop(
        std::bind(&TcpConnection::connectEstablished, conn));
}

// EventLoopThreadPool.cc — 轮询分发
EventLoop* EventLoopThreadPool::getNextLoop() {
    EventLoop* loop = baseLoop_;  // 默认用 mainLoop
    if (!loops_.empty()) {
        loop = loops_[next_];
        ++next_;
        if (next_ >= loops_.size()) {
            next_ = 0;  // 轮询
        }
    }
    return loop;
}
```

---

## 3.5 跨线程任务调度

**问题**：mainReactor 在线程 A，subReactor 在线程 B。A 怎么让 B 执行一个任务？

**方案**：`eventfd` + 任务队列

```
线程 A (mainLoop)              线程 B (subLoop)
      │                              │
      │ queueInLoop(task)            │
      │      │                       │
      │      ▼                       │
      │ pendingFunctors_.push(task)  │
      │      │                       │
      │      ▼                       │
      │ write(wakeupFd_, 1)  ───────▶│ epoll_wait 返回
      │                              │      │
      │                              │      ▼
      │                              │ handleRead() — 读 wakeupFd_
      │                              │      │
      │                              │      ▼
      │                              │ doPendingFunctors()
      │                              │   swap + 执行所有 task
```

**eventfd 比 pipe 好在哪？**
- 只需要一个 fd（pipe 需要两个）
- 语义更清晰（计数器语义）
- 更轻量

**关键实现**：

```cpp
// EventLoop.cc — 创建 wakeup 机制
int createEventfd() {
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return evtfd;
}

// EventLoop.cc — 跨线程调度
void EventLoop::queueInLoop(Functor cb) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(std::move(cb));
    }
    // 需要唤醒的两种情况：
    // 1. 不在当前线程（跨线程调用）
    // 2. 正在执行 doPendingFunctors（新加的 task 需要下轮执行）
    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();
    }
}

// EventLoop.cc — 高效执行回调
void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);  // swap 而不是拷贝！
    }
    // 锁外执行，minimiz锁持有时间
    for (const Functor &functor : functors) {
        functor();
    }
    callingPendingFunctors_ = false;
}
```

**swap 而不是拷贝的精妙之处**：
- 锁持有时间极短（只做一次指针交换）
- 执行回调时不持锁，其他线程可以继续加任务
- 避免死锁（回调中可能再次 queueInLoop）

---

## 3.6 One Loop Per Thread

**核心原则**：每个线程最多拥有一个 EventLoop，每个 EventLoop 只在创建它的线程中运行。

```cpp
// EventLoop.cc — 线程局部存储强制执行
__thread EventLoop *t_loopInThisThread = nullptr;

EventLoop::EventLoop() : threadId_(CurrentThread::tid()) {
    if (t_loopInThisThread) {
        LOG_FATAL("Another EventLoop exists in this thread %d\n", threadId_);
    }
    t_loopInThisThread = this;
}

// 判断是否在正确的线程
bool isInLoopThread() const {
    return threadId_ == CurrentThread::tid();
}
```

**线程模型图**：

```
Thread 1 (main)           Thread 2              Thread 3
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│ EventLoop    │    │ EventLoop    │    │ EventLoop    │
│ (baseLoop)   │    │ (subLoop1)   │    │ (subLoop2)   │
│              │    │              │    │              │
│ - Acceptor   │    │ - conn1      │    │ - conn3      │
│ - wakeupFd   │    │ - conn2      │    │ - conn4      │
│ - Poller     │    │ - wakeupFd   │    │ - wakeupFd   │
│              │    │ - Poller     │    │ - Poller     │
└──────────────┘    └──────────────┘    └──────────────┘
```

**EventLoopThread — 在新线程中创建 EventLoop**：

```cpp
// EventLoopThread.cc — 工作线程主函数
void EventLoopThread::threadFunc() {
    EventLoop loop;  // 在新线程栈上创建 EventLoop

    if (callback_) {
        callback_(&loop);  // 用户自定义初始化
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();  // 通知主线程：loop 已就绪
    }

    loop.loop();  // 开始事件循环，阻塞在此
}

// EventLoopThread.cc — 主线程等待 loop 就绪
EventLoop* EventLoopThread::startLoop() {
    thread_.start();

    EventLoop* loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (loop_ == nullptr) {
            cond_.wait(lock);  // 等待工作线程创建好 loop
        }
        loop = loop_;
    }
    return loop;
}
```

---

# 四、非阻塞 I/O 与 Buffer 设计

## 4.1 为什么需要应用层 Buffer

非阻塞 I/O 下，`read()` 和 `write()` 可能读/写不完整：

```
场景 1：读不完整
  - 内核缓冲区有 100 字节，但一条消息是 200 字节
  - 需要应用层缓存，等数据攒够再处理

场景 2：写不完整
  - 应用要发 1MB 数据，内核发送缓冲区只有 64KB
  - 剩余数据需要应用层缓存，等 EPOLLOUT 再继续发

场景 3：数据到达但业务层还没准备好处理
  - 先存到 Buffer，等业务层 ready 再取
```

---

## 4.2 Buffer 结构设计

```
+-------------------+------------------+------------------+
| prependable bytes |  readable bytes  | writable bytes   |
|    (已读/头部预留)   |    (待处理数据)   |    (可写空间)     |
+-------------------+------------------+------------------+
|                   |                  |                  |
0      <=      readerIndex   <=   writerIndex    <=     size

                8 bytes            初始 1024 bytes
             (kCheapPrepend)        (kInitialSize)
```

**三个区域**：
- **prependable**：已读区域 + 预留头部（8 字节），可以在数据前插入协议头
- **readable**：待处理的数据
- **writable**：空闲空间，可以写入新数据

**空间回收策略**：

```cpp
void Buffer::makeSpace(size_t len) {
    if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
        // 总空间不够 → 扩容
        buffer_.resize(writerIndex_ + len);
    } else {
        // 总空间够，但 writable 不够
        // 把 readable 移到前面，回收 prependable 空间
        size_t readable = readableBytes();
        std::copy(begin() + readerIndex_,
                  begin() + writerIndex_,
                  begin() + kCheapPrepend);
        readerIndex_ = kCheapPrepend;
        writerIndex_ = readerIndex_ + readable;
    }
}
```

```
扩容前:
[=====已读=====|==可读==|=可写=]  可写不够！
               ↑       ↑
          readerIndex writerIndex

空间回收后:
[prepend|==可读==|=======可写========]  够了！
        ↑       ↑
   readerIndex writerIndex
```

---

## 4.3 readv — 高效读取

**问题**：Buffer 的 writable 空间可能不够，但预先分配太大又浪费内存。

**方案**：`readv()` 分散读取，同时读入 Buffer 和栈上临时空间。

```cpp
// Buffer.cc — readFd
ssize_t Buffer::readFd(int fd, int* saveErrno) {
    char extrabuf[65536] = {0};  // 64KB 栈上临时缓冲区

    struct iovec vec[2];
    const size_t writable = writableBytes();

    // iovec[0]: Buffer 的可写区域
    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len = writable;

    // iovec[1]: 栈上缓冲区（溢出兜底）
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    // writable < 64KB 才用两个 iovec
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;

    const ssize_t n = ::readv(fd, vec, iovcnt);

    if (n < 0) {
        *saveErrno = errno;
    } else if (n <= static_cast<ssize_t>(writable)) {
        // 数据全部在 Buffer 内
        writerIndex_ += n;
    } else {
        // 溢出到 extrabuf，追加到 Buffer（会自动扩容）
        writerIndex_ = buffer_.size();
        append(extrabuf, n - writable);
    }
    return n;
}
```

**readv 的精妙之处**：

1. **一次系统调用读尽数据**：避免多次 `read()`
2. **按需扩容**：小数据量不扩容（writable 够用），大数据量才扩容
3. **栈上分配**：extrabuf 65KB 在栈上，不占堆内存
4. **LT 模式配合**：一次没读完，下次 epoll_wait 还会通知

---

## 4.4 非阻塞写 — 发送缓冲区与高水位

```cpp
// TcpConnection.cc — sendInLoop
void TcpConnection::sendInLoop(const void *data, size_t len) {
    ssize_t nwrote = 0;
    size_t remaining = len;

    // 优化：如果发送缓冲区为空，尝试直接发送
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(channel_->fd(), data, len);

        if (nwrote >= 0) {
            remaining = len - nwrote;
            if (remaining == 0 && writeCompleteCallback_) {
                // 全部发完，通知上层
                loop_->queueInLoop(
                    std::bind(writeCompleteCallback_, shared_from_this()));
            }
        } else {
            nwrote = 0;
            if (errno != EWOULDBLOCK) {
                if (errno == EPIPE || errno == ECONNRESET) {
                    faultError = true;  // 连接断了
                }
            }
        }
    }

    // 没发完的数据放入 outputBuffer_
    if (!faultError && remaining > 0) {
        size_t oldLen = outputBuffer_.readableBytes();

        // 高水位回调：防止发送速度 > 对端接收速度
        if (oldLen + remaining >= highWaterMark_
            && oldLen < highWaterMark_
            && highWaterMarkCallback_) {
            loop_->queueInLoop(
                std::bind(highWaterMarkCallback_, shared_from_this(),
                         oldLen + remaining));
        }

        outputBuffer_.append((char*)data + nwrote, remaining);

        // 注册写事件，等内核缓冲区有空间再继续发
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
}
```

**写流程图**：

```
发送数据 → outputBuffer_ 为空？
              │
      ┌───── 是 ──────┐
      │                │
  直接 write()     有数据在缓冲
      │                │
  写完了？          追加到 outputBuffer_
   │    │           注册 EPOLLOUT
   是    否              │
   │    │          epoll_wait 返回可写
   │    追加剩余          │
   │    到 buffer    handleWrite()
   │    注册写事件    写 buffer 数据
   │                     │
   ▼                 buffer 空了？
 完成回调              │    │
                     是    否
                     │    继续注册写事件
                取消写事件
                完成回调
```

**高水位控制（flow control）**：

```
发送方                                   接收方
  │                                        │
  │── data ──▶ [outputBuffer_ 增长]        │
  │── data ──▶ [outputBuffer_ 增长]        │  接收慢
  │── data ──▶ [到达 highWaterMark!]       │
  │                                        │
  │  触发 highWaterMarkCallback_           │
  │  上层可以暂停生产数据                    │
  │                                        │
  │         [outputBuffer_ 减少]           │  接收方追上来了
  │                                        │
  │  触发 writeCompleteCallback_           │
  │  上层可以恢复生产数据                    │
```

---

# 五、TCP 连接管理

## 5.1 连接的生命周期

```
                新连接到来 (accept)
                      │
                      ▼
              ┌──────────────┐
              │  kConnecting  │  TcpConnection 创建
              └──────┬───────┘
                     │ connectEstablished()
                     ▼
              ┌──────────────┐
              │  kConnected   │  正常读写
              └──────┬───────┘
                     │ shutdown() 或对端关闭
                     ▼
              ┌──────────────┐
              │kDisconnecting │  等待数据发完
              └──────┬───────┘
                     │ connectDestroyed()
                     ▼
              ┌──────────────┐
              │kDisconnected  │  资源释放
              └──────────────┘
```

### 连接建立（完整流程）

```
1. 客户端 connect() → 三次握手
2. mainLoop 的 Acceptor 监听到 EPOLLIN
3. Acceptor::handleRead() → accept() 获得 connfd
4. Acceptor 调用 TcpServer::newConnection()
5. TcpServer 轮询选择一个 subLoop
6. 创建 TcpConnection 对象（属于 subLoop 线程）
7. subLoop->runInLoop(connectEstablished)
8. connectEstablished():
   - 状态: kConnecting → kConnected
   - Channel 启用读事件
   - 调用用户的 connectionCallback_
```

### 数据收发

```
接收数据:
1. subLoop 的 epoll_wait 返回 EPOLLIN
2. Channel::handleEvent() → TcpConnection::handleRead()
3. inputBuffer_.readFd(fd) — readv 读入缓冲区
4. 调用用户的 messageCallback_(conn, &inputBuffer_, timestamp)

发送数据:
1. 用户调用 conn->send(data)
2. 如果在当前线程 → sendInLoop() 直接执行
   如果在其他线程 → runInLoop(sendInLoop) 跨线程调度
3. sendInLoop():
   - 尝试直接 write()
   - 没写完 → 放入 outputBuffer_，注册 EPOLLOUT
4. EPOLLOUT 触发 → handleWrite() → 继续写 outputBuffer_
5. 写完 → 取消 EPOLLOUT，触发 writeCompleteCallback_
```

### 连接关闭

```
主动关闭:
1. 用户调用 conn->shutdown()
2. 状态: kConnected → kDisconnecting
3. 如果 outputBuffer_ 还有数据 → 等写完再关
4. shutdownInLoop() → socket->shutdownWrite() (发送 FIN)
5. 对端收到 FIN → 回复 FIN+ACK
6. 本端收到 FIN → handleRead() 返回 0 → handleClose()
7. handleClose():
   - 状态: kDisconnecting → kDisconnected
   - Channel::disableAll() 取消所有事件
   - 调用 connectionCallback_ 通知用户断开
   - 调用 closeCallback_ → TcpServer 从 map 中移除

被动关闭:
1. 对端 close() → 本端收到 FIN
2. handleRead() → readFd() 返回 0
3. handleClose() → 同上
```

---

## 5.2 连接的线程安全

**原则**：TcpConnection 的所有操作都在其所属的 subLoop 线程中执行。

```cpp
// TcpConnection.cc — send 是线程安全的
void TcpConnection::send(const std::string& message) {
    if (state_ == kConnected) {
        if (loop_->isInLoopThread()) {
            // 在当前线程，直接发送
            sendInLoop(message.c_str(), message.size());
        } else {
            // 在其他线程，转发到 subLoop 线程
            loop_->runInLoop(
                [this, msg = message]() {
                    sendInLoop(msg.c_str(), msg.size());
                });
        }
    }
}
```

---

## 5.3 TcpConnection 的生命周期管理

**问题**：Channel 回调执行时，TcpConnection 可能已经被析构。

**解决**：`shared_ptr` + `weak_ptr` + `tie` 机制

```cpp
// TcpConnection 用 shared_ptr 管理
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

// TcpConnection 继承 enable_shared_from_this
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
    void connectEstablished() {
        channel_->tie(shared_from_this());  // 关键！
        channel_->enableReading();
    }
};

// Channel 中用 weak_ptr 观察 TcpConnection
void Channel::tie(const std::shared_ptr<void>& obj) {
    tie_ = obj;     // weak_ptr 不增加引用计数
    tied_ = true;
}

void Channel::handleEvent(Timestamp receiveTime) {
    if (tied_) {
        std::shared_ptr<void> guard = tie_.lock();
        if (guard) {
            // TcpConnection 还活着，安全执行回调
            handleEventWithGuard(receiveTime);
        }
        // guard 为空 → TcpConnection 已析构，跳过
    }
}
```

**为什么不直接用 shared_ptr？**
- Channel 如果持有 shared_ptr，会形成循环引用
- `TcpConnection → Channel → TcpConnection`（循环！）
- weak_ptr 打破循环，且能检测对象是否存活

---

## 5.4 TCP 粘包与拆包

**问题**：TCP 是字节流，没有消息边界。

```
发送方发了 3 条消息:
[msg1][msg2][msg3]

接收方可能收到:
情况 1: [msg1][msg2][msg3]        — 正常
情况 2: [msg1msg2][msg3]          — 粘包
情况 3: [msg1][ms][g2msg3]        — 拆包
情况 4: [msg1msg2msg3]            — 全粘
```

**解决方案**：

### 方案 1：固定长度

```cpp
// 每条消息固定 100 字节
char buf[100];
read(fd, buf, 100);
```

简单但浪费空间。

### 方案 2：分隔符

```cpp
// HTTP 用 \r\n\r\n 分隔头部和 body
const char* headerEnd = memmem(data, len, "\r\n\r\n", 4);
if (headerEnd) {
    // 找到完整的头部
}
```

### 方案 3：长度前缀（最常用，本项目 RPC 使用）

```
┌──────────┬──────────────────┐
│ 长度 (4B) │     消息体        │
└──────────┴──────────────────┘

// 发送
void send(const std::string& msg) {
    uint32_t len = htonl(msg.size());  // 网络字节序
    outputBuffer_.append(&len, 4);
    outputBuffer_.append(msg.data(), msg.size());
}

// 接收
void onMessage(Buffer* buf) {
    while (buf->readableBytes() >= 4) {
        uint32_t len = ntohl(*(uint32_t*)buf->peek());
        if (buf->readableBytes() >= 4 + len) {
            buf->retrieve(4);  // 跳过长度字段
            std::string msg(buf->peek(), len);
            buf->retrieve(len);
            processMessage(msg);
        } else {
            break;  // 数据不完整，等下次
        }
    }
}
```

### 方案 4：HTTP 方式（本项目 HTTP 模块使用）

```cpp
// 1. 找 \r\n\r\n 确定头部结束位置
const char* headerEnd = memmem(data, len, "\r\n\r\n", 4);
if (!headerEnd) return Incomplete;  // 头部不完整

// 2. 从 Content-Length 头确定 body 长度
size_t contentLen = parseContentLength(headers);

// 3. 检查 body 是否完整
if (bodyReceived < contentLen) return Incomplete;

// 4. 完整消息，处理
```

---

# 六、高性能网络编程实践

## 6.1 惊群问题

**问题描述**：

```
多个线程 epoll_wait 同一个 listenfd:

Thread 1 ──┐
Thread 2 ──┼──▶ epoll_wait(listenfd)
Thread 3 ──┘

新连接到来 → 所有线程被唤醒 → 只有一个能 accept 成功
→ 其他线程白白唤醒，浪费 CPU
```

**本项目的解决方案 — 主从 Reactor**：

```
mainLoop: 只有它监听 listenfd → accept
subLoop1: 只监听 connfd1, connfd2
subLoop2: 只监听 connfd3, connfd4

每个 fd 只在一个线程的 epoll 中 → 不可能惊群
```

**其他方案**：

```
方案 2: SO_REUSEPORT (Linux 3.9+)
  - 多个进程绑定同一端口
  - 内核在 accept 层面做负载均衡
  - 每个进程独立 accept，不冲突

方案 3: accept 加锁（Nginx 早期方案）
  - pthread_mutex_lock()
  - accept()
  - pthread_mutex_unlock()
```

---

## 6.2 SOCK_CLOEXEC 和 SOCK_NONBLOCK

```cpp
// 项目中 Socket 创建
int sockfd = socket(AF_INET,
    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

// 项目中 epoll 创建
int epollfd = epoll_create1(EPOLL_CLOEXEC);

// 项目中 eventfd 创建
int evtfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
```

**SOCK_CLOEXEC 的作用**：
- `fork()` + `exec()` 后自动关闭该 fd
- 防止子进程继承不该继承的 fd
- 避免 fd 泄漏到外部程序

**SOCK_NONBLOCK 的作用**：
- 创建时直接设为非阻塞
- 比 `fcntl(fd, F_SETFL, O_NONBLOCK)` 更原子（避免竞态）

---

## 6.3 readv/writev — 分散/聚集 I/O

```cpp
// readv: 一次读入多个缓冲区（项目 Buffer::readFd 使用）
struct iovec vec[2];
vec[0].iov_base = buffer;       // 主缓冲区
vec[0].iov_len = buffer_size;
vec[1].iov_base = extrabuf;     // 溢出缓冲区
vec[1].iov_len = 65536;
ssize_t n = readv(fd, vec, 2);  // 一次系统调用

// writev: 一次写入多个缓冲区
struct iovec vec[2];
vec[0].iov_base = header;
vec[0].iov_len = header_len;
vec[1].iov_base = body;
vec[1].iov_len = body_len;
ssize_t n = writev(fd, vec, 2);  // 合并发送
```

**优势**：减少系统调用次数，一次 readv/writev = 多次 read/write。

---

## 6.4 零拷贝技术

### sendfile — 文件直接发送到 socket

```cpp
#include <sys/sendfile.h>

// 不经过用户空间，内核直接从文件 → socket
ssize_t n = sendfile(sockfd, filefd, &offset, count);
```

```
传统方式 (4 次拷贝):
  磁盘 → 内核缓冲区 → 用户缓冲区 → 内核 socket 缓冲区 → 网卡

sendfile (2 次拷贝):
  磁盘 → 内核缓冲区 → 内核 socket 缓冲区 → 网卡
  (DMA)            (CPU)              (DMA)

sendfile + DMA gather (0 次 CPU 拷贝):
  磁盘 → 内核缓冲区 ────────────────────▶ 网卡
  (DMA)           (只传描述符)           (DMA gather)
```

### mmap — 内存映射

```cpp
void* addr = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
write(sockfd, addr, size);
munmap(addr, size);
```

减少一次拷贝：内核缓冲区和用户缓冲区共享物理页面。

### splice — 管道零拷贝

```cpp
// 从 socket 到 pipe（零拷贝）
splice(sockfd, NULL, pipefd[1], NULL, len, SPLICE_F_MOVE);
// 从 pipe 到 socket（零拷贝）
splice(pipefd[0], NULL, sockfd, NULL, len, SPLICE_F_MOVE);
```

---

## 6.5 性能调优清单

### 系统级

```bash
# 文件描述符上限
ulimit -n 1000000
echo "* soft nofile 1000000" >> /etc/security/limits.conf
echo "* hard nofile 1000000" >> /etc/security/limits.conf

# TCP 参数调优
echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse          # 重用 TIME_WAIT
echo 30 > /proc/sys/net/ipv4/tcp_fin_timeout       # 缩短 FIN_WAIT_2 时间
echo 65535 > /proc/sys/net/ipv4/ip_local_port_range # 更多临时端口
echo 4096 > /proc/sys/net/core/somaxconn           # listen backlog 上限
echo "4096 87380 16777216" > /proc/sys/net/ipv4/tcp_rmem  # 接收缓冲区
echo "4096 87380 16777216" > /proc/sys/net/ipv4/tcp_wmem  # 发送缓冲区
```

### 应用级

| 优化手段 | 说明 | 本项目是否使用 |
|---------|------|-------------|
| 非阻塞 I/O | 避免线程等待 | 是 |
| I/O 多路复用 (epoll) | 一个线程监听多 fd | 是 |
| One Loop Per Thread | 无锁设计 | 是 |
| 应用层 Buffer | readv 一次读尽 | 是 |
| swap 代替拷贝 | doPendingFunctors | 是 |
| 预留头部空间 | Buffer kCheapPrepend | 是 |
| 连接池复用 | 避免频繁创建销毁 | 是 |
| 定时器时间轮 | O(1) 定时器操作 | 是 |
| 异步日志双缓冲 | 日志不阻塞业务 | 是 |
| TCP_NODELAY | 低延迟场景 | 可选 |
| SO_REUSEPORT | 多进程负载均衡 | 可选 |
| sendfile | 静态文件零拷贝 | 未使用 |

---

## 6.6 常见错误码

| errno | 含义 | 处理方式 |
|-------|------|---------|
| EAGAIN / EWOULDBLOCK | 非阻塞 fd 暂无数据 | 等下次 epoll 通知 |
| EINTR | 被信号中断 | 重试 |
| ECONNRESET | 对端 RST 重置连接 | 关闭连接 |
| EPIPE | 写已关闭的 socket | 关闭连接（忽略 SIGPIPE） |
| EMFILE | fd 用完了 | 增加 ulimit 或关闭空闲连接 |
| ECONNREFUSED | 对端拒绝连接 | 重试或报错 |
| ETIMEDOUT | 连接超时 | 重试或报错 |

**项目中的错误处理**：

```cpp
// TcpConnection::handleRead — 读错误
ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
if (n > 0) {
    messageCallback_(...);       // 正常数据
} else if (n == 0) {
    handleClose();               // 对端关闭
} else {
    errno = savedErrno;
    handleError();               // 读错误
}

// TcpConnection::sendInLoop — 写错误
if (errno != EWOULDBLOCK) {
    if (errno == EPIPE || errno == ECONNRESET) {
        faultError = true;       // 连接断了，放弃发送
    }
}
```

---

# 七、面试高频问题速查

## 网络模型类

| 问题 | 关键答案 |
|------|---------|
| 五种 I/O 模型的区别？ | 阻塞/非阻塞 × 同步/异步，前四种都是同步（阶段 2 阻塞） |
| epoll 比 select 快在哪？ | 红黑树管理 fd、回调驱动就绪链表、只返回就绪 fd、fd 不用每次传递 |
| LT 和 ET 区别？ | LT 有数据就通知，ET 状态变化才通知；ET 必须循环读完 |
| 为什么选 LT？ | 安全不漏事件、编程简单、性能差距可忽略 |
| Reactor vs Proactor？ | Reactor: I/O 就绪通知，自己读写；Proactor: I/O 完成通知，内核读写 |

## 设计类

| 问题 | 关键答案 |
|------|---------|
| 为什么用 One Loop Per Thread？ | 无锁设计、缓存友好、可水平扩展 |
| 跨线程如何调度任务？ | eventfd + pendingFunctors 队列，swap 减少锁持有时间 |
| 为什么 swap 不拷贝？ | O(1) 交换指针，锁外执行回调，防止死锁 |
| Buffer 为什么用 readv？ | 一次系统调用读尽数据，栈上 extrabuf 兜底，按需扩容 |
| 高水位回调有什么用？ | 流量控制，防止发送方太快导致内存暴涨 |
| Channel 的 tie 机制？ | weak_ptr 观察 TcpConnection，防止回调时对象已析构 |

## TCP 类

| 问题 | 关键答案 |
|------|---------|
| 三次握手为什么不能两次？ | 防止旧 SYN 延迟到达导致误建连接 |
| 四次挥手为什么不能三次？ | TCP 全双工，关闭是单向的，ACK 和 FIN 可能不能合并 |
| TIME_WAIT 作用？ | 确保最后 ACK 到达 + 让旧数据包消失 |
| 大量 TIME_WAIT 怎么办？ | SO_REUSEADDR、tcp_tw_reuse、短连接改长连接 |
| 粘包怎么解决？ | 长度前缀、分隔符、固定长度 |
| CLOSE_WAIT 过多说明什么？ | 程序没有正确 close() 连接 |

## Socket 选项类

| 问题 | 关键答案 |
|------|---------|
| SO_REUSEADDR？ | 允许绑定 TIME_WAIT 状态的地址，服务器重启必备 |
| SO_REUSEPORT？ | 多进程共享端口，内核负载均衡 |
| TCP_NODELAY？ | 禁用 Nagle 算法，减少小包延迟 |
| SO_KEEPALIVE？ | TCP 心跳探测死连接 |
| SOCK_CLOEXEC？ | exec 后自动关 fd，防泄漏 |

## 性能类

| 问题 | 关键答案 |
|------|---------|
| 惊群问题？ | 多线程 accept 同一端口，本项目用主从 Reactor 避免 |
| 零拷贝有哪些？ | sendfile、mmap、splice |
| C10K 问题怎么解决？ | epoll + 非阻塞 I/O + 事件驱动 + 连接池 |
| 如何提升 QPS？ | 增加线程、减少系统调用、连接复用、异步日志 |

---

## 项目中的网络 I/O 技术栈总结

```
应用层:   HttpServer / RpcServer / WebSocketServer
          │
协议解析:  HttpRequest (状态机) / Protobuf (长度前缀) / WebSocket (帧解析)
          │
连接管理:  TcpConnection (状态机 + shared_ptr 生命周期)
          │
缓冲区:   Buffer (readv + 自动扩容 + prependable)
          │
事件分发:  Channel (fd + 回调 + tie 安全机制)
          │
I/O 复用:  EPollPoller (epoll LT + 动态扩容 events)
          │
线程模型:  EventLoop (One Loop Per Thread)
          EventLoopThreadPool (主从 Reactor + 轮询分发)
          │
跨线程:   eventfd + pendingFunctors (swap 优化)
          │
系统调用:  socket / bind / listen / accept / read / write / readv
          epoll_create1 / epoll_ctl / epoll_wait / eventfd
          │
内核:     TCP/IP 协议栈 → 网卡驱动 → NIC
```

---

> 本文档基于 mymuduo-http 项目实际源码，覆盖网络 I/O 从底层到架构的完整知识体系。
> 最后更新：2026-04-09
