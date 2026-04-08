# mymuduo-http 模块文档

本文档基于源码提取，包含所有模块的完整公开 API。

## 模块总览

```
mymuduo-http/src/
├── net/           # 核心网络库（Reactor 模式基础设施）
├── http/          # HTTP/1.1 服务器
├── rpc/           # RPC 框架（JSON-RPC 2.0 + Protobuf-RPC）
├── websocket/     # WebSocket 协议（RFC 6455）
├── registry/      # 服务注册与发现
├── balancer/      # 负载均衡策略
├── timer/         # 时间轮定时器
├── pool/          # TCP 连接池
├── asynclogger/   # 双缓冲异步日志
├── config/        # INI 格式配置管理
└── util/          # 工具类（信号处理）
```

---

## 一、核心网络库 (net/)

网络库的基础设施层，采用 Reactor 模式 + One Loop Per Thread 线程模型。

### 1.1 EventLoop — 事件循环

**文件**：`src/net/EventLoop.h`

**职责**：Reactor 模式的核心，每个线程最多持有一个 EventLoop，负责监听和分发 I/O 事件、执行跨线程任务。

**类型定义**：
```cpp
using Functor = std::function<void()>;
using ChannelList = std::vector<Channel*>;
```

**完整公开 API**：
```cpp
EventLoop();
~EventLoop();

// 事件循环控制
void loop();                              // 开启事件循环，阻塞直到 quit()
void quit();                              // 退出事件循环
Timestamp pollReturnTime() const;         // 获取上次 poll 返回的时间戳

// 跨线程任务调度
void runInLoop(Functor cb);               // 在本线程执行回调，若非本线程则转发
void queueInLoop(Functor cb);             // 加入任务队列，下次 loop 执行
void wakeup();                            // 通过 eventfd 唤醒阻塞中的 epoll_wait

// Channel 管理
void updateChannel(Channel* channel);     // 注册/更新 Channel 到 Poller
void removeChannel(Channel* channel);     // 从 Poller 移除 Channel
bool hasChannel(Channel* channel);        // 查询 Channel 是否在本 EventLoop

// 线程断言
bool isInLoopThread() const;              // 当前线程是否为 EventLoop 所属线程
```

**设计要点**：
- 通过 `eventfd` 实现跨线程唤醒，`wakeup()` 写入 eventfd 触发 epoll 返回
- `runInLoop()` 若在当前线程调用则立即执行，否则调用 `queueInLoop()` 转发
- 每次 `loop()` 迭代：`poll()` → 处理活跃 Channel 回调 → 执行 `pendingFunctors_`

---

### 1.2 Channel — 事件通道

**文件**：`src/net/Channel.h`

**职责**：封装一个文件描述符（fd）及其关注的事件类型和对应回调。Channel 不拥有 fd 的生命周期。

**类型定义**：
```cpp
using EventCallback = std::function<void()>;
using ReadEventCallback = std::function<void(Timestamp)>;
```

**常量**：
```cpp
static const int kNoneEvent;    // 无事件
static const int kReadEvent;    // EPOLLIN | EPOLLPRI
static const int kWriteEvent;   // EPOLLOUT
```

**完整公开 API**：
```cpp
Channel(EventLoop* loop, int fd);
~Channel();

// 事件回调设置
void setReadCallback(ReadEventCallback cb);
void setWriteCallback(EventCallback cb);
void setCloseCallback(EventCallback cb);
void setErrorCallback(EventCallback cb);

// 事件分发
void handleEvent(Timestamp receiveTime);  // 根据 revents_ 调用相应回调
void tie(const std::shared_ptr<void>&);   // 绑定 TcpConnection 防止回调时对象已销毁

// 事件控制（修改后自动调用 EventLoop::updateChannel）
void enableReading();                     // 开启读事件监听
void disableReading();                    // 关闭读事件监听
void enableWriting();                     // 开启写事件监听
void disableWriting();                    // 关闭写事件监听
void disableAll();                        // 关闭所有事件监听

// 状态查询
bool isNoneEvent() const;                 // 是否无关注事件
bool isWriting() const;                   // 是否关注写事件
bool isReading() const;                   // 是否关注读事件
int fd() const;                           // 获取文件描述符
int events() const;                       // 获取关注的事件
void set_revents(int revt);               // Poller 设置实际发生的事件

// Poller 使用
int index();                              // 在 Poller 中的状态索引
void set_index(int idx);
EventLoop* ownerLoop();                   // 获取所属 EventLoop

// 生命周期
void remove();                            // 从 EventLoop 移除自身
```

**设计要点**：
- `tie()` 机制：Channel 通过 `weak_ptr` 绑定 TcpConnection，`handleEvent` 时先 `lock()` 确保连接存活
- Channel 不直接操作 epoll，而是通过 `EventLoop::updateChannel()` 间接调用 Poller

---

### 1.3 Poller / EPollPoller — I/O 多路复用

**文件**：`src/net/Poller.h`、`src/net/EPollPoller.h`

**职责**：封装 I/O 多路复用系统调用。Poller 是抽象基类，EPollPoller 是 Linux epoll 实现。

**Poller 抽象接口**：
```cpp
using ChannelList = std::vector<Channel*>;
using ChannelMap = std::unordered_map<int, Channel*>;

Poller(EventLoop* loop);
virtual ~Poller();

// 纯虚方法
virtual Timestamp poll(int timeoutMs, ChannelList* activeChannels) = 0;
virtual void updateChannel(Channel* channel) = 0;
virtual void removeChannel(Channel* channel) = 0;

// 查询
bool hasChannel(Channel* channel) const;

// 工厂方法（定义在 DefaultPoller.cc，避免头文件引入具体实现）
static Poller* newDefaultPoller(EventLoop* loop);
```

