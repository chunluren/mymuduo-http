# mymuduo-http 性能优化计划

> 起草: 2026-04-26
> 范围: `src/net/`、`src/http/`、`src/websocket/`、`src/pool/`、`src/asynclogger/`
> 目标读者: 自己 / 后续接手者
> 一句话目标: **不动架构**前提下，把单机吞吐推到 25k+ QPS（HTTP keep-alive，4 worker），WS 长连吞吐到 100k 并发；优先做"已编码但未启用"和"代价小收益高"的事，把架构级重写延后。

## 0. 必读：本计划的取舍原则

1. **先修 bug 再做优化**。`TCP_NODELAY` 这种"代码已写好但没人调用"的口子先关上，不算优化。
2. **每项必须有可量化验收**。没有 benchmark 数字的"优化"不进 main。压测脚本和基线数据先固化。
3. **架构级改动单独立项**。本文档不包括 io_uring 重构、HTTP/2、协程化——这些是另一份 design doc 的事。
4. **回归优先于性能**。任何优化要保留可关闭开关（CMake / 运行时 flag），开关默认按现状，验证完再翻。
5. **优化对象顺序**：网络栈 > I/O 多路复用 > 数据路径 > 应用协议 > 周边基础设施。

---

## 1. 现状基线（验证过，不是猜的）

| 项 | 现状 | 出处 |
|----|------|------|
| `TCP_NODELAY` | 函数已实现 (`Socket.cc:59`)，**全代码无 caller** | grep `setTcpNoDelay` |
| epoll 模式 | LT (level-triggered)，无 ET 支持 | `EPollPoller.cc` |
| 写路径 | 单次 `writeFd`（`write(2)`），无 `writev` | `TcpConnection.cc:106` |
| 读路径 | `readv` 双 iovec，64KB 栈外溢区 | `Buffer.cc` |
| Buffer | 自动扩容 std::vector，prependable 头部 | `Buffer.h` |
| 线程模型 | one-loop-per-thread, round-robin 派发 | `EventLoopThreadPool.cc` |
| Pool 等待 | `std::mutex` + `condition_variable` | `pool/*.h` |
| AsyncLogger | 双缓冲 + 1000 条阈值 + 100ms 超时 | `AsyncLogger.h` |
| TLS | OpenSSL Memory BIO 每连接 1 对 | `HttpsServer.h:208` |
| 已优化 | Gzip/Chunked/CB/限流/HttpCore 抽取 | `CLAUDE.md` |

---

## 2. 优先级矩阵

```
影响  ↑
 高   │  P0: TCP_NODELAY    ★ P1: 业务 worker pool   ★ P2: ET 模式
      │     (1 行 / 0 风险)    (subLoop 不再被慢查询拖)    (改 Poller + 全部读循环)
      │                       P1: writev 合并 header+body
      │
 中   │  P0: 压测基线建立     P1: WS 帧对象池          P2: TLS BIO 池
      │     (没基线一切优化空谈)    (5%-10% 提升)            (TLS 重连场景)
      │
 低   │  P3: 缓存行对齐       P3: AsyncLogger          P3: io_uring 调研
      │     (热原子变量)        lock-free 队列          (>=Linux 5.6)
      │                                                            
      └──────────────────────────────────────────────→
        小                  中                       大     工作量
```

**业务 worker pool 是 P99 改善的最大杠杆**：当前 IO 线程被 DB/Redis 调用同步阻塞，
单条慢查询会拖垮整条 subLoop 上的几千连接。这一项收益比所有 Phase 3/4 微优化加起来都大。

---

## 3. 任务清单（按 Phase）

### Phase 0 — 准备工作（W0，1-2 天）

#### 0.1 压测基线建立 ★必须先做

**目的**: 没有基线，所有"优化提升 X%"都是骗自己。

