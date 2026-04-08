# mymuduo-http 技术详解

本文档深入讲解各模块的实现细节，所有代码片段均来自实际源码。

---

## 目录

1. [epoll 事件处理完整流程](#1-epoll-事件处理完整流程)
2. [HTTP 解析状态机](#2-http-解析状态机)
3. [RPC 协议帧格式](#3-rpc-协议帧格式)
4. [WebSocket 协议实现](#4-websocket-协议实现)
5. [定时器模块](#5-定时器模块)
6. [异步日志模块](#6-异步日志模块)
7. [连接池模块](#7-连接池模块)
8. [面试高频问题](#8-面试高频问题)

---

## 1. epoll 事件处理完整流程

### 1.1 EPollPoller 核心实现

#### Channel 在 Poller 中的三种状态

```cpp
// EPollPoller.cc
const int kNew = -1;      // Channel 从未注册到 Poller
const int kAdded = 1;     // Channel 已注册到 epoll
const int kDeleted = 2;   // Channel 已从 epoll 删除（但仍在 channels_ map 中）
```

#### poll() — 等待事件并填充活跃 Channel

```cpp
// EPollPoller.cc:53-86
Timestamp EPollPoller::poll(int timeoutMs, ChannelList* activeChannels)
{
    Timestamp now(Timestamp::now());
    int saveErrno = errno;

    // 核心系统调用：阻塞等待 I/O 事件
    int numEvents = ::epoll_wait(epollfd_,
                                 &*events_.begin(),
                                 static_cast<int>(events_.size()),
                                 timeoutMs);
    if (numEvents > 0)
    {
        // 将就绪事件对应的 Channel 填入 activeChannels
        fillActiveChannels(numEvents, activeChannels);

        // 动态扩容：如果事件数等于容量，说明可能有更多事件
        if (static_cast<size_t>(numEvents) == events_.size())
        {
            events_.resize(events_.size() * 2);
        }
    }
    else if (numEvents == 0)
    {
        // 超时，无事件
    }
    else
    {
        // EINTR 是信号中断，忽略；其他错误记录日志
        if (errno != EINTR)
        {
            errno = saveErrno;
            LOG_ERROR("epoll_wait error:%d\n", errno);
        }
    }
    return now;
}
```

**events_ 动态扩容策略**：
- 初始大小 `kInitEventListSize = 16`
- 每当返回事件数 == 当前容量时，扩容为 2 倍
- 避免预分配过大数组浪费内存

#### updateChannel() — 注册/修改/删除 Channel

```cpp
// EPollPoller.cc:96-153
void EPollPoller::updateChannel(Channel* channel)
{
    const int index = channel->index();

    if (index == kNew || index == kDeleted)
    {
        if (index == kNew)
        {
            int fd = channel->fd();
            channels_[fd] = channel;    // 加入 fd→Channel 映射表
        }
        channel->set_index(kAdded);
        update(EPOLL_CTL_ADD, channel); // 注册到 epoll
    }
    else  // kAdded
    {
        if (channel->isNoneEvent())
        {
            update(EPOLL_CTL_DEL, channel); // 无关注事件，从 epoll 删除
            channel->set_index(kDeleted);
        }
        else
        {
            update(EPOLL_CTL_MOD, channel); // 修改关注的事件
        }
    }
}
```

#### update() — 封装 epoll_ctl 系统调用

```cpp
// EPollPoller.cc:215-248
void EPollPoller::update(int operation, Channel* channel)
{
    epoll_event event;
    int fd = channel->fd();
    bzero(&event, sizeof(event));
    event.events = channel->events();
    event.data.fd = fd;
    event.data.ptr = channel;   // 关键：将 Channel 指针存入 epoll_event

    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)
    {
        if (operation == EPOLL_CTL_DEL)
        {
            LOG_ERROR("epoll_ctl del error:%d\n", errno);
        }
        else
        {
            LOG_FATAL("epoll_ctl add/mod error:%d\n", errno);  // 致命错误
        }
    }
}
```

**`event.data.ptr = channel` 的作用**：epoll_wait 返回时，可以直接从 `epoll_event.data.ptr` 拿到 Channel 指针，无需再通过 fd 查找。

### 1.2 完整的事件处理链路

```
                        一次 EventLoop 迭代
                        ──────────────────
epoll_wait(timeout)
    │
    ├── 返回 numEvents 个就绪事件
    │
    ▼
fillActiveChannels()
    │  从 events_[i].data.ptr 取出 Channel*
    │  设置 Channel 的 revents_ = events_[i].events
    │  加入 activeChannels 列表
    │
    ▼
遍历 activeChannels
    │
    ├── Channel::handleEvent(receiveTime)
    │       │
    │       ├── EPOLLIN/EPOLLPRI → readCallback_(receiveTime)
    │       │       │
    │       │       ├── Acceptor: accept() → newConnection()
    │       │       ├── TcpConnection: readFd() → messageCallback_()
    │       │       └── wakeupChannel: 读走 eventfd 数据（消费唤醒事件）
    │       │
    │       ├── EPOLLOUT → writeCallback_()
    │       │       └── TcpConnection: writeFd() → 发送输出缓冲区数据
    │       │
    │       ├── EPOLLHUP (且没有 EPOLLIN) → closeCallback_()
    │       │       └── TcpConnection::handleClose()
    │       │
    │       └── EPOLLERR → errorCallback_()
    │               └── TcpConnection::handleError()
    │
    ▼
doPendingFunctors()
    │  swap 取出 pendingFunctors_ 队列
    │  逐一执行跨线程投递的任务
    │
    ▼
回到 epoll_wait 等待下一轮事件
```

### 1.3 LT 模式 vs ET 模式

本项目使用 **LT（水平触发）模式**：

| 特性 | LT 模式（本项目） | ET 模式 |
|------|------------------|---------|
| 触发条件 | 只要 fd 有数据可读/可写就触发 | 仅在状态变化时触发一次 |
| 编程难度 | 简单，不会丢事件 | 复杂，必须一次读完/写完 |
| 是否需要循环读 | 不需要 | 必须循环读到 EAGAIN |
| 性能 | 略低（可能多次触发） | 略高（减少触发次数） |

**选择 LT 的原因**：
- 更安全，不会因为没读完数据而丢失事件
- 配合 `readv` + 栈缓冲区，单次读取效率已经足够
- muduo 原版也使用 LT 模式

---

## 2. HTTP 解析状态机

### 2.1 Peek-Parse-Consume 三阶段模式

HTTP 解析的核心挑战是 TCP 字节流的粘包/半包问题。本项目采用"先看后消费"的模式：

```
阶段一：Peek（只看不消费）
    │  buf->peek() 获取 Buffer 中的原始字节指针
    │  不移动 readerIndex_，数据保持不变
    │
    ▼
阶段二：Parse（解析判断完整性）
    │  用 memmem() 查找 "\r\n\r\n" 定位请求头结束位置
    │  解析 Content-Length 计算请求体长度
    │  判断 Buffer 中数据是否足够
    │
    ├── 不完整 → return Incomplete（等待更多数据）
    ├── 格式错误 → return Error（发送 400）
    │
    ▼
阶段三：Consume（确认完整后消费）
    │  buf->retrieve(headerLen)     消费请求头
    │  buf->retrieve(contentLen)    消费请求体
    │  return Complete
```

### 2.2 实际解析代码

```cpp
// HttpServer.h:334-376
ParseResult parseRequest(Buffer* buf, HttpRequest& request) {
    const char* data = buf->peek();       // 阶段一：只看
    size_t len = buf->readableBytes();

    // 查找请求头结束标记
    const char* headerEnd = static_cast<const char*>(
        memmem(data, len, "\r\n\r\n", 4));

    if (!headerEnd) {
        if (len > 8192) {                 // 请求头超过 8KB，拒绝
            return ParseResult::Error;
        }
        return ParseResult::Incomplete;   // 阶段二：不完整
    }

    size_t headerLen = headerEnd - data + 4;

    // 解析请求头各字段
    if (!parseHeader(header, request)) {
        return ParseResult::Error;
    }

    // 检查请求体完整性
    size_t contentLen = request.contentLength();
    if (len < headerLen + contentLen) {
        return ParseResult::Incomplete;
    }

    // 阶段三：数据完整，消费
    buf->retrieve(headerLen);
    if (contentLen > 0) {
        request.body.assign(buf->peek(), contentLen);
        buf->retrieve(contentLen);
    }
    return ParseResult::Complete;
}
```

### 2.3 HTTP Pipeline 处理

HTTP Pipeline 允许客户端在同一连接上连续发送多个请求，无需等待前一个响应：

```cpp
// HttpServer.h:265-316
void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
    // while 循环处理同一 Buffer 中的多个请求
    while (buf->readableBytes() > 0) {
        HttpRequest request;
        ParseResult result = parseRequest(buf, request);

        if (result == ParseResult::Incomplete) {
            return;  // 数据不完整，等下次 epoll 触发
        }
        if (result == ParseResult::Error) {
            HttpResponse resp = HttpResponse::badRequest("Bad Request");
            resp.closeConnection = true;
            conn->send(resp.toString());
            conn->shutdown();
            return;
        }

        // 请求体大小校验
        if (request.contentLength() > kMaxBodySize) {
            HttpResponse resp = HttpResponse::badRequest("Request body too large");
            resp.closeConnection = true;
            conn->send(resp.toString());
            conn->shutdown();
            return;
        }

        // 处理请求
        HttpResponse response;
        handleRequest(request, response);

        // Keep-Alive 判断
        response.closeConnection = !request.keepAlive();
        conn->send(response.toString());

        if (response.closeConnection) {
            conn->shutdown();
            return;
        }
        // 继续循环处理下一个 Pipeline 请求
    }
}
```

### 2.4 Keep-Alive 判断逻辑

```cpp
// HttpRequest.h
bool keepAlive() const {
    if (version == HttpVersion::HTTP_10) {
        // HTTP/1.0 默认不保持连接，需显式声明
        return getHeader("connection") == "keep-alive";
    }
    // HTTP/1.1 默认保持连接，除非显式 close
    return getHeader("connection") != "close";
}
```

### 2.5 路由匹配流程

```cpp
// HttpServer.h - handleRequest 内部流程
void handleRequest(const HttpRequest& request, HttpResponse& response) {
    // 1. 执行中间件链
    for (auto& middleware : middlewares_) {
        middleware(request, response);
    }

    // 2. 遍历路由表匹配
    for (const auto& route : routes_) {
        if (route.method == request.method &&
            std::regex_match(request.path, route.regex)) {
            route.handler(request, response);
            return;
        }
    }

    // 3. 尝试静态文件服务
    for (const auto& [prefix, dir] : staticDirs_) {
        if (request.path.find(prefix) == 0) {
            serveFile(request, response, dir, request.path.substr(prefix.size()));
            return;
        }
    }

    // 4. 未匹配，返回 404
    response = HttpResponse::notFound();
}
```

**路由的正则预编译**：
```cpp
struct Route {
    HttpMethod method;
    std::string pattern;
    HttpHandler handler;
    std::regex regex;   // 构造时编译，避免每次匹配都编译正则

    Route(HttpMethod m, const std::string& p, HttpHandler h)
        : method(m), pattern(p), handler(h), regex(p) {}
};
```

---

## 3. RPC 协议帧格式

### 3.1 Protobuf-RPC 二进制帧格式

#### 传输层帧结构

```
┌──────────────────┬──────────────────────────────────────┐
│  Length (4 bytes) │  Protobuf Payload (N bytes)          │
│  网络字节序       │  RpcRequest 或 RpcResponse           │
│  (big-endian)    │  的序列化数据                         │
└──────────────────┴──────────────────────────────────────┘
```

#### RpcRequest Protobuf 定义

```protobuf
message RpcRequest {
    string service = 1;    // 服务名，如 "Calculator"
    string method = 2;     // 方法名，如 "Add"
    bytes params = 3;      // 参数：具体请求消息的序列化字节
    int64 id = 4;          // 请求 ID，用于匹配响应
}
```

#### RpcResponse Protobuf 定义

```protobuf
message RpcResponse {
    int64 id = 1;          // 对应的请求 ID
    bytes result = 2;      // 结果：具体响应消息的序列化字节
    int32 code = 3;        // 错误码，0 = 成功
    string message = 4;    // 错误信息
}
```

#### 完整的请求/响应字节流

```
请求字节流示例：
┌─────────┬────────────────────────────────────────────────────┐
│ 00 00   │  0A 0A 43 61 6C 63 75 6C 61 74 6F 72              │
│ 00 2C   │  12 03 41 64 64                                    │
│ (44字节)│  1A 04 [AddRequest 序列化字节]                      │
│         │  20 01                                              │
└─────────┴────────────────────────────────────────────────────┘
  长度前缀     RpcRequest { service="Calculator", method="Add",
               params=<AddRequest bytes>, id=1 }

响应字节流示例：
┌─────────┬────────────────────────────────────────────────────┐
│ 00 00   │  08 01                                              │
│ 00 0C   │  12 06 [AddResponse 序列化字节]                     │
│ (12字节)│  18 00                                              │
└─────────┴────────────────────────────────────────────────────┘
  长度前缀     RpcResponse { id=1, result=<AddResponse bytes>,
               code=0 }
```

### 3.2 服务端帧解析实现

```cpp
// RpcServerPb.h:62-90
void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
    while (buf->readableBytes() >= 4) {
        // 1. 读取长度前缀（不消费）
        int32_t len = 0;
        memcpy(&len, buf->peek(), 4);
        len = ntohl(len);                    // 网络字节序 → 主机字节序

        // 2. 安全校验
        if (len <= 0 || static_cast<size_t>(len) > kMaxFrameSize) {  // kMaxFrameSize = 10MB
            sendError(conn, 0, -32600, "Invalid frame size");
            conn->shutdown();
            return;
        }

        // 3. 检查数据完整性
        if (buf->readableBytes() < 4 + static_cast<size_t>(len)) {
            return;  // 等待更多数据
        }

        // 4. 消费数据
        buf->retrieve(4);                    // 消费长度前缀
        std::string data(buf->peek(), len);  // 取出 Protobuf 数据
        buf->retrieve(len);                  // 消费数据体

        // 5. 处理请求
        processMessage(conn, data);
    }
}
```

### 3.3 客户端帧构造实现

```cpp
// RpcClientPb.h:72-132
template<typename T1, typename T2>
bool call(const std::string& service, const std::string& method,
          const T1& request, T2& response) {
    // 1. 构造 RpcRequest
    rpc::RpcRequest req;
    req.set_service(service);
    req.set_method(method);
    req.set_id(nextId_++);
    request.SerializeToString(req.mutable_params());  // 序列化参数

    // 2. 序列化整个请求
    std::string data;
    req.SerializeToString(&data);

    // 3. 发送：长度前缀 + 数据
    int32_t len = htonl(data.size());      // 主机字节序 → 网络字节序
    sendAll(&len, 4);                       // 发送 4 字节长度
    sendAll(data.c_str(), data.size());     // 发送数据体

    // 4. 接收响应长度
    len = 0;
    recvAll(&len, 4);
    len = ntohl(len);

    // 5. 校验响应帧长度
    if (len <= 0 || len > RPC_MAX_FRAME_LENGTH) {  // RPC_MAX_FRAME_LENGTH = 64MB
        return false;
    }

    // 6. 接收响应数据
    std::string respData(len, '\0');
    recvAll(&respData[0], len);

    // 7. 解析响应
    rpc::RpcResponse resp;
    resp.ParseFromString(respData);
    if (resp.code() != 0) return false;
    return response.ParseFromString(resp.result());
}
```

### 3.4 JSON-RPC 2.0 协议格式

```
传输层帧结构（与 Protobuf-RPC 相同）：
┌──────────────────┬──────────────────────────────────────┐
│  Length (4 bytes) │  JSON 字符串 (N bytes)               │
└──────────────────┴──────────────────────────────────────┘

请求 JSON：
{
    "jsonrpc": "2.0",
    "method": "add",
    "params": [1, 2],
    "id": 1
}

成功响应：
{
    "jsonrpc": "2.0",
    "result": 3,
    "id": 1
}

错误响应：
{
    "jsonrpc": "2.0",
    "error": {
        "code": -32601,
        "message": "Method not found"
    },
    "id": 1
}
```

### 3.5 性能对比

| 指标 | JSON-RPC | Protobuf-RPC | 差异 |
|------|----------|--------------|------|
| 序列化速度 | 基准 | 5-10 倍 | Protobuf 二进制编码 |
| 数据大小 | 基准 | 0.2-0.3 倍 | 无字段名、紧凑编码 |
| 类型安全 | 运行时检查 | 编译时检查 | .proto 生成代码 |
| 可读性 | 人类可读 | 二进制不可读 | JSON 调试方便 |
| 跨语言 | 广泛支持 | 需要 protoc | 生态差异 |

---

## 4. WebSocket 协议实现

### 4.1 握手流程（HTTP Upgrade）

WebSocket 连接从一个 HTTP 请求开始：

```
客户端 ──────────────────────────────────────▶ 服务端

GET /chat HTTP/1.1
Host: server.example.com
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13

服务端 ◀──────────────────────────────────── 服务端

HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

#### Accept Key 计算（RFC 6455 规定）

```cpp
// WebSocketFrame.h - computeAcceptKey()
static std::string computeAcceptKey(const std::string& clientKey) {
    // 固定 GUID（RFC 6455 Section 4.2.2）
    const std::string GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = clientKey + GUID;

    // SHA-1 哈希
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()),
         combined.size(), hash);

    // Base64 编码
    return base64Encode(hash, SHA_DIGEST_LENGTH);
}
```

**RFC 6455 示例验证**：
- 客户端 Key：`dGhlIHNhbXBsZSBub25jZQ==`
- 拼接 GUID 后 SHA-1 → Base64
- 预期结果：`s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`（项目测试用例已验证）

### 4.2 WebSocket 帧格式（RFC 6455）

```
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-------+-+-------------+-------------------------------+
 |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
 |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
 |N|V|V|V|       |S|             |   (if payload len==126/127)   |
 | |1|2|3|       |K|             |                               |
 +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
 |     Extended payload length continued, if payload len == 127  |
 + - - - - - - - - - - - - - - - +-------------------------------+
 |                               |Masking-key, if MASK set to 1  |
 +-------------------------------+-------------------------------+
 | Masking-key (continued)       |          Payload Data         |
 +-------------------------------- - - - - - - - - - - - - - - - +
```

**各字段说明**：

| 字段 | 位数 | 说明 |
|------|------|------|
| FIN | 1 bit | 1 = 最终分片，0 = 后续还有分片 |
| RSV1-3 | 各 1 bit | 保留位，无扩展时必须为 0 |
| opcode | 4 bits | 操作码（见下表） |
| MASK | 1 bit | 1 = 有掩码（客户端→服务端必须为 1） |
| Payload len | 7 bits | 0-125: 直接表示长度；126: 后续 2 字节表示；127: 后续 8 字节表示 |
| Masking-key | 32 bits | 仅 MASK=1 时存在，用于 XOR 解码负载 |
| Payload Data | 变长 | 实际数据（若有掩码则需 XOR 解码） |

**操作码**：

| 值 | 名称 | 说明 |
|----|------|------|
| 0x0 | Continuation | 延续帧 |
| 0x1 | Text | UTF-8 文本 |
| 0x2 | Binary | 二进制数据 |
| 0x8 | Close | 关闭连接 |
| 0x9 | Ping | 心跳请求 |
| 0xA | Pong | 心跳响应 |

### 4.3 帧编码实现

```cpp
// WebSocketFrame.h:137-195
static std::vector<uint8_t> encode(const WebSocketFrame& frame, bool mask = false) {
    std::vector<uint8_t> data;

    // 字节 1：FIN + RSV + Opcode
    uint8_t byte1 = (frame.fin ? 0x80 : 0x00) |
                    (static_cast<uint8_t>(frame.opcode) & 0x0F);
    data.push_back(byte1);

    // 字节 2：MASK + Payload Length
    size_t payloadLen = frame.payload.size();
    uint8_t byte2 = (mask ? 0x80 : 0x00);

    if (payloadLen <= 125) {
        byte2 |= static_cast<uint8_t>(payloadLen);
        data.push_back(byte2);
    } else if (payloadLen <= 65535) {
        byte2 |= 126;                          // 标记为 16-bit 扩展长度
        data.push_back(byte2);
        data.push_back((payloadLen >> 8) & 0xFF);  // big-endian
        data.push_back(payloadLen & 0xFF);
    } else {
        byte2 |= 127;                          // 标记为 64-bit 扩展长度
        data.push_back(byte2);
        for (int i = 7; i >= 0; --i) {
            data.push_back((payloadLen >> (i * 8)) & 0xFF);
        }
    }

    // 掩码密钥（客户端→服务端时使用）
    uint8_t maskingKey[4] = {0};
    if (mask) {
        std::random_device rd;
        std::mt19937 gen(rd());
        for (int i = 0; i < 4; ++i) {
            maskingKey[i] = static_cast<uint8_t>(gen() % 256);
            data.push_back(maskingKey[i]);
        }
    }

    // 负载数据（有掩码时 XOR 编码）
    for (size_t i = 0; i < payloadLen; ++i) {
        if (mask) {
            data.push_back(frame.payload[i] ^ maskingKey[i % 4]);
        } else {
            data.push_back(frame.payload[i]);
        }
    }

    return data;
}
```

### 4.4 帧解码实现

```cpp
// WebSocketFrame.h:250-326
static DecodeResult decode(const uint8_t* data, size_t len) {
    DecodeResult result;

    if (len < 2) {
        result.status = DecodeResult::Incomplete;
        return result;
    }

    // 解析字节 1
    result.frame.fin = (data[0] & 0x80) != 0;
    result.frame.opcode = static_cast<WsOpcode>(data[0] & 0x0F);

    // 解析字节 2
    result.frame.mask = (data[1] & 0x80) != 0;
    uint64_t payloadLen = data[1] & 0x7F;
    size_t headerSize = 2;

    // 扩展长度
    if (payloadLen == 126) {
        if (len < 4) return {DecodeResult::Incomplete};
        payloadLen = (static_cast<uint64_t>(data[2]) << 8) | data[3];
        headerSize = 4;
    } else if (payloadLen == 127) {
        if (len < 10) return {DecodeResult::Incomplete};
        payloadLen = 0;
        for (int i = 2; i < 10; ++i) {
            payloadLen = (payloadLen << 8) | data[i];
        }
        headerSize = 10;
    }

    // 掩码密钥
    if (result.frame.mask) {
        if (len < headerSize + 4) return {DecodeResult::Incomplete};
        memcpy(result.frame.maskingKey, data + headerSize, 4);
        headerSize += 4;
    }

    // 检查负载完整性
    if (len < headerSize + payloadLen) {
        result.status = DecodeResult::Incomplete;
        return result;
    }

    // 解码负载（有掩码则 XOR）
    result.frame.payload.resize(payloadLen);
    const uint8_t* payloadData = data + headerSize;
    if (result.frame.mask) {
        for (uint64_t i = 0; i < payloadLen; ++i) {
            result.frame.payload[i] = payloadData[i] ^ result.frame.maskingKey[i % 4];
        }
    } else {
        memcpy(result.frame.payload.data(), payloadData, payloadLen);
    }

    result.status = DecodeResult::Ok;
    result.consumed = headerSize + payloadLen;
    return result;
}
```

### 4.5 WebSocket 消息处理流程

```
新 TCP 连接到达
    │
    ▼
onConnection() → 创建 WsSession（状态：Connecting）
    │
    ▼
onMessage()
    │
    ├── 状态 == Connecting
    │       │
    │       ▼
    │   handleHandshake()
    │   ├── 解析 HTTP 请求头
    │   ├── 验证 Upgrade: websocket
    │   ├── 验证 Sec-WebSocket-Key
    │   ├── 计算 Accept Key
    │   ├── 发送 101 Switching Protocols
    │   └── 状态 → Open
    │
    ├── 状态 == Open
    │       │
    │       ▼
    │   handleWsFrames()
    │   ├── 循环解码帧 (decode)
    │   ├── 根据 opcode 分发：
    │   │   ├── Text/Binary → messageHandler_()
    │   │   ├── Ping → 自动回复 Pong
    │   │   ├── Pong → updateActivity()
    │   │   └── Close → 回复 Close + 关闭连接
    │   └── 状态 → Closed（如果收到 Close）
    │
    └── 状态 == Closing/Closed → 忽略
```

---

## 5. 定时器模块

### 5.1 时间轮 vs 其他定时器

| 数据结构 | 添加 | 删除 | 触发最近 | 适用场景 |
|----------|------|------|---------|----------|
| 最小堆 | O(log n) | O(log n) | O(1) | 定时器少 |
| 红黑树 | O(log n) | O(log n) | O(log n) | 需有序遍历 |
| **时间轮** | **O(1)** | **O(1)** | **均摊 O(1)** | **大量定时器** |

### 5.2 时间轮数据结构

```
默认配置：buckets=60, tickMs=1000
覆盖时间范围：60 * 1000ms = 60 秒

桶[0]  ──▶ [Timer(id=1)] → [Timer(id=5)] → ...
桶[1]  ──▶ [Timer(id=3)] → ...
桶[2]  ──▶ (空)
桶[3]  ──▶ [Timer(id=7)] → ...
  ...
桶[59] ──▶ [Timer(id=N)] → ...
  ▲
  │
currentBucket_ (当前指针，每 tick 前进一格)
```

**辅助结构**：
```cpp
std::vector<std::list<std::shared_ptr<Timer>>> wheel_;  // 时间轮（桶数组）
std::unordered_map<int64_t, std::shared_ptr<Timer>> timers_;  // ID → Timer（快速取消）
```

### 5.3 核心操作

**添加定时器 — O(1)**：
```cpp
int64_t addTimer(TimerCallback cb, int delayMs, int intervalMs = 0) {
    size_t ticks = delayMs / tickMs_;
    size_t bucket = (currentBucket_ + ticks) % buckets_;
    wheel_[bucket].push_back(timer);   // 链表尾插 O(1)
    timers_[timerId] = timer;          // 哈希表插入 O(1)
    return timerId;
}
```

**取消定时器 — O(1)**：
```cpp
void cancelTimer(int64_t timerId) {
    auto it = timers_.find(timerId);   // 哈希查找 O(1)
    if (it != timers_.end()) {
        it->second->cancel();          // 标记取消，不从链表删除
        timers_.erase(it);
    }
}
```

**推进时间轮 — 均摊 O(1)**：
```cpp
void tick() {
    auto& bucket = wheel_[currentBucket_];
    int64_t now = Timer::now();

    for (auto it = bucket.begin(); it != bucket.end(); ) {
        auto& timer = *it;
        if (timer->isCancelled()) {
            it = bucket.erase(it);     // 延迟删除已取消的定时器
        } else if (timer->expiration() <= now) {
            timer->run();              // 执行回调
            if (timer->repeat()) {
                timer->restart(now);
                size_t newBucket = ...;
                wheel_[newBucket].push_back(timer);  // 周期定时器重新入轮
            }
            it = bucket.erase(it);
        } else {
            ++it;
        }
    }
    currentBucket_ = (currentBucket_ + 1) % buckets_;
}
```

### 5.4 Timer ID 生成

```cpp
// Timer.h:164-165 — C++17 inline static
inline static std::atomic<int64_t> Timer::nextId_{0};

// 每次创建 Timer 时原子递增
Timer(...) : id_(nextId_.fetch_add(1, std::memory_order_relaxed)) {}
```

---

## 6. 异步日志模块

### 6.1 双缓冲技术

```
业务线程（多个）                         后台线程（1 个）
──────────────                         ──────────────
log() 写入 currentBuffer_              writerLoop() 等待
log() 写入 currentBuffer_
log() 写入 currentBuffer_
                                        ┌──────────────────────────────┐
缓冲区满或 100ms 到期 ── notify ──────▶ │ 被唤醒                        │
                                        │                              │
     ┌── mutex 加锁 ──┐                │                              │
     │ swap(current,   │                │                              │
     │      flush)     │ ◀───────────▶ │ swap(current, flush)         │
     └── mutex 解锁 ──┘                │                              │
                                        │ 遍历 flushBuffer_ 写入文件   │
继续写入新的 currentBuffer_             │ flushBuffer_->clear()        │
                                        └──────────────────────────────┘
```

### 6.2 关键实现

**前端写入**（`AsyncLogger.h:174-203`）：
```cpp
void log(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (level < level_.load(std::memory_order_relaxed)) return;

    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    LogEntry entry{level, getTimestamp(), "", file, line, buf};

    {
        std::lock_guard<std::mutex> lock(mutex_);
        currentBuffer_->push_back(entry);
        if (currentBuffer_->size() >= kFlushThreshold) {  // kFlushThreshold = 1000
            cv_.notify_one();
        }
    }
}
```

**后台刷盘**（`AsyncLogger.h:239-282`）：
```cpp
void writerLoop() {
    while (running_.load() || !currentBuffer_->empty()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !running_.load() || currentBuffer_->size() >= kFlushThreshold;
            });
            std::swap(currentBuffer_, flushBuffer_);  // O(1) 指针交换
        }
        // 无锁区域：写文件不阻塞前端线程
        for (const auto& entry : *flushBuffer_) {
            formatEntry(file, entry);
        }
        file.flush();
        flushBuffer_->clear();
    }
}
```

### 6.3 设计要点

- **`kFlushThreshold = 1000`**：累积 1000 条日志或 100ms 超时触发刷盘
- **`std::swap` 交换指针**：O(1) 操作，锁持有时间极短
- **`wait_for` 超时**：保证即使日志量少也能在 100ms 内刷盘
- **缓冲区预分配**：`bufferA_.reserve(kFlushThreshold)` 减少动态扩容

---

## 7. 连接池模块

### 7.1 连接池状态机

```
┌──────────────────────────────────────────────┐
│              ConnectionPool                   │
│                                               │
│  创建时：预创建 minSize (5) 个连接             │
│                                               │
│  acquire()                                    │
│  ├── 池中有空闲连接 → 取出复用                 │
│  ├── 池空但未达 maxSize (20) → 新建连接        │
│  └── 池空且已达 maxSize → 等待 timeoutMs       │
│                                               │
│  release()                                    │
│  └── 归还到池中 + notify_one() 唤醒等待者      │
│                                               │
│  healthCheck()                                │
│  └── 清理超过 60s 未使用的空闲连接             │
│      （保留不少于 minSize 个）                  │
└──────────────────────────────────────────────┘
```

### 7.2 线程安全

```cpp
ConnectionPool 的线程安全机制：
- std::mutex mutex_              保护 pool_ 队列和 totalCreated_ 计数
- std::condition_variable cv_    等待/唤醒（acquire 等待 + release 唤醒）
- acquire() 使用 wait_for()      超时返回 nullptr，不会无限阻塞
```

---

## 8. 面试高频问题

### Q1: HTTP 和 RPC 有什么区别？

| 维度 | HTTP | RPC |
|------|------|-----|
| 定位 | 通用应用层协议 | 服务间调用框架 |
| 格式 | 文本协议（可读） | 可以是二进制（高效） |
| 状态 | 无状态 | 可以有状态 |
| 适用 | 对外 API、浏览器 | 内部微服务调用 |

### Q2: 为什么用 Protobuf 而不是 JSON？

| 指标 | JSON | Protobuf |
|------|------|----------|
| 序列化性能 | 基准 | 快 5-10 倍 |
| 数据大小 | 基准 | 小 3-5 倍 |
| 类型安全 | 运行时 | 编译时 |
| 可读性 | 人类可读 | 二进制不可读 |

### Q3: epoll LT 和 ET 的区别？

- **LT（本项目使用）**：有数据就一直通知，编程简单，不会丢事件
- **ET**：状态变化才通知一次，性能略高，但必须循环读到 EAGAIN

### Q4: 项目有哪些技术难点？

1. **TCP 粘包/半包**：HTTP 用 Peek-Parse-Consume 三阶段解决；RPC 用长度前缀协议
2. **TcpConnection 生命周期**：shared_ptr + weak_ptr + 两阶段销毁
3. **跨线程任务调度**：eventfd + swap 队列 + callingPendingFunctors_ 标志
4. **WebSocket 协议**：二进制帧编解码 + 掩码 XOR + SHA1 握手密钥
5. **时间轮定时器**：O(1) 添加/取消，原子 ID 生成

### Q5: 如何保证高并发？

1. **I/O 多路复用**：epoll LT 模式
2. **多线程**：mainReactor + subReactor 模型
3. **连接复用**：HTTP Keep-Alive + 连接池
4. **异步处理**：双缓冲异步日志，不阻塞业务线程
5. **高效定时器**：时间轮 O(1) 管理心跳/超时

### Q6: 如果让你继续优化，会做什么？

1. **HTTP/2**：多路复用、头部压缩
2. **TLS/HTTPS**：SSL 加密传输
3. **内存池**：减少 malloc/free 开销
4. **协程**：同步写法实现异步性能
5. **ET 模式**：减少 epoll_wait 触发次数