**EPollPoller 实现**：
```cpp
static const int kInitEventListSize = 16;   // 初始事件列表大小
using EventList = std::vector<epoll_event>;

explicit EPollPoller(EventLoop* loop);
virtual ~EPollPoller() override;

Timestamp poll(int timeoutMs, ChannelList* activeChannels) override;
void updateChannel(Channel* channel) override;
void removeChannel(Channel* channel) override;
```

**设计要点**：
- 使用 LT（水平触发）模式，更简单不易出错
- `EventList` 动态扩容：当返回的事件数等于列表大小时，自动扩容为 2 倍
- Channel 在 Poller 中有三种状态：`kNew`（未注册）、`kAdded`（已注册）、`kDeleted`（已删除）

---

### 1.4 Buffer — 读写缓冲区

**文件**：`src/net/Buffer.h`

**职责**：自动增长的缓冲区，管理 TCP 数据的读取和写入。

**常量**：
```cpp
static const size_t kCheapPrepend = 8;     // 预留前置空间（用于协议头）
static const size_t kInitialSize = 1024;   // 初始缓冲区大小
```

**内存布局**：
```
+------------------+------------------+------------------+
| prependable (≥8) |  readable bytes  |  writable bytes  |
+------------------+------------------+------------------+
0          readerIndex_      writerIndex_          size()
```

**完整公开 API**：
```cpp
explicit Buffer();

// 容量查询
size_t readableBytes() const;             // writerIndex_ - readerIndex_
size_t writableBytes() const;             // size() - writerIndex_
size_t prependableBytes() const;          // readerIndex_

// 数据指针
const char* peek();                       // 可读数据起始位置
char* beginWrite();                       // 可写位置
const char* beginWrite() const;

// 读取操作
void retrieve(size_t len);                // 消费 len 字节
void retrieveAll();                       // 消费全部可读数据
std::string retrieveAsString(size_t len); // 取出 len 字节作为 string
std::string retrieveAllAsString();        // 取出全部可读数据作为 string

// 写入操作
void ensureWritableBytes(size_t len);     // 确保有足够可写空间
void append(const char* data, size_t len);// 追加数据

// fd 读写
ssize_t readFd(int fd, int* saveErrno);   // 从 fd 读数据（使用 readv + 栈上缓冲）
ssize_t writeFd(int fd, int* saveErrno);  // 向 fd 写数据
```

**设计要点**：
- `readFd()` 使用 `readv` 配合 65536 字节的栈上临时缓冲区，避免预分配大 buffer 又保证单次能读取大量数据
- 空间不足时优先移动数据复用 prepend 区域，仍不够才 `resize`

---

### 1.5 Socket — 套接字封装

**文件**：`src/net/Socket.h`

**职责**：RAII 封装 socket fd，析构时自动 `close()`。

**完整公开 API**：
```cpp
explicit Socket(int sockfd);
~Socket();                                // close(sockfd_)

int fd() const;                           // 获取原始 fd
void bindAddress(const InetAddress& localaddr);
void listen();
int accept(InetAddress* peeraddr);        // 接受连接，返回 connfd
void shutdownWrite();                     // 关闭写端

// 套接字选项
void setTcpNoDelay(bool on);              // TCP_NODELAY（禁用 Nagle）
void setReuseAddr(bool on);               // SO_REUSEADDR
void setReusePort(bool on);               // SO_REUSEPORT
void setKeepAlive(bool on);               // SO_KEEPALIVE
```

---

### 1.6 InetAddress — 网络地址

**文件**：`src/net/InetAddress.h`

**职责**：封装 `sockaddr_in`，提供 IP:Port 的便捷操作。

**完整公开 API**：
```cpp
explicit InetAddress(uint16_t port = 0, std::string ip = "127.0.0.1");
explicit InetAddress(const sockaddr_in& addr);

std::string toIp() const;                // 获取 IP 字符串
std::string toIpPort() const;            // 获取 "IP:Port" 字符串
uint16_t toPort() const;                 // 获取端口号
const sockaddr_in* getSockAddr() const;  // 获取底层 sockaddr_in 指针
void setSockAddr(const sockaddr_in& addr);
```

---

### 1.7 Acceptor — 连接接受器

**文件**：`src/net/Acceptor.h`

**职责**：运行在 mainReactor 线程，监听端口并接受新连接。

**类型定义**：
```cpp
using NewConnectionCallback = std::function<void(int sockfd, const InetAddress&)>;
```

**完整公开 API**：
```cpp
Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport);
~Acceptor();

void setNewConnectionCallback(const NewConnectionCallback& cb);
bool listenning() const;                  // 是否正在监听
void listen();                            // 开始监听
```

**设计要点**：
- 支持 `SO_REUSEPORT`，允许多线程各自 accept，提升并发性能
- 新连接到来时回调 TcpServer，由 TcpServer 分配到 subReactor

---

### 1.8 TcpConnection — TCP 连接

**文件**：`src/net/TcpConnection.h`

**职责**：管理单个 TCP 连接的完整生命周期，包含输入/输出缓冲区。

**连接状态**：
```cpp
enum StateE { kDisconnected, kConnecting, kConnected, kDisconnecting };
```

**完整公开 API**：
```cpp
TcpConnection(EventLoop* loop, const std::string& nameArg,
              int sockfd, const InetAddress& localAddr,
              const InetAddress& peerAddr);
~TcpConnection();

// 信息查询
EventLoop* getLoop() const;
const std::string& name() const;
const InetAddress& localAddress() const;
const InetAddress& peerAddress() const;
bool connected() const;

// 数据发送
void send(const std::string& message);    // 线程安全的发送

// 连接控制
void shutdown();                          // 发起半关闭（关闭写端）
void shutdownInLoop();

// 回调设置
void setConnectionCallback(const ConnectionCallback& cb);
void setMessageCallback(const MessageCallback& cb);
void setWriteCompleteCallback(const WriteCompleteCallback& cb);
void setHighWaterMarkCallback(const HighWaterMarkCallback& cb);
void setCloseCallback(const CloseCallback& cb);

// 生命周期管理（由 TcpServer 调用）
void connectEstablished();                // 连接建立后调用
void connectDestroyed();                  // 连接销毁前调用
```

