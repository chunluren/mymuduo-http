# 快速开始指南

## 5 分钟上手 mymuduo-http

---

## 1. HTTP 服务器

### 最简示例

```cpp
#include "HttpServer.h"

int main() {
    EventLoop loop;
    HttpServer server(&loop, InetAddress(8080));
    
    server.GET("/", [](const HttpRequest& req, HttpResponse& resp) {
        resp.setHtml("<h1>Hello World</h1>");
    });
    
    server.GET("/api/time", [](const HttpRequest& req, HttpResponse& resp) {
        time_t now = time(nullptr);
        resp.json("{\"time\": \"" + std::string(ctime(&now)) + "\"}");
    });
    
    server.start();
    loop.loop();
}
```

### 运行

```bash
g++ -O2 -std=c++17 -I. -L./lib server.cpp -lmymuduo -lpthread -o server
./server

# 测试
curl http://localhost:8080/
curl http://localhost:8080/api/time
```

---

## 2. JSON-RPC

### 服务端

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
    json result = client.call("add", {{"a", 10}, {"b", 20}});
    std::cout << result << std::endl;  // {"result": 30}
}
```

---

## 3. Protobuf-RPC

### 定义协议

```protobuf
// calc.proto
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

### 服务端

```cpp
#include "RpcServerPb.h"
#include "calc.pb.h"

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

### 客户端

```cpp
#include "RpcClientPb.h"
#include "calc.pb.h"

int main() {
    RpcClientPb client("127.0.0.1", 8082);
    
    CalcRequest req;
    req.set_a(10);
    req.set_b(5);
    
    CalcResponse resp;
    client.call<CalcRequest, CalcResponse>("calc", "add", req, resp);
    
    std::cout << "Result: " << resp.result() << std::endl;  // 15
}
```

---

## 4. 定时器

```cpp
#include "timer/TimerQueue.h"

TimerQueue timers;

// 5 秒后执行
timers.addTimer([]() {
    std::cout << "Timer fired!" << std::endl;
}, 5000);

// 每 1 秒执行
timers.addTimer([]() {
    std::cout << "Heartbeat" << std::endl;
}, 1000, 1000);

// 主循环中调用
while (true) {
    timers.tick();
    usleep(100000);  // 100ms
}
```

---

## 5. 异步日志

```cpp
#include "asynclogger/AsyncLogger.h"

// 启动
AsyncLogger::instance().setLogFile("/var/log/app.log");
AsyncLogger::instance().start();

// 使用
LOG_INFO("Server started");
LOG_ERROR("Connection failed: %s", strerror(errno));

// 停止
AsyncLogger::instance().stop();
```

---

## 6. 连接池

```cpp
#include "pool/ConnectionPool.h"

// 创建连接池
ConnectionPool pool("127.0.0.1", 3306, 5, 20);

// 获取连接
auto conn = pool.acquire();
if (conn) {
    conn->send("PING", 4);
    char buf[1024];
    conn->recv(buf, sizeof(buf));
    pool.release(conn);
}

// 健康检查
pool.healthCheck();
```

---

## 7. 配置管理

### 配置文件 (server.conf)

```ini
[server]
port = 8080
threads = 4

[log]
level = INFO
file = /var/log/app.log
```

### 使用

```cpp
#include "config/Config.h"

Config::instance().load("server.conf");

int port = CONFIG_INT("server.port");
std::string logFile = CONFIG_STRING("log.file");
```

---

## 常见问题

### Q: 编译报错找不到 protobuf?

```bash
# Ubuntu
sudo apt install libprotobuf-dev protobuf-compiler

# macOS
brew install protobuf
```

### Q: 如何设置线程数?

```cpp
server.setThreadNum(4);  // 4 个 IO 线程
```

### Q: 如何实现优雅退出?

```cpp
#include "util/SignalHandler.h"

Signals::gracefulExit([]() {
    server.stop();
    AsyncLogger::instance().stop();
});
```

---

更多细节请参考 [架构文档](docs/ARCHITECTURE.md)