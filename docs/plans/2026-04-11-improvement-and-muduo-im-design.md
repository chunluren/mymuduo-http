# mymuduo-http 改进 + muduo-im 通信软件设计文档

> 日期: 2026-04-11
> 状态: 已批准，待实施

---

## 第一部分：mymuduo-http 改进

### 第一梯队（必须做）

#### 1. WebSocketServer 修复

**问题**: handleHandshake/handleWsFrames/parseHeaders 等方法在 WebSocketServer.h 中声明了但没有内联定义，导致 websocket_server 示例链接错误。

**方案**: 将所有方法体内联到 WebSocketServer.h 中（保持 header-only 一致性）。

**文件**: `src/websocket/WebSocketServer.h`

#### 2. 连接池集成

**问题**: ConnectionPool.h 存在但没有被任何模块使用。muduo-im 需要 MySQL 和 Redis 连接池。

**方案**: 
- 重构 ConnectionPool 为通用模板，支持任意连接类型
- 新增 MySQLPool（基于 libmysqlclient）
- 新增 RedisPool（基于 hiredis）
- 集成到 HttpClient/RpcClient 中可选使用

**文件**: 
- 修改 `src/pool/ConnectionPool.h`
- 新增 `src/pool/MySQLPool.h`
- 新增 `src/pool/RedisPool.h`

#### 3. 自动心跳 + 空闲超时

**问题**: WebSocket Ping/Pong 配置存在但没集成到 EventLoop 定时器。HTTP 连接没有超时机制。

**方案**:
- WebSocketServer: 使用 `loop->runEvery()` 定期发 Ping，超时未 Pong 断开
- HttpServer: 连接建立时 `runAfter()` 启动超时定时器，收到完整请求后取消
- TcpServer: 新增可配置的空闲连接超时

**文件**:
- 修改 `src/websocket/WebSocketServer.h`
- 修改 `src/http/HttpServer.h`

#### 4. 限流 Rate Limiter

**问题**: 没有任何限流机制，无法防止恶意请求。

**方案**: 实现两种限流算法：
- **令牌桶**（Token Bucket）: 平滑限流，允许突发
- **滑动窗口**（Sliding Window）: 精确限流

提供 HttpServer 中间件形式的集成。

**文件**: 
- 新增 `src/util/RateLimiter.h`
- HttpServer 新增 `useRateLimit()` 方法

```cpp
// 使用示例
server.useRateLimit(100, 1);  // 每秒最多 100 个请求

// 或精细控制
RateLimiter limiter(RateLimiter::TokenBucket, 100, 1);
server.use([&limiter](const HttpRequest& req, HttpResponse& resp) {
    if (!limiter.allow(req.getHeader("x-real-ip"))) {
        resp.setStatusCode(HttpStatusCode::TOO_MANY_REQUESTS);
        resp.setBody("Rate limit exceeded");
    }
});
```

### 第二梯队（面试加分）

#### 5. Gzip 压缩

**问题**: HTTP 响应没有压缩，浪费带宽。

**方案**: 
- 检查请求的 `Accept-Encoding` 是否包含 gzip
- 使用 zlib 压缩响应体
- 设置 `Content-Encoding: gzip`
- 作为 HttpServer 中间件，可选开启

**依赖**: zlib（大多数 Linux 自带）

**文件**: 
- 新增 `src/http/GzipMiddleware.h`
- HttpServer 新增 `enableGzip(minSize)` 方法

#### 6. Chunked Transfer Encoding

**问题**: 大响应必须全部加载到内存再发送，不支持流式传输。

**方案**:
- HttpResponse 新增 `setChunkedBody()` 方法
- 发送时按 chunk 格式编码: `大小\r\n数据\r\n...0\r\n\r\n`
- 适用于大文件下载、SSE、流式 API

**文件**: 修改 `src/http/HttpResponse.h`

#### 7. 内存池 / 对象池

**问题**: Buffer、TcpConnection 等频繁创建销毁，每次 malloc/free 有开销。

**方案**: 
- 实现简单的对象池模板 `ObjectPool<T>`
- 预分配 N 个对象，acquire/release 复用
- 先用在 Buffer 上（最频繁分配的对象）