**回调类型**（定义在 `src/net/Callbacks.h`）：
```cpp
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*, Timestamp)>;
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, size_t)>;
```

**设计要点**：
- 使用 `shared_ptr` 管理，通过 `shared_from_this()` 保证回调期间对象存活
- 支持高水位回调：当输出缓冲区积压超过阈值时触发，可用于限流
- `send()` 线程安全：非本线程调用时自动转发到 I/O 线程执行

---

### 1.9 TcpServer — TCP 服务器

**文件**：`src/net/TcpServer.h`

**职责**：用户编写服务器程序的主要入口，管理 Acceptor 和所有 TcpConnection。

**枚举与类型**：
```cpp
enum Option { kNoReusePort, kReusePort };
using ThreadInitCallback = std::function<void(EventLoop*)>;
using ConnectionMap = std::unordered_map<std::string, TcpConnectionPtr>;
```

**完整公开 API**：
```cpp
TcpServer(EventLoop* loop, const InetAddress& listenAddr,
          const std::string& nameArg, Option option = kNoReusePort);
~TcpServer();

// 配置
void setThreadNum(int numThreads);        // 设置 subReactor 线程数
void setThreadInitcallback(const ThreadInitCallback& cb);

// 回调设置
void setConnectionCallback(const ConnectionCallback& cb);
void setMessageCallback(const MessageCallback& cb);
void setWriteCompleteCallback(const WriteCompleteCallback& cb);

// 启动
void start();                             // 启动服务器（仅首次调用有效）
```

**架构**：
```
mainLoop (Acceptor) ──accept──> newConnection()
                                    │
                            round-robin 分配
                                    │
                    ┌───────────────┼───────────────┐
                    ▼               ▼               ▼
              subLoop_0        subLoop_1        subLoop_2
           (TcpConnection)  (TcpConnection)  (TcpConnection)
```

---

### 1.10 EventLoopThread / EventLoopThreadPool — 线程管理

**文件**：`src/net/EventLoopThread.h`、`src/net/EventLoopThreadPool.h`

**EventLoopThread**：一个线程 + 一个 EventLoop 的封装。
```cpp
using ThreadInitCallback = std::function<void(EventLoop*)>;

EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback(),
                const std::string& name = std::string());
~EventLoopThread();
EventLoop* startLoop();                   // 启动线程并返回 EventLoop 指针
```

**EventLoopThreadPool**：管理多个 I/O 线程。
```cpp
EventLoopThreadPool(EventLoop* baseLoop, const std::string& nameArg);
~EventLoopThreadPool();

void setThreadNum(int numThreads);        // 设置线程数
void start(const ThreadInitCallback& cb = ThreadInitCallback());

EventLoop* getNextLoop();                 // Round-Robin 获取下一个 EventLoop
std::vector<EventLoop*> getAllLoops();
bool started() const;
const std::string name();
```

---

### 1.11 Thread — 线程封装

**文件**：`src/net/Thread.h`

```cpp
using ThreadFunc = std::function<void()>;

explicit Thread(ThreadFunc, const std::string& name = std::string());
~Thread();

void start();
void join();
bool started() const;
pid_t tid() const;
const std::string& name() const;
static int numCreated();                  // 已创建线程总数
```

---

### 1.12 Timestamp — 时间戳

**文件**：`src/net/Timestamp.h`

```cpp
Timestamp();
explicit Timestamp(int64_t microSecondsSinceEpoch);

static Timestamp now();                   // 获取当前时间
std::string toString() const;             // 格式化输出
int64_t microSecondsSinceEpoch_;          // 微秒级时间戳
```

---

### 1.13 Logger — 简易日志

**文件**：`src/net/logger.h`

**日志级别**：
```cpp
enum LogLvel { INFO, DEBUG, ERROR, FATAL };
```

**API**：
```cpp
static Logger& instance();               // 单例
void setLogLevel(int level);
void log(std::string msg);
```

**日志宏**（固定 1024 字节缓冲区）：
```cpp
LOG_INFO(logmsgFormat, ...)
LOG_ERROR(logmsgFormat, ...)
LOG_FATAL(logmsgFormat, ...)              // 输出后 exit(EXIT_FAILURE)
LOG_DEBUG(logmsgFormat, ...)              // 仅 DEBUG 级别时输出
```

---

### 1.14 noncopyable — 禁止拷贝基类

**文件**：`src/net/noncopyable.h`

```cpp
class noncopyable {
protected:
    noncopyable() = default;
    ~noncopyable() = default;
    noncopyable(const noncopyable&) = delete;
    noncopyable& operator=(const noncopyable&) = delete;
};
```

---

### 1.15 CurrentThread — 线程本地工具

**文件**：`src/net/CurrentThread.h`

```cpp
namespace CurrentThread {
    extern __thread int t_cachedTid;      // 线程本地缓存的 tid
    void cacheTid();                      // 首次调用 syscall(SYS_gettid) 并缓存
    inline int tid();                     // 返回缓存的 tid
}
```

---

## 二、HTTP 模块 (http/)

### 2.1 HttpServer — HTTP 服务器

**文件**：`src/http/HttpServer.h`

**职责**：基于 TcpServer 构建的 HTTP/1.1 服务器，支持路由、中间件、静态文件服务。

**类型定义**：
```cpp
using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

struct Route {
    HttpMethod method;
    std::string pattern;
    HttpHandler handler;
    std::regex regex;
    Route(HttpMethod m, const std::string& p, HttpHandler h);
};

enum class ParseResult { Complete, Incomplete, Error };
```

**常量**：
```cpp
static constexpr size_t kMaxBodySize = 10 * 1024 * 1024;   // 请求体上限 10MB
static constexpr size_t kMaxRequestLine = 8192;             // 请求行上限 8KB
```

