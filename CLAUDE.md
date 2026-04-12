# mymuduo-http

C++17 Reactor 模式高性能网络框架，基于 muduo 设计，支持 HTTP/RPC/WebSocket/服务注册发现，含完整的服务端和客户端组件。

## 目录结构

```
src/
├── net/              核心网络层（Reactor 架构）
│   ├── EventLoop.h/cc       事件循环（timerfd 集成，支持 runAfter/runEvery）
│   ├── Channel.h/cc         fd + 事件回调封装
│   ├── Poller.h/cc          I/O 多路复用抽象
│   ├── EPollPoller.h/cc     epoll LT 模式实现
│   ├── TcpServer.h/cc       TCP 服务端（mainReactor + subReactors）
│   ├── TcpClient.h/cc       TCP 客户端（Connector + TcpConnection）
│   ├── Connector.h/cc       非阻塞 connect + 指数退避重连
│   ├── TcpConnection.h/cc   TCP 连接（服务端客户端共用）
│   ├── Acceptor.h/cc        被动接受连接
│   ├── Buffer.h/cc          自动扩容 I/O 缓冲区（readv）
│   ├── Socket.h/cc          Socket 封装
│   ├── InetAddress.h/cc     地址封装
│   ├── EventLoopThread.h/cc     线程 + EventLoop
│   ├── EventLoopThreadPool.h/cc 线程池（轮询分发）
│   ├── TimerId.h            定时器标识符
│   ├── Callbacks.h          回调类型定义
│   ├── Timestamp.h/cc       时间戳
│   ├── Thread.h/cc          线程封装
│   ├── CurrentThread.h/cc   线程局部存储
│   ├── noncopyable.h        不可拷贝基类
│   └── logger.h/cc          日志（LOG_INFO/ERROR/FATAL/DEBUG）
├── http/             HTTP 模块
│   ├── HttpServer.h         HTTP 服务端（路由、中间件、CORS、静态文件）
│   ├── HttpClient.h         HTTP 客户端（GET/POST/PUT/DELETE，同步/异步）
│   ├── HttpRequest.h        请求解析（方法、路径、头部、Cookie、查询参数）
│   ├── HttpResponse.h       响应构造（状态码、CORS、Cookie、redirect）
│   ├── GzipMiddleware.h     Gzip 压缩中间件（请求解压 + 响应压缩）
│   ├── MultipartParser.h    Multipart/form-data 文件上传解析
│   ├── SslContext.h         OpenSSL Memory BIO 封装
│   └── HttpsServer.h        HTTPS/TLS 服务端
├── rpc/              RPC 模块
│   ├── RpcServer.h          JSON-RPC 2.0 服务端
│   ├── RpcClient.h          JSON-RPC 客户端（阻塞版 + ReactorRpcClient）
│   ├── RpcServerPb.h        Protobuf-RPC 服务端
│   ├── RpcClientPb.h        Protobuf-RPC 客户端
│   └── proto/rpc.proto      Protobuf 定义
├── websocket/        WebSocket 模块
│   ├── WebSocketServer.h    WebSocket 服务端
│   ├── WebSocketClient.h    WebSocket 客户端（掩码、握手、自动重连）
│   ├── WebSocketFrame.h     帧编解码 + Accept Key 计算
│   └── WsSession.h          会话管理（状态机、消息类型）
├── registry/         服务注册发现
│   ├── RegistryServer.h     注册中心 REST API
│   ├── RegistryClient.h     注册客户端 SDK
│   ├── ServiceCatalog.h     服务元数据存储
│   ├── HealthChecker.h      健康检查
│   └── ServiceMeta.h        服务元数据结构
├── balancer/         负载均衡（5 种策略）
│   └── LoadBalancer.h       轮询/加权/最少连接/随机/一致性哈希
├── timer/            定时器
│   ├── TimerQueue.h         时间轮 O(1)（已集成到 EventLoop）
│   └── Timer.h              定时器对象
├── pool/             连接池
│   ├── ConnectionPool.h
│   ├── MySQLPool.h          MySQL 连接池
│   └── RedisPool.h          Redis 连接池
├── asynclogger/      异步日志（双缓冲）
│   └── AsyncLogger.h
├── config/           配置管理
│   └── Config.h             INI 解析
└── util/             工具
    ├── SignalHandler.h      信号处理
    ├── RateLimiter.h        限流器（令牌桶 + 滑动窗口）
    ├── ObjectPool.h         通用对象池
    ├── CircuitBreaker.h     熔断器
    └── Metrics.h            Prometheus 监控指标
```