**交付**:
- `benchmark/perf/` 目录 + 一个 `baseline.sh` 脚本：
  - HTTP keep-alive：`wrk -t4 -c200 -d30s http://127.0.0.1:8080/health`
  - HTTP short conn：`ab -n 50000 -c 100 http://127.0.0.1:8080/health`
  - WS 并发：现有 `benchmark/ws_bench.cpp` 跑 1k / 5k / 10k 并发
  - HTTPS：同 HTTP 跑一份 TLS 版本
- `benchmark/perf/baseline.md`：每条命令对应的 RPS/P50/P99/CPU%/RSS
- `Makefile` target `make perf-baseline` 一键生成

**验收**: 在 main 上跑出一份基线数据进 `baseline.md`，commit。后续每个优化的 PR 必须附带一份"优化后"对比表。

---

### Phase 1 — 已编码但未启用 / 一行修复（W1，半天）

#### 1.1 启用 `TCP_NODELAY`（P0）

**问题**: Nagle 算法会把小包聚合最多 40ms，对 IM ack / 短 HTTP 响应是直接灾难。代码已写好就是没人调。

**改动** (`TcpServer.cc` 中 newConnection 创建 TcpConnection 后):
```cpp
TcpConnectionPtr conn(new TcpConnection(...));
conn->socket().setTcpNoDelay(true);  // 加这一行
conn->socket().setKeepAlive(true);   // 顺便补这个
```

或者把这个移到 `TcpConnection` 构造函数里默认开启，给 `setNoDelay(false)` 留个 setter。

**风险**: 极低。Nagle 关掉只会让小包多发几次，绝不会出错。

**验收**:
- `wrk` 单连接 single-request 模式 P50 应该从 ~40ms 跌到 < 1ms
- IM ack 端到端延迟（topology_e2e.py 的 ack-latency）下降明显
- 有/无的对比表进 commit message

---

#### 1.2 启用 SO_KEEPALIVE 默认 + 调参（P1）

**问题**: 默认 keepalive 探测间隔 7200s，等于"半连接"撑两小时也没人发现。

**改动**: 在 `Socket.cc` 加 `setKeepAliveTime(idle, intvl, count)` 用 `TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT`，默认 `60/10/3`（一分钟空闲后开始探，10s 一次，3 次断）。

**风险**: 长连接 keepalive 失败会更早断；这本来就是想要的。

**验收**: ws 客户端拔网线 90s 内服务端能感知 close。

---

### Phase 2 — 数据路径优化（W2-W3）

#### 2.1 写路径加 `writev`（P1，半天）

**问题**: HTTP 响应分两次 append（header + body）→ outputBuffer 内拼一起 → write() 一次系统调用。这本身没问题。**但** 如果 body 是大文件读出来的几 MB，header 一起 append 进 outputBuffer 会触发 vector 扩容拷贝。

**方案**: 给 `TcpConnection::send` 加一个 `sendIov(iovec*, count)` 接口；HttpResponse 序列化时 header 和 body 分两个 iovec，writev 一把出去。

**改动文件**:
- `src/net/TcpConnection.{h,cc}`: 加 `sendIov` / `sendIovInLoop`
- `src/http/HttpResponse.{h,cc}` 或 `HttpCore`: 用新接口拼响应

**风险**: 中。要处理"writev 没写完"的部分回退到 outputBuffer 的逻辑——和现有 send 的退化逻辑一致。

**验收**: 1MB body 响应的 CPU% 下降；wrk 大响应吞吐提升 ≥ 10%。

---

#### 2.2 静态文件 `sendfile(2)` 零拷贝（P2，1 天）

**问题**: 当前 `serveStatic` 一次性读全文件入内存再 send，10MB 文件就是 10MB 内存 + 一次完整拷贝。

**方案**:
- `TcpConnection` 加 `sendFile(int fd, off_t offset, size_t count)` 接口
- 实现走 `sendfile(2)` 直接内核态零拷贝
- HttpsServer 不能用（TLS 必须先加密）→ 走原路径
- HttpServer 静态文件路径切到 sendFile

**约束**: sendfile 要求 dst 是 socket、src 是 regular file（不能是 pipe）；fd 不能跨 EventLoop 传递（要 dup）。