**完整公开 API**：
```cpp
HttpServer(EventLoop* loop, const InetAddress& addr,
           const std::string& name = "HttpServer");

// 配置
void setThreadNum(int num);               // 设置 I/O 线程数
void start();                             // 启动 HTTP 服务器

// 路由注册（支持正则表达式路径）
void GET(const std::string& path, HttpHandler handler);
void POST(const std::string& path, HttpHandler handler);
void PUT(const std::string& path, HttpHandler handler);
void DELETE(const std::string& path, HttpHandler handler);

// 静态文件服务
void serveStatic(const std::string& urlPrefix, const std::string& dir);

// 中间件
void use(HttpHandler middleware);         // 注册全局中间件
```

**特性说明**：
- 路由支持正则表达式匹配，如 `"/users/(\\d+)"`
- 中间件按注册顺序执行，在路由处理之前
- Keep-Alive 连接复用：根据 `Connection` 头决定是否保持连接
- HTTP Pipeline：支持同一连接上的多个请求顺序处理
- 静态文件服务自动检测 MIME 类型，防止路径穿越攻击

---

### 2.2 HttpRequest — HTTP 请求

**文件**：`src/http/HttpRequest.h`

**枚举**：
```cpp
enum class HttpMethod { GET, POST, PUT, DELETE, HEAD, UNKNOWN };
enum class HttpVersion { HTTP_10, HTTP_11, UNKNOWN };
```

**公开成员**：
```cpp
HttpMethod method;
HttpVersion version;
std::string path;
std::string query;
std::unordered_map<std::string, std::string> headers;
std::unordered_map<std::string, std::string> params;  // 查询参数
std::string body;
```

**完整公开 API**：
```cpp
HttpRequest();

bool parseRequestLine(const std::string& line);    // 解析请求行
bool parseHeader(const std::string& line);          // 解析单个头部
std::string getHeader(const std::string& key) const;// 获取头部值
bool keepAlive() const;                             // 是否保持连接
size_t contentLength() const;                       // Content-Length 值
```

---

### 2.3 HttpResponse — HTTP 响应

**文件**：`src/http/HttpResponse.h`

**状态码枚举**：
```cpp
enum class HttpStatusCode {
    OK = 200, CREATED = 201, NO_CONTENT = 204,
    BAD_REQUEST = 400, NOT_FOUND = 404,
    INTERNAL_SERVER_ERROR = 500
};
```

**公开成员**：
```cpp
HttpStatusCode statusCode;
std::string statusMessage;
std::unordered_map<std::string, std::string> headers;
std::string body;
bool closeConnection;
```

**完整公开 API**：
```cpp
HttpResponse();

// 设置方法
void setStatusCode(HttpStatusCode code);
void setContentType(const std::string& type);
void setContentLength(size_t len);
void setHeader(const std::string& key, const std::string& value);
void setBody(const std::string& b);

// 便捷方法
void setJson(const std::string& json);    // 设置 JSON 响应体 + Content-Type
void setHtml(const std::string& html);    // 设置 HTML 响应体 + Content-Type
void setText(const std::string& text);    // 设置纯文本响应体 + Content-Type

// 序列化
std::string toString() const;             // 生成完整 HTTP 响应报文

// 工厂方法
static HttpResponse ok(const std::string& body = "");
static HttpResponse json(const std::string& json);
static HttpResponse notFound(const std::string& msg = "Not Found");
static HttpResponse badRequest(const std::string& msg = "Bad Request");
static HttpResponse serverError(const std::string& msg = "Internal Server Error");
```

---

## 三、RPC 模块 (rpc/)

提供两套 RPC 实现：JSON-RPC 2.0（人类可读）和 Protobuf-RPC（高性能二进制）。

### 3.1 RpcServer — JSON-RPC 服务端

**文件**：`src/rpc/RpcServer.h`

**类型定义**：
```cpp
using RpcMethod = std::function<json(const json&)>;
```

**完整公开 API**：
```cpp
RpcServer(EventLoop* loop, const InetAddress& addr);

void setThreadNum(int num);
void start();
void registerMethod(const std::string& name, RpcMethod method);

template<typename T>
void registerService(T* service);         // 批量注册服务对象的所有方法
```

**协议格式**（JSON-RPC 2.0）：
```json
// 请求
{"jsonrpc": "2.0", "method": "add", "params": [1, 2], "id": 1}
// 响应
{"jsonrpc": "2.0", "result": 3, "id": 1}
```

---

### 3.2 RpcClient — JSON-RPC 客户端

**文件**：`src/rpc/RpcClient.h`

```cpp
RpcClient(const std::string& host, int port);

json call(const std::string& method, const json& params = json());
std::future<json> asyncCall(const std::string& method, const json& params = json());
```

---

### 3.3 RpcServerPb — Protobuf-RPC 服务端

**文件**：`src/rpc/RpcServerPb.h`

**类型定义**：
```cpp
using PbMethod = std::function<void(const google::protobuf::Message&,
                                     google::protobuf::Message&)>;
```

**常量**：
```cpp
static constexpr size_t kMaxFrameSize = 10 * 1024 * 1024;  // 最大帧 10MB
```

**完整公开 API**：
```cpp
RpcServerPb(EventLoop* loop, const InetAddress& addr);

void setThreadNum(int num);
void start();

template<typename T1, typename T2>
void registerMethod(const std::string& serviceName,
                    const std::string& methodName,
                    std::function<void(const T1&, T2&)> handler);
```

**协议格式**（二进制帧）：
```
+----------------+------------------+------------------+
| 4 bytes length | service/method   | protobuf payload |
+----------------+------------------+------------------+
```

---

### 3.4 RpcClientPb — Protobuf-RPC 客户端

**文件**：`src/rpc/RpcClientPb.h`

**常量**：
```cpp
constexpr int32_t RPC_MAX_FRAME_LENGTH = 64 * 1024 * 1024;  // 最大帧 64MB
```