## 架构

```
服务端路径: TcpServer → Acceptor → accept() → TcpConnection
客户端路径: TcpClient → Connector → connect() → TcpConnection
                                                    ↑
                              连接建立后完全相同，共用 TcpConnection

线程模型:
  mainReactor (baseLoop): 运行 Acceptor，只负责 accept
  subReactor (IO Thread): 运行 TcpConnection，负责读写
  轮询分发: EventLoopThreadPool::getNextLoop()

定时器: EventLoop 内置 timerfd + TimerQueue（时间轮）
  loop->runAfter(秒, 回调)
  loop->runEvery(秒, 回调)
  loop->cancel(timerId)
```

## 构建

```bash
mkdir build && cd build
cmake .. && make -j$(nproc)

# 带 Sanitizer
cmake -DENABLE_ASAN=ON .. && make -j$(nproc)   # AddressSanitizer
cmake -DENABLE_TSAN=ON .. && make -j$(nproc)   # ThreadSanitizer
```

依赖: Protobuf, OpenSSL, nlohmann/json（CMake 自动拉取）, zlib, libmysqlclient (optional), libhiredis (optional), CMake >= 3.10, GCC >= 7

## 测试

```bash
./run_tests.sh              # 构建 + 运行所有测试
cd build && ./test_buffer   # 单个测试
```

测试文件: test_buffer, test_eventloop, test_http, test_config, test_timer, test_tcp_server_client, test_http_client, test_websocket_client, test_load_balancer, test_registry, test_websocket_frame, test_websocket_server, test_mysql_pool, test_redis_pool, test_rate_limiter, test_gzip, test_chunked, test_object_pool, test_circuit_breaker, test_multipart, test_metrics, test_https, test_graceful_shutdown

## 已知问题

- `test_tcp_server_client` 的多线程测试在部分环境下超时

## 编码规范

- 新模块 **header-only**（仅 src/net/ 核心有 .cc 文件）
- 测试宏: `TEST(name)` / `RUN_TEST(name)` + `assert()`
- 日志: `LOG_INFO(fmt, ...)`、`LOG_ERROR`、`LOG_FATAL`、`LOG_DEBUG`
- CMake 自动发现: `file(GLOB TEST_SOURCES tests/*.cpp)` — 新测试放 tests/ 即可
- InetAddress 构造: `InetAddress(port, "ip")` — 端口在前，IP 在后

## 完成记录

所有 4 个阶段 + 额外改进已全部完成:
- 阶段 1: WebSocketServer 修复、MySQLPool、RedisPool、心跳/超时、限流器
- 阶段 2: Gzip 压缩、Chunked Transfer、ObjectPool、CircuitBreaker
- 阶段 3: muduo-im 服务端（JWT、UserService、ChatServer、消息路由、文件上传、消息撤回、已读回执）
- 阶段 4: muduo-im 前端（index.html 单文件 + 静态文件服务）+ 端到端测试 + WebSocket 压力测试
- P0-P3 审计修复: 好友申请制、群组退出/解散、用户资料/密码/注销、正在输入提示、消息长度校验、文件上传限制（50MB + 类型白名单）、消息撤回软删除、Redis 降级回退、未读计数、搜索用户
- 额外改进: HTTPS/TLS、优雅关闭、Multipart 解析、Prometheus 监控、路由线程安全、路由 O(1) 匹配、请求解压、热路径日志优化、toString 性能优化