**验收**: 100MB 静态文件下载 RSS 不增；网络吞吐打满千兆口。

---

#### 2.3 Buffer 池化 / 预分配（P2，1 天）

**问题**: 高并发下每连接都 new 一个 Buffer，readableBytes 满了 vector 扩容（1.5x 或 2x）会拷贝。

**方案 A（轻）**: 给 Buffer 一个"初始尺寸"参数，TcpConnection 构造时按 `tcpInitBufferSize=4KB` 预分配，避免短连接频繁小扩容。

**方案 B（重）**: 引入 SlabAllocator 把 Buffer 的内存按 1KB/4KB/16KB/64KB 分级池化。这一步要小心，慎做。

**验收**: 短连接基准（ab）下，perf 火焰图里 `__memmove_avx_unaligned_erms` 比例下降。

---

### Phase 3 — I/O 多路复用升级（W4，1-2 周）

#### 3.1 EPoll ET 模式（P2，3-5 天）★高风险

**问题**: LT 模式下，一次可读事件会被 epoll_wait 反复返回直到 buffer 全读空。高并发短包场景 epoll_wait 开销显著。ET 一次通知就要把数据全读完，但 epoll_wait 调用频率显著降低。

**改动范围（评估）**:
- `EPollPoller.cc`: 注册时 `events |= EPOLLET`
- `Channel.cc`: enableReading/Writing 时设置 ET 标志位（按 fd 类型可选）
- `TcpConnection::handleRead`: 必须**循环 read 到 EAGAIN**，否则 ET 下数据永远拉不完
- `TcpConnection::handleWrite`: 同样要循环 write 到 EAGAIN
- `Acceptor::handleRead`: accept 也要循环（ET 下没拿干净，下次新连接来才会再次唤醒）
- 所有 `Buffer::readFd` 已经返回 n>0 时再读直到 -1/EAGAIN？**不是**，要改

**风险**: 高。漏读一个 ET 边沿就是死锁，连接挂着没人理。
- 必须有覆盖率测试：accept 多并发触发、readFd 返回 EAGAIN 处理、SSL_read 的 WANT_READ 与 ET 交互

**前置**: 必须先有 Phase 0 的压测基线 + 必须有 stress test。

**做或不做的决策点**:
- 如果 Phase 1+2 已经把 P99 降到目标内，**这步可以延后**。
- 如果性能要再上一个台阶，再做这步。

**验收**:
- 同样并发下 epoll_wait 调用次数下降 3-5x（用 `strace -c` 验）
- 没有连接卡死、没有数据丢失（跑现有 e2e 全套 + 1 小时长跑）
- 可通过 `EPollPoller.useEdgeTriggered(false)` 退回 LT，保留逃生通道

---

#### 3.2 io_uring 调研（P3，调研 2 天 / 实现 1-2 周）

**问题**: epoll 是 readiness 模型（"可读了告诉你"），io_uring 是 completion 模型（"读好了告诉你"），减少 1 次系统调用 + 真异步。

**调研内容**:
- 内核版本：项目最低支持的发行版？Ubuntu 22.04 内核 5.15 ✓，CentOS 7 (3.10) ✗
- liburing 依赖
- 是否值得：epoll + ET 优化后的天花板距离 io_uring 多远？
- 是否要重写 EventLoop 还是新加一个 `IoUringPoller : public Poller`

**输出**: 一份 `2026-XX-XX-iouring-feasibility.md`，决定 do / don't / when。

**默认假设**: 在 IM 场景下 epoll+ET 已经够用，io_uring 性价比低。**Phase 3.2 倾向于"调研后不做"，把精力放业务**。

---

### Phase 4 — 应用层与基础设施（W5）

#### 4.0 业务 worker pool（P1，2-3 天）★结构性改动

**问题**：当前所有业务回调（HTTP handler、WS messageHandler）直接跑在 IO 线程（subLoop）上。
`MySQLPool::acquire`、`RedisPool::acquire`、ES HTTP 调用、Multipart 解析这些都是**阻塞**的，
一次 200ms 慢查询会**直接卡住整条 subLoop**——这条 loop 上其他几千连接的消息全部停摆。