**完整公开 API**：
```cpp
RpcClientPb(const std::string& host, int port);
~RpcClientPb();

bool connect();
void disconnect();

template<typename T1, typename T2>
bool call(const std::string& service, const std::string& method,
          const T1& request, T2& response);

template<typename T1, typename T2>
std::future<bool> asyncCall(const std::string& service, const std::string& method,
                            const T1& request, T2& response);
```

---

## 四、WebSocket 模块 (websocket/)

实现 RFC 6455 WebSocket 协议。

### 4.1 WebSocketFrame / WebSocketFrameCodec — 帧编解码

**文件**：`src/websocket/WebSocketFrame.h`

**操作码枚举**：
```cpp
enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA
};
```

**WebSocketFrame 结构**：
```cpp
struct WebSocketFrame {
    bool fin;
    uint8_t rsv1, rsv2, rsv3;
    WsOpcode opcode;
    bool mask;
    uint8_t maskingKey[4];
    std::vector<uint8_t> payload;

    WebSocketFrame();
    bool isControlFrame() const;
    size_t payloadSize() const;
    std::string textPayload() const;
    void setTextPayload(const std::string& text);
    void setBinaryPayload(const std::vector<uint8_t>& data);
};
```

**WebSocketFrameCodec 编解码器**：
```cpp
struct DecodeResult {
    enum Status { Ok, Incomplete, Error };
    Status status;
    std::string error;
    WebSocketFrame frame;
    size_t consumed;
};

// 编码
static std::vector<uint8_t> encode(const WebSocketFrame& frame, bool mask = false);
static std::vector<uint8_t> encodeText(const std::string& text);
static std::vector<uint8_t> encodeBinary(const std::vector<uint8_t>& data);
static std::vector<uint8_t> encodeClose(uint16_t code = 1000, const std::string& reason = "");
static std::vector<uint8_t> encodePing(const std::vector<uint8_t>& data = {});
static std::vector<uint8_t> encodePong(const std::vector<uint8_t>& pingData = {});

// 解码
static DecodeResult decode(const uint8_t* data, size_t len);

// 握手密钥计算（SHA1 + Base64）
static std::string computeAcceptKey(const std::string& clientKey);
```

---

### 4.2 WsSession — WebSocket 会话

**文件**：`src/websocket/WsSession.h`

**连接状态**：
```cpp
enum class WsState { Connecting, Open, Closing, Closed };
```

**消息结构**：
```cpp
struct WsMessage {
    WsOpcode opcode;
    std::vector<uint8_t> data;

    bool isText() const;
    bool isBinary() const;
    bool isClose() const;
    bool isPing() const;
    bool isPong() const;
    std::string text() const;
};
```

**完整公开 API**：
```cpp
using WsSessionPtr = std::shared_ptr<WsSession>;
using MessageHandler = std::function<void(const WsSessionPtr&, const WsMessage&)>;
using CloseHandler = std::function<void(const WsSessionPtr&)>;
using ErrorHandler = std::function<void(const WsSessionPtr&, const std::string&)>;

WsSession(const TcpConnectionPtr& conn);

// 发送消息
void sendText(const std::string& text);
void sendBinary(const std::vector<uint8_t>& data);
void ping(const std::vector<uint8_t>& data = {});
void pong(const std::vector<uint8_t>& data = {});

// 连接控制
void close(uint16_t code = 1000, const std::string& reason = "");
void forceClose();

// 状态查询
WsState state() const;
bool isOpen() const;
bool isClosed() const;
TcpConnectionPtr connection() const;
std::string clientAddress() const;

// 回调设置
void setMessageHandler(MessageHandler handler);
void setCloseHandler(CloseHandler handler);
void setErrorHandler(ErrorHandler handler);

// 上下文存储（键值对附加数据）
void setContext(const std::string& key, const std::string& value);
std::string getContext(const std::string& key, const std::string& defaultValue = "") const;

// 活跃度
void updateActivity();
int64_t idleTimeMs() const;
```

---

### 4.3 WebSocketServer — WebSocket 服务器

**文件**：`src/websocket/WebSocketServer.h`

**配置结构**：
```cpp
struct WebSocketConfig {
    int maxMessageSize = 10 * 1024 * 1024;  // 最大消息 10MB
    int idleTimeoutMs = 60000;               // 空闲超时 60s
    bool enablePingPong = true;              // 启用心跳
    int pingIntervalMs = 30000;              // 心跳间隔 30s
};
```

**类型定义**：
```cpp
using ConnectionHandler = std::function<void(const WsSessionPtr&)>;
using MessageHandler = std::function<void(const WsSessionPtr&, const WsMessage&)>;
using CloseHandler = std::function<void(const WsSessionPtr&)>;
using ErrorHandler = std::function<void(const WsSessionPtr&, const std::string&)>;
using HandshakeValidator = std::function<bool(const TcpConnectionPtr&,
    const std::string& path, const std::map<std::string, std::string>& headers)>;
```

**完整公开 API**：
```cpp
WebSocketServer(EventLoop* loop, const InetAddress& addr,
                const std::string& name = "WebSocketServer");
~WebSocketServer() = default;

// 配置
void setThreadNum(int num);
void setConfig(const WebSocketConfig& config);

// 启动
void start();

// 广播
void broadcast(const std::string& message);            // 广播文本
void broadcastBinary(const std::vector<uint8_t>& data);// 广播二进制

// 回调设置
void setConnectionHandler(ConnectionHandler handler);
void setMessageHandler(MessageHandler handler);
void setCloseHandler(CloseHandler handler);
void setErrorHandler(ErrorHandler handler);
void setHandshakeValidator(HandshakeValidator validator);// 自定义握手验证

// 会话管理
std::vector<WsSessionPtr> getAllSessions() const;
size_t sessionCount() const;
```

---

## 五、服务注册中心 (registry/)

### 5.1 ServiceKey / InstanceMeta — 服务元数据

**文件**：`src/registry/ServiceMeta.h`