**文件**: 新增 `src/util/ObjectPool.h`

```cpp
template<typename T>
class ObjectPool {
    std::vector<std::unique_ptr<T>> pool_;
    std::queue<T*> available_;
    std::mutex mutex_;
public:
    ObjectPool(size_t initialSize);
    T* acquire();
    void release(T* obj);
};
```

#### 8. 熔断器 Circuit Breaker

**问题**: RPC 客户端调用下游服务失败时没有熔断，会持续发请求拖垮系统。

**方案**: 实现三态熔断器:
- **Closed**: 正常放行，统计失败率
- **Open**: 失败率超阈值 → 直接拒绝所有请求
- **Half-Open**: 定时放一个探测请求，成功则恢复

**文件**: 新增 `src/util/CircuitBreaker.h`

```cpp
class CircuitBreaker {
    enum State { Closed, Open, HalfOpen };
    
    int failureCount_;
    int failureThreshold_;      // 连续失败多少次打开熔断
    int successThreshold_;      // Half-Open 成功多少次恢复
    int timeout_;               // Open 状态持续多久进入 Half-Open
public:
    bool allow();               // 是否允许请求通过
    void recordSuccess();       // 记录成功
    void recordFailure();       // 记录失败
};
```

---

## 第二部分：muduo-im 通信软件

### 项目定位

- 面试作品 + 学习项目
- 基于 mymuduo-http 网络库开发
- 展示系统设计、数据库设计、实时通信能力

### 技术栈

| 层 | 技术 | 说明 |
|----|------|------|
| 前端 | HTML + CSS + JS（单文件） | 原生 WebSocket + fetch API |
| 接入层 | mymuduo-http | WebSocket 实时消息 + HTTP REST API |
| 业务层 | C++ | UserService / MessageService / GroupService / FriendService |
| 存储 | MySQL | 用户 / 消息 / 好友 / 群组 |
| 缓存+队列 | Redis | 在线状态 + 未读计数 + 消息队列 |
| 认证 | JWT + bcrypt | 登录令牌 + 密码哈希 |

### 项目结构

```
muduo-im/
├── third_party/mymuduo-http/     # git submodule
├── src/
│   ├── server/
│   │   ├── ChatServer.h           # 主服务: WebSocket 路由 + HTTP API
│   │   ├── UserService.h          # 注册/登录/JWT
│   │   ├── MessageService.h       # 消息存储/查询/离线
│   │   ├── GroupService.h         # 群聊管理
│   │   ├── FriendService.h        # 好友关系
│   │   ├── OnlineManager.h        # 在线状态 (Redis)
│   │   └── main.cpp
│   ├── db/
│   │   ├── MySQLPool.h            # MySQL 连接池
│   │   └── RedisClient.h          # Redis 封装
│   └── common/
│       ├── JWT.h                  # JWT 生成/验证
│       └── Protocol.h             # 消息协议定义
├── web/
│   └── index.html                 # Web 前端（单文件）
├── sql/
│   └── init.sql                   # 建表脚本
├── tests/
├── CMakeLists.txt
└── README.md
```

### 消息协议 (WebSocket JSON)

```json
// 客户端 → 服务端
{"type":"msg", "to":"userId", "content":"hello", "msgId":"uuid"}
{"type":"group_msg", "to":"groupId", "content":"hello", "msgId":"uuid"}

// 服务端 → 客户端
{"type":"msg", "from":"userId", "to":"me", "content":"hello", "msgId":"uuid", "timestamp":1712700000}
{"type":"ack", "msgId":"uuid"}
{"type":"online", "userId":"xxx"}
{"type":"offline", "userId":"xxx"}
{"type":"error", "message":"..."}
```

### HTTP REST API

| 接口 | 方法 | 说明 |
|------|------|------|
| /api/register | POST | 注册 |
| /api/login | POST | 登录 → JWT |
| /api/friends | GET | 好友列表 |
| /api/friends/add | POST | 添加好友 |
| /api/friends/delete | POST | 删除好友 |
| /api/groups | GET | 群列表 |
| /api/groups/create | POST | 创建群 |
| /api/groups/join | POST | 加入群 |
| /api/groups/members | GET | 群成员 |
| /api/messages/history | GET | 历史消息 |

