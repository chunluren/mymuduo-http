# mymuduo-http

基于 mymuduo 网络库的高性能 HTTP 服务器和 RPC 框架

## 特性

### 核心功能
- ✅ HTTP/1.1 服务器
- ✅ JSON-RPC 2.0 框架
- ✅ **Protobuf-RPC** - 高性能二进制协议
- ✅ Keep-Alive 连接复用
- ✅ 路由注册 (GET/POST/PUT/DELETE)
- ✅ 正则路由匹配

### 网络库增强
- ✅ **时间轮定时器** - O(1) 复杂度，支持百万级定时器
- ✅ **异步日志** - 双缓冲，不阻塞业务线程
- ✅ **连接池** - 复用 TCP 连接，减少开销
- ✅ **信号处理** - 优雅退出，忽略 SIGPIPE
- ✅ **配置管理** - INI 格式配置文件
- ✅ **负载均衡** - 多种策略：轮询、加权轮询、最小连接数、一致性哈希
- ✅ **服务注册中心** - 分布式服务发现与注册
- ✅ **WebSocket** - RFC 6455 标准 WebSocket 服务器

## 项目结构

```
mymuduo-http/
├── src/
│   ├── http/           # HTTP 模块
│   ├── rpc/            # RPC 模块
│   ├── timer/          # 定时器
│   ├── asynclogger/    # 异步日志
│   ├── pool/           # 连接池
│   ├── balancer/       # 负载均衡
│   ├── registry/       # 服务注册中心
│   ├── websocket/      # WebSocket
│   ├── util/           # 工具类
│   └── config/         # 配置管理
├── examples/           # 示例代码
├── benchmark/          # 性能压测
├── tests/              # 单元测试
├── config/             # 配置文件
└── CMakeLists.txt
```

## 快速开始

```bash
# 克隆
git clone https://github.com/chunluren/mymuduo-http
cd mymuduo-http

# 编译
mkdir build && cd build
cmake ..
make -j4

# 运行 HTTP 服务器
./http_server

# 运行 RPC 服务器
./rpc_server

# 运行完整版（使用所有模块）
./full_server ../config/server.conf
```

## HTTP 使用

```cpp
#include "HttpServer.h"

int main() {
    EventLoop loop;
    HttpServer server(&loop, InetAddress(8080));
    
    server.GET("/", [](const HttpRequest& req, HttpResponse& resp) {
        resp.setHtml("<h1>Hello</h1>");
    });
    
    server.GET("/api/time", [](const HttpRequest& req, HttpResponse& resp) {
        resp.json("{\"time\": \"now\"}");
    });
    
    server.start();
    loop.loop();
}
```

## RPC 使用

### JSON-RPC

#### 服务端

```cpp
#include "RpcServer.h"

int main() {
    EventLoop loop;
    RpcServer server(&loop, InetAddress(8081));
    
    server.registerMethod("add", [](const json& params) {
        return {{"result", params["a"].get<int>() + params["b"].get<int>()}};
    });
    
    server.start();
    loop.loop();
}
```

### 客户端

```cpp
#include "RpcClient.h"

int main() {
    RpcClient client("127.0.0.1", 8081);
    json result = client.call("add", {{"a", 1}, {"b", 2}});
    // result = {"result": 3}
}
```

### Protobuf-RPC（高性能）

#### 定义协议 (proto/rpc.proto)

```protobuf
syntax = "proto3";
package rpc;

message CalcRequest {
    double a = 1;
    double b = 2;
}

message CalcResponse {
    double result = 1;
}
```

#### 服务端

```cpp
#include "RpcServerPb.h"

int main() {
    EventLoop loop;
    RpcServerPb server(&loop, InetAddress(8082));
    
    server.registerMethod<CalcRequest, CalcResponse>(
        "calc", "add",
        [](const CalcRequest& req, CalcResponse& resp) {
            resp.set_result(req.a() + req.b());
        });
    
    server.start();
    loop.loop();
}
```

#### 客户端

```cpp
#include "RpcClientPb.h"

int main() {
    RpcClientPb client("127.0.0.1", 8082);
    
    CalcRequest req;
    req.set_a(10);
    req.set_b(5);
    
    CalcResponse resp;
    client.call<CalcRequest, CalcResponse>("calc", "add", req, resp);
    // resp.result() == 15
}
```

**Protobuf vs JSON 性能对比：**
| 协议 | 序列化速度 | 数据大小 | 类型安全 |
|------|-----------|----------|----------|
| JSON | 慢 | 大 | 弱 |
| Protobuf | 快 5-10x | 小 3-5x | 强 |

## 定时器

```cpp
TimerQueue timers;

// 一次性定时器
timers.addTimer([]() {
    std::cout << "Timer fired!" << std::endl;
}, 5000);  // 5秒后

// 重复定时器
timers.addTimer([]() {
    std::cout << "Heartbeat" << std::endl;
}, 1000, 1000);  // 1秒后开始，每1秒

// 推进时间轮
timers.tick();
```

## 异步日志