**ServiceKey** — 服务唯一标识：
```cpp
struct ServiceKey {
    std::string namespace_;                // 命名空间
    std::string serviceName;               // 服务名
    std::string version;                   // 版本号

    ServiceKey() = default;
    ServiceKey(const std::string& ns, const std::string& name, const std::string& ver);

    std::string key() const;               // 返回 "namespace:name:version"
    bool operator==(const ServiceKey& other) const;
    bool operator<(const ServiceKey& other) const;
    json toJson() const;
    static ServiceKey fromJson(const json& j);
};
```

**InstanceMeta** — 服务实例信息：
```cpp
struct InstanceMeta {
    std::string instanceId;                // 实例 ID
    std::string host;                      // 主机地址
    int port;                              // 端口
    int weight;                            // 权重（负载均衡用）
    int64_t lastHeartbeatMs;               // 最近心跳时间（毫秒时间戳）
    int ttlSeconds;                        // 生存时间（秒）
    std::string status;                    // 状态："UP" / "DOWN"
    std::map<std::string, std::string> metadata;  // 自定义元数据

    InstanceMeta();
    InstanceMeta(const std::string& id, const std::string& h, int p);

    std::string address() const;           // 返回 "host:port"
    void heartbeat();                      // 更新心跳时间
    bool isExpired() const;                // 是否已过期
    int64_t remainingTtlMs() const;        // 剩余存活时间
    json toJson() const;
    static InstanceMeta fromJson(const json& j);
};
```

**ServiceInstance** — 完整服务实例记录：
```cpp
struct ServiceInstance {
    ServiceKey serviceKey;
    InstanceMeta instance;

    ServiceInstance() = default;
    ServiceInstance(const ServiceKey& key, const InstanceMeta& inst);

    json toJson() const;
    static ServiceInstance fromJson(const json& j);
};
```

---

### 5.2 ServiceCatalog — 服务目录

**文件**：`src/registry/ServiceCatalog.h`

**类型定义**：
```cpp
using InstancePtr = std::shared_ptr<InstanceMeta>;
```

**统计信息**：
```cpp
struct Stats {
    size_t totalServices;
    size_t totalInstances;
    size_t healthyInstances;
    size_t expiredInstances;
};
```

**完整公开 API**：
```cpp
// 注册与注销
bool registerInstance(const ServiceKey& key, const InstancePtr& instance);
bool deregisterInstance(const ServiceKey& key, const std::string& instanceId);
bool heartbeat(const ServiceKey& key, const std::string& instanceId);

// 服务发现
std::vector<InstancePtr> discover(const ServiceKey& key) const;
std::vector<ServiceKey> discoverByNamespace(const std::string& ns) const;
std::map<ServiceKey, std::vector<InstancePtr>> getAllServices() const;

// 健康管理
int cleanExpiredInstances();               // 清除过期实例
int markExpiredInstancesDown();            // 标记过期实例为 DOWN

// 统计与管理
Stats getStats() const;
void clear();
```

---

### 5.3 HealthChecker — 健康检查器

**文件**：`src/registry/HealthChecker.h`

```cpp
using HealthCheckCallback = std::function<void(const std::vector<std::string>& expiredInstances)>;

HealthChecker(ServiceCatalog* catalog);
~HealthChecker();

void setCheckInterval(int ms);            // 设置检查间隔（毫秒）
void setExpiredCallback(HealthCheckCallback cb);  // 过期回调
void start();                             // 启动后台检查线程
void stop();                              // 停止检查
int checkOnce();                          // 手动执行一次检查
int cleanExpired();                       // 清理过期实例
bool isRunning() const;
```

---

### 5.4 RegistryServer — 注册中心服务器

**文件**：`src/registry/RegistryServer.h`

```cpp
RegistryServer(EventLoop* loop, const InetAddress& addr,
               const std::string& name = "RegistryServer");
~RegistryServer();

void setThreadNum(int num);
void start();
ServiceCatalog* catalog();                // 获取服务目录实例
ServiceCatalog::Stats getStats() const;
```

**REST API 端点**：

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/api/v1/registry/register` | 注册服务实例 |
| POST | `/api/v1/registry/deregister` | 注销服务实例 |
| POST | `/api/v1/registry/heartbeat` | 发送心跳 |
| GET | `/api/v1/registry/discover` | 发现服务实例 |
| GET | `/api/v1/registry/services` | 列出所有服务 |
| GET | `/api/v1/registry/stats` | 获取统计信息 |

---

### 5.5 RegistryClient — 注册中心客户端 SDK

**文件**：`src/registry/RegistryClient.h`

**辅助类 SimpleHttpClient**：
```cpp
struct Response {
    int statusCode;
    std::string body;
    bool success;
};

static Response post(const std::string& host, int port,
                     const std::string& path, const std::string& body,
                     const std::string& contentType = "application/json");
static Response get(const std::string& host, int port, const std::string& path);
```

**RegistryClient 完整公开 API**：
```cpp
using ServiceChangedCallback = std::function<void(const ServiceKey&,
    const std::vector<InstancePtr>&)>;

RegistryClient(const std::string& registryHost, int registryPort);
~RegistryClient();

// 服务注册
bool registerService(const ServiceKey& key, const InstanceMeta& instance,
                     std::string* outInstanceId = nullptr);
bool deregisterService(const ServiceKey& key, const std::string& instanceId);

// 心跳
bool sendHeartbeat(const ServiceKey& key, const std::string& instanceId);
void startHeartbeat(const ServiceKey& key, const std::string& instanceId);
void setHeartbeatInterval(int ms);

// 服务发现
std::vector<InstancePtr> discoverService(const ServiceKey& key);
InstancePtr selectInstance(const ServiceKey& key,
    LoadBalancer::Strategy strategy = LoadBalancer::Strategy::RoundRobin);

// 健康检查
bool isRegistryHealthy();

