# mymuduo-http 项目文档

> 基于 mymuduo 网络库的高性能 HTTP 服务器和 RPC 框架

---

## 目录

1. [项目概述](#1-项目概述)
2. [架构设计](#2-架构设计)
3. [模块详解](#3-模块详解)
4. [编译与运行](#4-编译与运行)
5. [性能数据](#5-性能数据)
6. [后续规划](#6-后续规划)

---

## 1. 项目概述

### 1.1 简介

mymuduo-http 是一个基于 Reactor 模式的高性能网络应用框架，提供 HTTP 服务器和 RPC 框架功能。

### 1.2 核心特性

| 特性 | 说明 |
|------|------|
| HTTP/1.1 服务器 | Keep-Alive、路由分发、静态文件 |
| JSON-RPC 2.0 | 轻量级 RPC，易于调试 |
| Protobuf-RPC | 高性能二进制协议，快 5-10x |
| 时间轮定时器 | O(1) 复杂度，支持百万级定时器 |
| 异步日志 | 双缓冲技术，不阻塞业务 |
| 连接池 | TCP 连接复用，减少开销 |
| 配置管理 | INI 格式，支持热加载 |

### 1.3 技术栈

- **语言**: C++17
- **网络**: epoll (Linux)
- **序列化**: JSON (nlohmann/json), Protobuf
- **并发**: 多线程、无锁编程

---

## 2. 架构设计

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐  │
│  │ HTTP Server │  │ JSON-RPC    │  │ Protobuf-RPC    │  │
│  └─────────────┘  └─────────────┘  └─────────────────┘  │
├─────────────────────────────────────────────────────────┤
│                     Service Layer                        │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐  │
│  │  Router     │  │ Middleware  │  │ Service Registry│  │
│  └─────────────┘  └─────────────┘  └─────────────────┘  │
├─────────────────────────────────────────────────────────┤
│                    Network Layer                         │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐  │
│  │  TcpServer  │  │  Buffer     │  │  Connection Pool│  │
│  └─────────────┘  └─────────────┘  └─────────────────┘  │
├─────────────────────────────────────────────────────────┤
│                    Infrastructure                        │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐  │
│  │  Timer      │  │ AsyncLogger │  │  Config         │  │
│  └─────────────┘  └─────────────┘  └─────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

### 2.2 网络模型

采用 **one loop per thread + 线程池** 模型：

```
                    ┌──────────────┐
                    │  mainLoop    │ ← Accept connections
                    └──────┬───────┘
                           │
         ┌─────────────────┼─────────────────┐
         │                 │                 │
    ┌────▼────┐      ┌────▼────┐      ┌────▼────┐
    │ subLoop1│      │ subLoop2│      │ subLoop3│
    │ (epoll) │      │ (epoll) │      │ (epoll) │
    └─────────┘      └─────────┘      └─────────┘
```

### 2.3 目录结构

```
mymuduo-http/
├── src/                      # 源代码
│   ├── http/                 # HTTP 模块
│   │   ├── HttpRequest.h     #   请求解析
│   │   ├── HttpResponse.h    #   响应构造
│   │   └── HttpServer.h      #   HTTP 服务器
│   ├── rpc/                  # RPC 模块
│   │   ├── RpcServer.h       #   JSON-RPC 服务端
│   │   ├── RpcClient.h       #   JSON-RPC 客户端
│   │   ├── RpcServerPb.h     #   Protobuf-RPC 服务端
│   │   ├── RpcClientPb.h     #   Protobuf-RPC 客户端
│   │   └── proto/            #   Protobuf 协议
│   ├── timer/                # 定时器
│   │   ├── Timer.h           #   定时器对象
│   │   └── TimerQueue.h      #   时间轮
│   ├── asynclogger/          # 日志
│   │   └── AsyncLogger.h     #   异步日志
│   ├── pool/                 # 连接池
│   │   └── ConnectionPool.h  #   TCP 连接池
│   ├── util/                 # 工具
│   │   └── SignalHandler.h   #   信号处理
│   └── config/               # 配置
│       └── Config.h          #   配置管理
├── examples/                 # 示例代码
├── config/                   # 配置文件
├── docs/                     # 文档
└── CMakeLists.txt           # 构建配置
```

---

## 3. 模块详解

### 3.1 HTTP 模块

#### HttpRequest.h - HTTP 请求解析

**功能**: 解析 HTTP 请求行、请求头、请求体

**关键技术**:
- 支持 GET/POST/PUT/DELETE 方法
- 解析查询字符串 (Query String)
- 提取 Content-Length、Connection 等头信息

```cpp
class HttpRequest {
    HttpMethod method;           // GET/POST/PUT/DELETE
    HttpVersion version;         // HTTP/1.0 or HTTP/1.1
    std::string path;            // 请求路径
    std::string query;           // 查询字符串
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> params;
    std::string body;
    
    bool parseRequestLine(const std::string& line);  // 解析请求行
    bool parseHeader(const std::string& line);       // 解析请求头
    bool keepAlive() const;                          // 是否保持连接
};
```

#### HttpResponse.h - HTTP 响应构造

**功能**: 构造 HTTP 响应，支持多种内容类型

**关键技术**:
- 支持常见状态码 (200/400/404/500)
- 自动设置 Content-Length
- Keep-Alive 支持

```cpp
class HttpResponse {
    HttpStatusCode statusCode;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    
    void setJson(const std::string& json);    // JSON 响应
    void setHtml(const std::string& html);    // HTML 响应
    void setText(const std::string& text);    // 文本响应
    std::string toString() const;              // 序列化
};
```

#### HttpServer.h - HTTP 服务器

**功能**: 高性能 HTTP 服务器，基于 TcpServer

**关键技术**:
- 路由注册 (正则匹配)
- Keep-Alive 连接复用
- 静态文件服务
- 中间件支持

```cpp
class HttpServer {
    void GET(const std::string& path, HttpHandler handler);
    void POST(const std::string& path, HttpHandler handler);
    void serveStatic(const std::string& urlPrefix, const std::string& dir);
    void use(HttpHandler middleware);  // 中间件
};
```

---

### 3.2 RPC 模块

#### JSON-RPC (RpcServer.h / RpcClient.h)

**协议**: JSON-RPC 2.0

**特点**:
- 易于调试（文本协议）
- 跨语言支持
- 同步/异步调用

```cpp
// 服务端
server.registerMethod("add", [](const json& params) {
    return {{"result", params["a"].get<int>() + params["b"].get<int>()}};
});

// 客户端
json result = client.call("add", {{"a", 1}, {"b", 2}});
```

#### Protobuf-RPC (RpcServerPb.h / RpcClientPb.h)

**协议**: 自定义二进制协议

**特点**:
- 高性能（比 JSON 快 5-10x）
- 类型安全
- 数据紧凑（小 3-5x）

**消息格式**:
```protobuf
message RpcRequest {
    string service = 1;    // 服务名
    string method = 2;     // 方法名
    bytes params = 3;      // 参数
    int64 id = 4;          // 请求 ID
}
```

**传输格式**:
```
+----------------+----------------+
|  Length (4B)   |  RpcRequest   |
+----------------+----------------+
```

---

### 3.3 定时器模块

#### Timer.h - 定时器对象

```cpp
class Timer {
    TimerCallback callback_;   // 回调函数
    int64_t expiration_;       // 到期时间 (ms)
    int64_t interval_;         // 间隔 (0=一次性)
    bool repeat_;              // 是否重复
};
```

#### TimerQueue.h - 时间轮

**算法**: 时间轮 (Timing Wheel)

**复杂度**:
- 添加定时器: O(1)
- 删除定时器: O(1)
- 触发定时器: O(n/m), n=活跃定时器数, m=桶数

**实现原理**:
```
时间轮 (60 个桶，每桶 1 秒):

  [0] → Timer1 → Timer2
  [1] → Timer3
  [2] → empty
  ...
  [59] → TimerN
   ↑
 currentBucket
```

```cpp
class TimerQueue {
    int64_t addTimer(TimerCallback cb, int delayMs, int intervalMs = 0);
    void cancelTimer(int64_t timerId);
    void tick();  // 推进时间轮
};
```

---

### 3.4 异步日志模块

#### AsyncLogger.h - 异步日志器

**技术**: 双缓冲 (Double Buffering)

**工作流程**:
```
业务线程              后台线程
    │                    │
    ▼                    ▼
[Buffer A]          [Buffer B]
    │                    │
    │  交换缓冲区         │
    ├────────────────────▶
    │                    │
    │                写入文件
```

**特点**:
- 不阻塞业务线程
- 批量写入，减少 IO
- 支持多级别 (DEBUG/INFO/WARN/ERROR/FATAL)

```cpp
AsyncLogger::instance().setLogFile("/var/log/app.log");
AsyncLogger::instance().start();

LOG_INFO("Server started on port %d", 8080);
LOG_ERROR("Connection failed: %s", strerror(errno));
```

---

### 3.5 连接池模块

#### ConnectionPool.h - TCP 连接池

**功能**: 复用 TCP 连接，减少连接开销

**特性**:
- 最小/最大连接数
- 自动扩容/缩容
- 健康检查（清理空闲连接）

```cpp
ConnectionPool pool("127.0.0.1", 3306, 5, 20);  // min=5, max=20

auto conn = pool.acquire();  // 获取连接
conn->send("PING", 4);
pool.release(conn);          // 归还连接

pool.healthCheck();          // 健康检查
```

---

### 3.6 配置管理模块

#### Config.h - 配置管理

**格式**: INI 格式

**特性**:
- 支持分区 (section)
- 类型转换 (int/double/bool/string/list)
- 热加载 (reload)

**配置文件示例**:
```ini
[server]
port = 8080
threads = 4
timeout = 60

[log]
level = INFO
file = /var/log/mymuduo-http.log
```

**使用**:
```cpp
Config::instance().load("server.conf");
int port = CONFIG_INT("server.port");
```

---

### 3.7 信号处理模块

#### SignalHandler.h - 信号处理

**功能**:
- 优雅退出 (SIGINT/SIGTERM)
- 忽略 SIGPIPE（防止写入已关闭 socket 导致进程退出）

```cpp
Signals::ignorePipe();  // 忽略 SIGPIPE
Signals::gracefulExit([]() {
    // 清理资源
    server.stop();
});
```

---

## 4. 编译与运行

### 4.1 依赖

- C++17 编译器 (GCC 7+, Clang 5+)
- CMake 3.10+
- Protobuf 3.0+
- nlohmann/json (自动下载)

### 4.2 编译

```bash
# 克隆
git clone https://github.com/chunluren/mymuduo-http
cd mymuduo-http

# 编译
mkdir build && cd build
cmake ..
make -j4
```

### 4.3 运行

```bash
# HTTP 服务器
./http_server

# JSON-RPC 服务器
./rpc_server

# Protobuf-RPC 服务器
./rpc_server_pb

# 完整版（使用所有模块）
./full_server ../config/server.conf
```

### 4.4 测试

```bash
# 测试 HTTP
curl http://localhost:8080/
curl http://localhost:8080/api/time

# 测试 JSON-RPC
curl -X POST http://localhost:8081/rpc \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"calc.add","params":{"a":1,"b":2},"id":1}'

# 测试 Protobuf-RPC
./rpc_client_pb
```

---

## 5. 性能数据

### 5.1 测试环境

- CPU: 12th Gen Intel i5-12400F
- OS: Linux (WSL2)
- 编译选项: -O2

### 5.2 HTTP 性能

| 测试项 | QPS | 说明 |
|--------|-----|------|
| Echo (12B) | ~20,000 | 4 线程 |
| Hello World | ~25,000 | 简单响应 |
| JSON API | ~15,000 | JSON 序列化 |

### 5.3 RPC 性能

| 协议 | QPS | 说明 |
|------|-----|------|
| JSON-RPC | ~15,000 | JSON 序列化开销 |
| Protobuf-RPC | ~50,000+ | 二进制协议更快 |

### 5.4 定时器性能

| 操作 | 复杂度 | 说明 |
|------|--------|------|
| 添加 | O(1) | 时间轮 |
| 删除 | O(1) | hash map |
| 触发 | O(n/60) | n=活跃定时器 |

---

## 6. 后续规划

### 6.1 规划功能

| 功能 | 优先级 | 状态 | 说明 |
|------|--------|------|------|
| ~~负载均衡~~ | P0 | ✅ 已完成 | 轮询、加权、最小连接数、一致性哈希 |
| ~~服务注册中心~~ | P0 | ✅ 已完成 | 服务发现、健康检查、TTL 过期 |
| ~~WebSocket~~ | P1 | ✅ 已完成 | RFC 6455、帧编解码、会话管理 |
| ~~性能压测~~ | P1 | ✅ 已完成 | QPS、延迟分位、报告生成 |
| 限流 | P1 | 📋 待开发 | 令牌桶、漏桶 |
| 熔断降级 | P1 | 📋 待开发 | Circuit Breaker |
| 单元测试 | P2 | 📋 待开发 | Google Test |
| Docker 化 | P2 | 📋 待开发 | 容器部署 |
| CI/CD | P2 | 📋 待开发 | GitHub Actions |

### 6.2 版本规划

```
v1.0 (当前)
├── HTTP/1.1 服务器 ✓
├── JSON-RPC ✓
├── Protobuf-RPC ✓
├── 定时器 ✓
├── 异步日志 ✓
├── 连接池 ✓
├── 负载均衡 ✓
├── 服务注册中心 ✓
├── WebSocket ✓
└── 性能压测 ✓

v1.1 (计划中)
├── 限流
├── 熔断降级
└── 单元测试完善

v2.0 (远期)
├── HTTP/2
├── gRPC 兼容
└── 分布式追踪
```

---

## 附录

### A. 参考资源

- [muduo 网络库](https://github.com/chenshuo/muduo)
- [Protocol Buffers](https://protobuf.dev/)
- [JSON-RPC 2.0](https://www.jsonrpc.org/specification)

### B. 作者

- GitHub: https://github.com/chunluren

### C. License

MIT

---

*最后更新: 2026-03-14*