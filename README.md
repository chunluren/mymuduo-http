# mymuduo-http

基于 mymuduo 网络库的 HTTP 服务器和 RPC 框架

## 项目结构

```
mymuduo-http/
├── src/
│   ├── http/
│   │   ├── HttpRequest.h    # HTTP 请求解析
│   │   ├── HttpResponse.h   # HTTP 响应
│   │   └── HttpServer.h     # HTTP 服务器
│   └── rpc/
│       ├── RpcServer.h      # RPC 服务端
│       └── RpcClient.h      # RPC 客户端
├── examples/
│   ├── http_server.cpp      # HTTP 服务器示例
│   ├── rpc_server.cpp       # RPC 服务器示例
│   └── rpc_client.cpp       # RPC 客户端示例
└── CMakeLists.txt
```

## 功能特性

### HTTP 服务器
- HTTP/1.1 协议解析
- Keep-Alive 连接复用
- 路由注册（GET/POST/PUT/DELETE）
- 正则路由匹配
- 静态文件服务
- 中间件支持

### RPC 框架
- JSON-RPC 2.0 协议
- 服务方法注册
- 同步/异步调用
- 错误处理

## 快速开始

```bash
# 编译
mkdir build && cd build
cmake ..
make

# 启动 HTTP 服务器
./http_server

# 启动 RPC 服务器
./rpc_server

# 测试 RPC 客户端
./rpc_client
```

## HTTP 使用示例

```cpp
#include "HttpServer.h"

int main() {
    EventLoop loop;
    InetAddress addr(8080);
    HttpServer server(&loop, addr);
    
    // 注册路由
    server.GET("/", [](const HttpRequest& req, HttpResponse& resp) {
        resp.setHtml("<h1>Hello World</h1>");
    });
    
    server.GET("/api/time", [](const HttpRequest& req, HttpResponse& resp) {
        resp.json("{\"time\": \"now\"}");
    });
    
    server.POST("/api/data", [](const HttpRequest& req, HttpResponse& resp) {
        resp.json(req.body);  // Echo
    });
    
    server.start();
    loop.loop();
}
```

## RPC 使用示例

### 服务端

```cpp
#include "RpcServer.h"

int main() {
    EventLoop loop;
    InetAddress addr(8081);
    RpcServer server(&loop, addr);
    
    // 注册方法
    server.registerMethod("hello", [](const json& params) {
        return {{"message", "Hello, " + params.value("name", "World")}};
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
    
    json result = client.call("hello", {{"name", "mymuduo"}});
    std::cout << result << std::endl;
}
```

## 技术亮点

1. **高性能网络层** - 基于 mymuduo 的 Reactor 模式
2. **零拷贝解析** - Buffer 数据直接传递
3. **连接复用** - HTTP Keep-Alive 支持
4. **路由灵活** - 正则匹配 + 参数提取
5. **JSON-RPC** - 标准协议，跨语言兼容

## License

MIT