具体场景（已能复现）：
- subLoop-2 挂着 2500 个 ws 连接
- user 42 触发一次聚合慢查询（200ms）
- 这 200ms 内 subLoop-2 上其他 2499 个用户的 ack / typing / push 全部延迟

**方案**：选择性派发（不是全员异步），分两层：

1. **`mymuduo-http` 层**：在 `src/util/` 加通用 `ThreadPool`（不绑定 EventLoop），固定 N 线程 + 阻塞队列。
   - 接口：`submit(std::function<void()>)`、`submitAffinity(uint64_t key, fn)`（按 key hash 路由到固定 worker，保序）
   - 内部用 `std::vector<ThreadPool::Worker>`，每个 worker 一个 `std::queue + mutex + cv`

2. **业务层（muduo-im）**：在 ChatServer 持有 `ThreadPool workerPool_`（默认 8 线程，可配）。
   只把**含 DB / 外部 IO 的 handler** 切过去：
   ```cpp
   // 慢路径：丢 worker
   void handlePrivateMessage(session, json) {
       auto loop = session->getLoop();
       workerPool_.submitAffinity(session->connId(), [=]() {
           // 这里跑 MySQL 写 + 推送
           auto result = doHandlePrivateMessage(json);
           // 切回 IO loop 发响应（ws send 必须在 IO 线程做）
           loop->runInLoop([session, result]() {
               session->sendText(result);
           });
       });
   }

   // 快路径：留 IO 线程
   void handleTyping(session, json) {
       broadcastTyping(session, json);  // 纯内存操作不切
   }
   ```

**关键细节**：

- **保序**：同一连接的多条消息**必须**用 `submitAffinity(conn_id)` 路由到固定 worker，否则
  ws send 顺序会乱。
- **回 IO 切线程**：`sendText` 不能在 worker 线程直接调（TcpConnection 要求所属 loop 调用），
  必须 `loop->runInLoop`。
- **慢/快路径分类**：列一张表登记每个 handler 是哪类。建议默认按 handler 名后缀 `*FastHandler` /
  `*SlowHandler` 区分，编译期可见。
- **背压**：worker 队列要有上限（比如 10k 任务），超了直接 drop + 返回 503/error，不要无限堆积。

**风险**：

- 跨线程切换 +10us 左右，纯轻量回调（只 send ack）走 worker 反而慢——所以**不能全员派发**。
- worker 间 false sharing 可能拖性能：每个 worker 的队列对象 `alignas(64)`。
- 业务里如果错把"等待 ws send 完成"和 worker 工作交织（比如 `waitForResponse()`），会死锁。
  worker 永远不应该 block 等 IO loop 的事件——单向流。

**改动文件**：

- 新增：`mymuduo-http/src/util/ThreadPool.h`
- 改：`muduo-im/src/server/ChatServer.h` 持有 `workerPool_`，把所有 `handle*Message` 中含
  DB 调用的部分包到 `workerPool_.submitAffinity` 里
- 测试：`tests/test_thread_pool.cpp` 验证保序 + 背压 + 关闭

**验收**：

- 注入测试：mock 一个 `MessageService::savePrivateMessage` 实现 `sleep(200ms)`，
  并发 100 个 ws 连接同时发消息——"非自己"那 99 个连接的 ack 延迟应该 ≤ 20ms（之前会全部 ≥ 200ms）
- topology_e2e.py 跑 50×20=1000 条消息，加跑一组人为慢查询注入，对比延迟分布
- perf record 看 IO 线程 CPU% 应该下降，worker 线程 CPU% 上升（总量持平就对了）

**WHY 不该被推迟**：

这条比 Phase 4.1-4.4 的微优化收益大一个数量级。Phase 4.0 改完之后，
**P99 不再被偶发慢查询牵着走**，比 ET / lock-free queue 这些纯吞吐优化更影响"体感"。
建议提到 Phase 2 之后立刻做，排在 Phase 3 ET 之前。