```cpp
// 启动
AsyncLogger::instance().setLogFile("/var/log/app.log");
AsyncLogger::instance().start();

// 使用
LOG_INFO("Server started on port %d", 8080);
LOG_ERROR("Connection failed: %s", strerror(errno));

// 停止
AsyncLogger::instance().stop();
```

## 连接池

```cpp
// 创建连接池
ConnectionPool pool("127.0.0.1", 3306, 5, 20);  // min=5, max=20

// 获取连接
auto conn = pool.acquire();
if (conn) {
    conn->send("PING", 4);
    // ...
    pool.release(conn);  // 归还
}

// 健康检查（清理空闲连接）
pool.healthCheck();
```

## 配置文件

```ini
[server]
port = 8080
threads = 4
timeout = 60

[log]
level = INFO
file = /var/log/mymuduo-http.log
```

```cpp
Config::instance().load("server.conf");
int port = CONFIG_INT("server.port");
```

## 性能

- HTTP QPS: ~20,000+ (echo, 4 threads)
- JSON-RPC QPS: ~15,000+
- **Protobuf-RPC QPS: ~50,000+** (二进制协议更快)
- 定时器: O(1) 插入/删除

## 负载均衡

支持多种负载均衡策略，可用于客户端请求分发：

```cpp
#include "balancer/LoadBalancer.h"

// 创建负载均衡器
LoadBalancer lb(LoadBalancer::Strategy::WeightedRoundRobin);

// 添加后端服务器（host, port, weight）
lb.addServer("192.168.1.1", 8080, 5);  // 权重 5
lb.addServer("192.168.1.2", 8080, 3);  // 权重 3
lb.addServer("192.168.1.3", 8080, 2);  // 权重 2

// 选择服务器
auto server = lb.select();
std::cout << "Selected: " << server->address() << std::endl;

// 释放连接（最小连接数策略）
lb.releaseConnection(server);
```

### 策略说明

| 策略 | 说明 | 适用场景 |
|------|------|---------|
| RoundRobin | 轮询 | 服务器性能相近 |
| WeightedRoundRobin | 平滑加权轮询 | 服务器性能不均 |
| LeastConnections | 最小连接数 | 长连接场景 |
| Random | 随机选择 | 简单场景 |
| ConsistentHash | 一致性哈希 | 缓存场景 |

## 性能压测

项目提供多种压测工具：

```bash
# 方法 1: 使用 shell 脚本
./benchmark/run_benchmark.sh --threads 4 --requests 1000

# 方法 2: 使用 Python 脚本
python3 benchmark/benchmark.py --type all --concurrency 100 --requests 10000

# 方法 3: 使用 C++ 客户端
./benchmark_client --type http --port 8080 --threads 4 --requests 1000
./benchmark_client --type rpc --port 8081 --threads 4 --requests 1000
```

详细报告见 [benchmark/report.md](benchmark/report.md)

## 服务注册中心

提供服务注册、发现、心跳和健康检查功能：

### 服务端

```cpp
#include "registry/RegistryServer.h"

int main() {
    EventLoop loop;
    RegistryServer server(&loop, InetAddress(8500));
    server.start();
    loop.loop();
}
```

### 客户端 SDK

```cpp
#include "registry/RegistryClient.h"

// 创建客户端
RegistryClient client("127.0.0.1", 8500);

// 注册服务
ServiceKey key("production", "user-service", "v1.0.0");
InstanceMeta instance("inst-001", "192.168.1.100", 8080);
std::string instanceId;
client.registerService(key, instance, &instanceId);

// 发现服务
auto instances = client.discoverService(key);
for (const auto& inst : instances) {
    std::cout << inst->address() << std::endl;
}

// 自动心跳
client.startHeartbeat(key, instanceId);
```

### API 端点

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/v1/registry/register` | POST | 注册服务实例 |
| `/api/v1/registry/deregister` | POST | 注销服务实例 |
| `/api/v1/registry/heartbeat` | POST | 发送心跳 |
| `/api/v1/registry/discover` | GET | 发现服务 |
| `/api/v1/registry/services` | GET | 获取所有服务 |
| `/api/v1/registry/stats` | GET | 统计信息 |

## WebSocket

RFC 6455 标准的 WebSocket 服务器实现：

### 服务端

```cpp
#include "websocket/WebSocketServer.h"

int main() {
    EventLoop loop;
    WebSocketServer server(&loop, InetAddress(9500));

    // 设置连接处理器
    server.setConnectionHandler([](const WsSessionPtr& session) {
        std::cout << "New connection: " << session->clientAddress() << std::endl;
        session->sendText("Welcome!");
    });

    // 设置消息处理器
    server.setMessageHandler([](const WsSessionPtr& session, const WsMessage& msg) {
        if (msg.isText()) {
            // Echo
            session->sendText("Echo: " + msg.text());
        }
    });

    server.start();
    loop.loop();
}
```

### 特性

- 完整的 RFC 6455 握手协议
- 文本/二进制消息支持
- Ping/Pong 心跳
- 关闭帧处理
- 自定义握手验证
- 广播消息

## 技术栈

- C++17
- epoll (Linux)
- nlohmann/json
- **Protobuf** (高性能序列化)
- **OpenSSL** (WebSocket SHA1 计算)
- 无锁编程

## License

MIT