// 停止
void stop();
```

---

## 六、负载均衡模块 (balancer/)

**文件**：`src/balancer/LoadBalancer.h`

### 6.1 BackendServer — 后端服务器描述

```cpp
struct BackendServer {
    std::string host;
    int port;
    int weight;                            // 权重（默认 1）
    int currentWeight;                     // 当前权重（加权轮询用）
    int connections;                       // 当前连接数（最少连接策略用）
    bool healthy;                          // 是否健康（默认 true）

    BackendServer(const std::string& h, int p, int w = 1);
    std::string address() const;           // 返回 "host:port"
};

using BackendServerPtr = std::shared_ptr<BackendServer>;
```

### 6.2 ILoadBalanceStrategy — 策略接口

```cpp
class ILoadBalanceStrategy {
public:
    virtual ~ILoadBalanceStrategy() = default;
    virtual BackendServerPtr select(const std::vector<BackendServerPtr>& servers) = 0;
    virtual std::string name() const = 0;
};
```

### 6.3 五种负载均衡策略

| 策略类 | 算法 | 适用场景 |
|--------|------|---------|
| `RoundRobinStrategy` | 顺序轮询 | 服务器性能相近 |
| `WeightedRoundRobinStrategy` | 平滑加权轮询（Nginx 算法） | 服务器性能不同 |
| `LeastConnectionsStrategy` | 最少活跃连接 | 长连接场景 |
| `RandomStrategy` | 随机选择 | 简单场景 |
| `ConsistentHashStrategy` | 一致性哈希（默认 150 虚拟节点） | 缓存场景，减少重分布 |

**LeastConnectionsStrategy 特有方法**：
```cpp
void release(BackendServerPtr server);    // 释放连接（connections--）
```

**ConsistentHashStrategy 特有方法**：
```cpp
ConsistentHashStrategy(int virtualNodes = 150);
void init(const std::vector<BackendServerPtr>& servers);
BackendServerPtr selectWithKey(uint32_t key);  // 按 key 选择节点
```

### 6.4 LoadBalancer — 负载均衡器

**策略枚举**：
```cpp
enum class Strategy { RoundRobin, WeightedRoundRobin, LeastConnections, Random, ConsistentHash };
```

**完整公开 API**：
```cpp
LoadBalancer(Strategy strategy = Strategy::RoundRobin);

void addServer(const std::string& host, int port, int weight = 1);
void removeServer(const std::string& host, int port);
BackendServerPtr select();                // 根据策略选择服务器
void releaseConnection(BackendServerPtr server);  // 释放连接（最少连接策略）
void setServerHealth(const std::string& host, int port, bool healthy);

std::vector<BackendServerPtr> servers() const;
std::string strategyName() const;
```

---

## 七、定时器模块 (timer/)

### 7.1 Timer — 定时器对象

**文件**：`src/timer/Timer.h`

```cpp
using TimerCallback = std::function<void()>;

Timer(TimerCallback cb, int64_t when, int64_t interval = 0);

void run() const;                         // 执行回调
void cancel();                            // 取消定时器
void restart(int64_t now);                // 重新设置到期时间

int64_t expiration() const;               // 到期时间
bool repeat() const;                      // 是否周期定时器
int64_t id() const;                       // 定时器唯一 ID
int64_t interval() const;                 // 周期间隔
bool isCancelled() const;                 // 是否已取消

static int64_t now();                     // 当前毫秒时间戳

// C++17 inline static
inline static std::atomic<int64_t> nextId_{0};  // 全局 ID 生成器
```

### 7.2 TimerQueue — 时间轮

**文件**：`src/timer/TimerQueue.h`

```cpp
explicit TimerQueue(size_t buckets = 60, int tickMs = 1000);

int64_t addTimer(TimerCallback cb, int delayMs, int intervalMs = 0);
void cancelTimer(int64_t timerId);
void tick();                              // 推进时间轮一格
int getNextTimeout() const;              // 距离下次到期的毫秒数
size_t timerCount() const;              // 当前定时器数量
```

**算法复杂度**：
- 添加定时器：O(1)
- 取消定时器：O(1)
- 触发到期定时器：均摊 O(1)

---

## 八、连接池模块 (pool/)

**文件**：`src/pool/ConnectionPool.h`

### 8.1 Connection — 单个连接

```cpp
using Ptr = std::shared_ptr<Connection>;

Connection(int fd, const std::string& host, int port);
~Connection();                            // close(fd)

int fd() const;
bool valid() const;                       // 连接是否有效
int64_t lastUsed() const;                // 最后使用时间戳
void markUsed();                          // 更新最后使用时间

ssize_t send(const void* buf, size_t len);
ssize_t recv(void* buf, size_t len);
void setNonBlocking();
```

### 8.2 ConnectionPool — 连接池

```cpp
ConnectionPool(const std::string& host, int port,
               size_t minSize = 5, size_t maxSize = 20);
~ConnectionPool();

Connection::Ptr acquire(int timeoutMs = 5000);  // 获取连接（超时等待）
void release(Connection::Ptr conn);              // 归还连接
void healthCheck();                              // 清理无效/空闲连接
size_t available() const;                        // 当前可用连接数
size_t totalCreated() const;                     // 累计创建连接数
bool isClosed() const;                           // 连接池是否已关闭
```

---

## 九、异步日志模块 (asynclogger/)

**文件**：`src/asynclogger/AsyncLogger.h`

### 9.1 日志级别与日志条目

```cpp
enum class LogLevel { DEBUG, INFO, WARN, ERROR, FATAL };

struct LogEntry {
    LogLevel level;
    std::string timestamp;
    std::string threadName;
    std::string file;
    int line;
    std::string message;
};
```

### 9.2 AsyncLogger — 异步日志器

```cpp
static constexpr size_t kFlushThreshold = 1000;  // 缓冲区条目数阈值

static AsyncLogger& instance();           // 单例

// 配置
void setLogFile(const std::string& filename);
void setLogLevel(LogLevel level);

// 控制
void start();                             // 启动后台刷盘线程
void stop();                              // 停止并刷空缓冲区