### 数据库设计 (MySQL)

```sql
-- 用户
CREATE TABLE users (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    username VARCHAR(64) UNIQUE NOT NULL,
    password VARCHAR(128) NOT NULL,
    nickname VARCHAR(64),
    avatar VARCHAR(256) DEFAULT '',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 好友（双向存储）
CREATE TABLE friends (
    user_id BIGINT NOT NULL,
    friend_id BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_id, friend_id),
    INDEX idx_user (user_id)
);

-- 群组
CREATE TABLE groups (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(128) NOT NULL,
    owner_id BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 群成员
CREATE TABLE group_members (
    group_id BIGINT NOT NULL,
    user_id BIGINT NOT NULL,
    joined_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (group_id, user_id),
    INDEX idx_user_groups (user_id)
);

-- 单聊消息（写扩散）
CREATE TABLE private_messages (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    msg_id VARCHAR(36) UNIQUE,
    from_user BIGINT NOT NULL,
    to_user BIGINT NOT NULL,
    content TEXT,
    msg_type TINYINT DEFAULT 0,
    timestamp BIGINT NOT NULL,
    INDEX idx_chat (from_user, to_user, timestamp),
    INDEX idx_inbox (to_user, timestamp)
);

-- 群聊消息（读扩散）
CREATE TABLE group_messages (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    msg_id VARCHAR(36) UNIQUE,
    group_id BIGINT NOT NULL,
    from_user BIGINT NOT NULL,
    content TEXT,
    msg_type TINYINT DEFAULT 0,
    timestamp BIGINT NOT NULL,
    INDEX idx_group_time (group_id, timestamp)
);
```

### Redis 数据结构

```
online:{userId}           → "1" (TTL 30s)        在线状态
unread:{userId}:{peerId}  → 计数                   未读数
msg_queue                 → List [json, ...]       消息队列
```

### 核心流程

```
消息路由:
  WebSocket 消息到达 → 解析 type
  ├── msg: 写 Redis 队列 + 查在线 → 在线转发 / 离线存未读
  └── group_msg: 写队列 + 遍历群成员 → 逐个转发

批量入库:
  后台线程 → Redis lrange 取 100 条 → MySQL batch INSERT → ltrim

认证:
  登录 → JWT → WebSocket 连接带 token → 验证后绑定 userId
```

### 前端

```
单文件 index.html:
  ├── 登录/注册页
  ├── 聊天主界面（好友列表 + 消息窗口 + 输入框）
  ├── 原生 WebSocket 连接
  ├── fetch API 调 REST
  └── localStorage 存 JWT
```

### 可扩展（完整版预留）

- 文件传输（HTTP 上传 + URL 分享）
- 消息撤回（type: recall）
- 已读回执（type: read_ack）
- 正在输入（type: typing）

---

## 实施顺序

```
阶段 1: mymuduo-http 第一梯队改进
  ├── WebSocketServer 修复
  ├── 连接池集成 (MySQLPool + RedisPool)
  ├── 自动心跳 + 空闲超时
  └── 限流 Rate Limiter

阶段 2: mymuduo-http 第二梯队改进
  ├── Gzip 压缩
  ├── Chunked Transfer
  ├── 内存池 / 对象池
  └── 熔断器 Circuit Breaker

阶段 3: muduo-im 服务端
  ├── 项目初始化 (submodule + CMake)
  ├── MySQL/Redis 连接
  ├── UserService (注册/登录/JWT)
  ├── ChatServer (WebSocket 消息路由)
  ├── MessageService (存储/离线/历史)
  ├── FriendService
  └── GroupService

阶段 4: muduo-im 前端 + 联调
  ├── index.html (登录 + 聊天界面)
  ├── 联调测试
  └── 文档
```

### 依赖

| 新增依赖 | 用途 | 安装 |
|---------|------|------|
| libmysqlclient-dev | MySQL C 客户端 | `sudo apt install libmysqlclient-dev` |
| libhiredis-dev | Redis C 客户端 | `sudo apt install libhiredis-dev` |
| zlib | Gzip 压缩 | `sudo apt install zlib1g-dev` |