---

#### 4.1 WebSocket 帧对象池（P2，1 天）

**问题**: 每次解出一个 WS frame 就 new 一个 `vector<uint8_t>` payload。WS 长连接吞吐场景下 GC（heap allocation）压力可见。

**方案**: 给 `WsMessage` 加一个固定 4KB 栈缓冲做 SSO（小消息免堆分配）；超过走 ObjectPool（已有）。

**验收**: 1k 并发各发 1KB 文本 30s，perf 看 `operator new` 比例下降。

---

#### 4.2 ConnectionPool 用 lock-free 队列（P3，2 天）

**问题**: 当前 `std::mutex + condition_variable`，几千 QPS 之后 wait/notify 切换上下文。

**方案**: 用 `boost::lockfree::queue` 或自己写 MPMC 队列；wait 改成 1us 自旋 + futex 退避。

**风险**: lock-free 实现自己写容易写错；用现成库要加依赖。

**先决条件**: perf 显示 mutex contention 在前 5 名才做。Phase 0 跑完再判断。

---

#### 4.3 AsyncLogger 调优（P3，半天）

- `currentBuffer_` / `flushBuffer_` 增加 cache-line 对齐（`alignas(64)`）
- 阈值从硬编码 1000 改为可配
- 落盘 fsync 频率参数化

**验收**: 高并发日志场景，前后台线程不再互相争锁。

---

#### 4.4 热路径 cache-line 对齐（P3，半天）

**目标变量**:
- `EventLoop::quit_` (atomic_bool)
- `AsyncLogger::running_`、`level_`
- `TcpConnection::state_`

加 `alignas(64)` 防 false sharing。

**验收**: 对照组（一个 4 路并发的小 benchmark）跑差值；不一定明显，做就做了。

---

### Phase 5 — TLS 路径优化（按需）

#### 5.1 OpenSSL BIO 池（P2 / 短连接 HTTPS 才有意义）

**问题**: 每个 HTTPS 连接 `BIO_new(BIO_s_mem())` × 2，连接关闭时 free。短连接场景下分配-释放频繁。

**方案**: BIO 对象池，复用 read/write BIO（注意 `BIO_reset()`）。

**约束**: 长连接（IM WSS）场景这个开销可忽略，**Phase 5.1 优先级低**，除非项目真有 HTTPS 短连场景。

---

#### 5.2 SSL session 复用（P2）

**问题**: 每个 TLS 连接做完整握手 ~3 RTT。复用 session 可降到 1 RTT。

**方案**: 启用 `SSL_CTX_set_session_cache_mode(SSL_SESS_CACHE_SERVER)` + ticket。

**风险**: 低，OpenSSL 内置。

**验收**: HTTPS 重连耗时下降 60%+。

---

## 4. 验收门槛（每个 Phase 出口）

| Phase | 完成标志 |
|-------|---------|
| 0 | `baseline.md` 落库；CI 跑 perf 不报错（数字不卡门槛，纯回归） |
| 1 | TCP_NODELAY commit 包含 wrk before/after 对比；topology_e2e P99 下降 |
| 2 | writev / sendfile 各自的 wrk 数据 + perf 火焰图对比 |
| 4.0 | 慢查询注入测试中"非自己"连接 ack 延迟 ≤ 20ms（基线下会被拖到 ≥ 200ms）|
| 3.1 | ET 模式下 24h 长跑稳定，epoll_wait 调用降 3x+ |
| 3.2 | 出 feasibility doc 决议（do / not now / never） |
| 4.1-4.4 | WS 帧对象池 perf 数字；其他改动 P95 内回归 |
| 5 | HTTPS 重连 RTT 下降 |

---

## 5. 不做的事（明确划界）

以下每件都值得讨论但**不在本次计划**：

