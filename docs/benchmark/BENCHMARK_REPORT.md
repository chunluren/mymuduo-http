# mymuduo-http 性能基准测试报告

## wrk 基准测试 (2026-04-11)

环境: Intel i5-12400F (6C/12T), WSL2 Ubuntu 22.04
服务端: 4 IO 线程, 日志关闭, 44B JSON 响应

| 并发 | QPS | Avg Latency | P99 Latency |
|------|-----|------------|-------------|
| 4 | ~17,000 | 233us | - |
| 16 | ~45,000 | 379us | - |
| 64 | ~73,000 | 0.8ms | 2.9ms |
| 128 | ~87,000 | 1.9ms | - |
| 256 | ~91,000 | 2.6ms | 10ms |
| 512 | ~100,000 | 5.2ms | 17ms |

## 优化措施

- epoll events 初始大小 16 -> 128
- 热路径日志移至 LOG_DEBUG (生产编译排除)
- HttpResponse::toString 去除 ostringstream
- 路由 O(1) 哈希匹配 (exactRoutes_)