// 日志写入
void log(LogLevel level, const char* file, int line, const char* fmt, ...);
```

**日志宏**：
```cpp
ASYNC_LOG_DEBUG(fmt, ...)
ASYNC_LOG_INFO(fmt, ...)
ASYNC_LOG_WARN(fmt, ...)
ASYNC_LOG_ERROR(fmt, ...)
ASYNC_LOG_FATAL(fmt, ...)
```

**双缓冲机制**：
- 前端线程写入 `currentBuffer_`
- 当缓冲区满或定时到期，与 `nextBuffer_` 交换
- 后台线程将满缓冲区写入磁盘
- 交换过程仅需 `swap` 指针，最小化锁竞争

---

## 十、配置模块 (config/)

**文件**：`src/config/Config.h`

### 10.1 ConfigValue — 配置值包装

```cpp
explicit ConfigValue(const std::string& value);

std::string asString() const;
int asInt() const;
int64_t asInt64() const;
double asDouble() const;
bool asBool() const;
std::vector<std::string> asList(char delim = ',') const;
```

### 10.2 Config — 配置管理器

**支持格式**：INI
```ini
[section]
key = value
```

**完整公开 API**：
```cpp
static Config& instance();               // 单例

bool load(const std::string& filename);   // 加载配置文件
bool reload();                            // 重新加载
ConfigValue get(const std::string& key, const std::string& defaultValue = "");
void set(const std::string& key, const std::string& value);
bool has(const std::string& key) const;
```

**便捷宏**：
```cpp
CONFIG(key)                               // 获取 ConfigValue
CONFIG_INT(key)                           // 获取 int
CONFIG_STRING(key)                        // 获取 string
CONFIG_BOOL(key)                          // 获取 bool
CONFIG_DOUBLE(key)                        // 获取 double
```

---

## 十一、工具模块 (util/)

**文件**：`src/util/SignalHandler.h`

### 11.1 SignalHandler — 信号处理

```cpp
using SignalCallback = std::function<void(int)>;

static SignalHandler& instance();         // 单例

void registerHandler(int signum, SignalCallback cb);  // 注册信号回调
void ignore(int signum);                              // 忽略指定信号
void start();                                         // 启动信号处理

static void setupGracefulExit(std::function<void()> onExit);  // 设置优雅退出
```

### 11.2 Signals — 常用信号操作

```cpp
static void ignorePipe();                // 忽略 SIGPIPE
static void gracefulExit(std::function<void()> onExit);  // 设置 SIGINT/SIGTERM 优雅退出
```

---

## 模块依赖关系

```
应用层
┌─────────────────────────────────────────────────────┐
│  HttpServer    RpcServer/Pb    WebSocketServer      │
│  RegistryServer                RegistryClient       │
└─────────────┬───────────────────────────────────────┘
              │
中间层        │
┌─────────────┴───────────────────────────────────────┐
│  LoadBalancer    TimerQueue    ConnectionPool        │
│  AsyncLogger     Config       SignalHandler          │
└─────────────┬───────────────────────────────────────┘
              │
基础层        │
┌─────────────┴───────────────────────────────────────┐
│  TcpServer ← Acceptor + EventLoopThreadPool         │
│  TcpConnection ← Channel + Buffer + Socket          │
│  EventLoop ← EPollPoller + Channel                  │
│  InetAddress    Timestamp    Thread                  │
└─────────────────────────────────────────────────────┘
```

---

## 快速使用示例

### TCP Echo 服务器

```cpp
EventLoop loop;
InetAddress addr(8080);
TcpServer server(&loop, addr, "EchoServer");

server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
    conn->send(buf->retrieveAllAsString());
});

server.setThreadNum(4);
server.start();
loop.loop();
```

### HTTP 服务器

```cpp
EventLoop loop;
InetAddress addr(8080);
HttpServer server(&loop, addr);

server.GET("/", [](const HttpRequest&, HttpResponse& resp) {
    resp.setHtml("<h1>Hello World</h1>");
});

server.POST("/api/users", [](const HttpRequest& req, HttpResponse& resp) {
    resp.setJson(R"({"success": true})");
});

server.serveStatic("/static", "./public");
server.setThreadNum(4);
server.start();
loop.loop();
```

### Protobuf-RPC 服务

```cpp
// 服务端
EventLoop loop;
InetAddress addr(8081);
RpcServerPb server(&loop, addr);

server.registerMethod<AddRequest, AddResponse>(
    "Calculator", "Add",
    [](const AddRequest& req, AddResponse& resp) {
        resp.set_result(req.a() + req.b());
    });

server.setThreadNum(4);
server.start();
loop.loop();
```

```cpp
// 客户端
RpcClientPb client("127.0.0.1", 8081);
client.connect();

AddRequest req;
req.set_a(10);
req.set_b(20);
AddResponse resp;

client.call("Calculator", "Add", req, resp);
// resp.result() == 30
```

### WebSocket 服务器

```cpp
EventLoop loop;
InetAddress addr(9090);
WebSocketServer server(&loop, addr);

server.setMessageHandler([](const WsSessionPtr& session, const WsMessage& msg) {
    session->sendText("Echo: " + msg.text());
});

server.setConnectionHandler([](const WsSessionPtr& session) {
    session->sendText("Welcome!");
});

WebSocketConfig config;
config.pingIntervalMs = 30000;
server.setConfig(config);

server.setThreadNum(4);
server.start();
loop.loop();
```

### 服务注册与发现

```cpp
// 注册中心服务端
EventLoop loop;
InetAddress addr(8500);
RegistryServer registry(&loop, addr);
registry.start();
loop.loop();
```

```cpp
// 客户端注册 + 发现
RegistryClient client("127.0.0.1", 8500);

ServiceKey key("default", "user-service", "v1.0.0");
InstanceMeta instance("inst-001", "192.168.1.100", 8080);
client.registerService(key, instance);
client.startHeartbeat(key, "inst-001");

auto instances = client.discoverService(key);
auto selected = client.selectInstance(key, LoadBalancer::Strategy::RoundRobin);
```