1. **HTTP/2 多路复用** — 架构级，要重写 connection layer
2. **io_uring 重构** — Phase 3.2 调研后再决定
3. **协程化（C++20 coroutine / boost::asio）** — 改 reactor model，工作量太大
4. **连接迁移 / SO_REUSEPORT 多进程** — 部署侧问题，不是库问题
5. **eBPF based observability** — 运维基建，不是网络库优化
6. **kTLS / DTLS** — 看真实需求
7. **mmap 文件读** — sendfile 是更优解
8. **Buffer 用 ring buffer** — 重写 Buffer 不值

---

## 6. 时间估算与里程碑

| 周 | 内容 | 累计预期 |
|----|------|---------|
| W0 | Phase 0 压测基线 | 基线就位 |
| W1 | 1.1 + 1.2 一行修复 | P50 ack 显著下降 |
| W2 | 2.1 writev + 2.2 sendfile | 大响应 / 文件 CPU 下降 |
| W3 | **4.0 业务 worker pool** ★ + 2.3 Buffer 预分配 | **P99 不再被慢查询牵走** |
| W4 | 3.1 ET 模式（高风险） | 高并发 epoll 调用降 |
| W5 | 4.1-4.4 应用层调优 | 锦上添花 |
| W6 | 5.x TLS 优化（按需） | HTTPS 场景受益 |

**注**：4.0 业务 worker pool 虽然编号在 Phase 4，但**实际应该在 W3 跟 Phase 2 一起做**，
不要等到 W5。原因是它对 P99 影响最大，先把这个杠杆撬动比纯吞吐优化优先级更高。

**保守预期**：W1+W2 完成时单机 HTTP keep-alive 吞吐应该已经从基线到 +30~50%。W4 之后再 +10-20%。再多就是边际收益。

---

## 7. 失败模式与回滚

每项优化都必须满足两条：

1. **可关闭**：CMake 选项或运行时 flag。例如 `cmake -DMYMUDUO_USE_EPOLL_ET=OFF`。
2. **可回归**：现有 e2e 测试套必须全绿才能合 main。

如果某项优化跑出来发现 P99 反而恶化（比如 ET 在某些 workload 下因为读循环带来 CPU 抖动），**立即回滚不犹豫**，写一份 postmortem 进 docs。

---

## 8. 启动建议

最务实的入口：

```bash
# 第一步：建基线（半天）
cd benchmark
./baseline.sh > baseline-$(date +%Y%m%d).md
git add baseline-*.md && git commit -m "perf: baseline 2026-04-26"

# 第二步：先把"代码已写但没启用"的 TCP_NODELAY 修了
# 改 TcpServer.cc + TcpConnection.cc 一行，跑 baseline 对比，commit

# 第三步：再决定 Phase 2/3 的顺序
```

**别先做 ET / io_uring**——这些是诱人但代价大的优化。先把"白白浪费的 40ms Nagle 延迟"省回来再说。

---

## 9. 附：关键文件指针

| 任务 | 主要文件 |
|------|---------|
| 1.1 TCP_NODELAY | `src/net/TcpServer.cc` `newConnection()` |
| 1.2 KeepAlive | `src/net/Socket.cc` |
| 2.1 writev | `src/net/TcpConnection.{h,cc}`、`src/http/HttpResponse.cc` |
| 2.2 sendfile | `src/net/TcpConnection.{h,cc}`、`src/http/HttpServer.h::serveStatic` |
| 2.3 Buffer init | `src/net/Buffer.h`、`src/net/TcpConnection.cc` |
| **4.0 worker pool** | 新建 `mymuduo-http/src/util/ThreadPool.h`；`muduo-im/src/server/ChatServer.h` 把 `handle*Message` 含 DB 调用部分包到 `workerPool_.submitAffinity` |
| 3.1 ET | `src/net/EPollPoller.{h,cc}`、`src/net/Channel.cc`、`src/net/TcpConnection.cc::handleRead/Write`、`src/net/Acceptor.cc` |
| 4.1 WS pool | `src/websocket/WsSession.h`、`src/websocket/WebSocketFrame.h` |
| 5.1 BIO pool | `src/http/HttpsServer.h::onConnection` |
