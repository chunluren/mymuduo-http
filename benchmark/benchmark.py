#!/usr/bin/env python3
"""
性能压测脚本
支持 HTTP 和 RPC 压测
"""

import argparse
import asyncio
import aiohttp
import json
import time
import statistics
from dataclasses import dataclass
from typing import List, Optional
import os
import sys

@dataclass
class RequestResult:
    """单个请求结果"""
    success: bool
    latency_ms: float
    status_code: int
    error: Optional[str] = None

@dataclass
class BenchmarkStats:
    """压测统计"""
    total_requests: int
    successful_requests: int
    failed_requests: int
    total_time_s: float
    avg_latency_ms: float
    min_latency_ms: float
    max_latency_ms: float
    p50_latency_ms: float
    p90_latency_ms: float
    p99_latency_ms: float
    qps: float

    def __str__(self):
        return f"""
========================================
性能测试报告
========================================
总请求数:       {self.total_requests:,}
成功请求:       {self.successful_requests:,}
失败请求:       {self.failed_requests:,}
总耗时:         {self.total_time_s:.2f}s
----------------------------------------
平均延迟:       {self.avg_latency_ms:.2f}ms
最小延迟:       {self.min_latency_ms:.2f}ms
最大延迟:       {self.max_latency_ms:.2f}ms
P50 延迟:       {self.p50_latency_ms:.2f}ms
P90 延迟:       {self.p90_latency_ms:.2f}ms
P99 延迟:       {self.p99_latency_ms:.2f}ms
----------------------------------------
QPS:            {self.qps:,.2f} req/s
========================================
"""

def calculate_stats(results: List[RequestResult], total_time: float) -> BenchmarkStats:
    """计算统计数据"""
    successful = [r for r in results if r.success]
    failed = [r for r in results if not r.success]

    latencies = sorted([r.latency_ms for r in successful])

    if not latencies:
        return BenchmarkStats(
            total_requests=len(results),
            successful_requests=0,
            failed_requests=len(failed),
            total_time_s=total_time,
            avg_latency_ms=0,
            min_latency_ms=0,
            max_latency_ms=0,
            p50_latency_ms=0,
            p90_latency_ms=0,
            p99_latency_ms=0,
            qps=0
        )

    def percentile(data: List[float], p: float) -> float:
        """计算百分位数"""
        if not data:
            return 0
        k = (len(data) - 1) * p / 100
        f = int(k)
        c = f + 1 if f + 1 < len(data) else f
        return data[f] + (k - f) * (data[c] - data[f])

    return BenchmarkStats(
        total_requests=len(results),
        successful_requests=len(successful),
        failed_requests=len(failed),
        total_time_s=total_time,
        avg_latency_ms=statistics.mean(latencies),
        min_latency_ms=min(latencies),
        max_latency_ms=max(latencies),
        p50_latency_ms=percentile(latencies, 50),
        p90_latency_ms=percentile(latencies, 90),
        p99_latency_ms=percentile(latencies, 99),
        qps=len(results) / total_time if total_time > 0 else 0
    )

async def http_benchmark(
    url: str,
    concurrency: int,
    total_requests: int,
    method: str = "GET",
    payload: Optional[dict] = None
) -> BenchmarkStats:
    """HTTP 压测"""

    results: List[RequestResult] = []
    semaphore = asyncio.Semaphore(concurrency)
    request_count = 0
    request_lock = asyncio.Lock()

    async def make_request(session: aiohttp.ClientSession) -> RequestResult:
        nonlocal request_count

        async with semaphore:
            async with request_lock:
                if request_count >= total_requests:
                    return None
                request_count += 1

            start = time.perf_counter()
            try:
                if method == "GET":
                    async with session.get(url) as resp:
                        await resp.read()
                        latency = (time.perf_counter() - start) * 1000
                        return RequestResult(
                            success=200 <= resp.status < 300,
                            latency_ms=latency,
                            status_code=resp.status
                        )
                else:
                    async with session.post(url, json=payload) as resp:
                        await resp.read()
                        latency = (time.perf_counter() - start) * 1000
                        return RequestResult(
                            success=200 <= resp.status < 300,
                            latency_ms=latency,
                            status_code=resp.status
                        )
            except Exception as e:
                latency = (time.perf_counter() - start) * 1000
                return RequestResult(
                    success=False,
                    latency_ms=latency,
                    status_code=0,
                    error=str(e)
                )

    start_time = time.perf_counter()

    connector = aiohttp.TCPConnector(limit=concurrency * 2, ttl_dns_cache=300)
    timeout = aiohttp.ClientTimeout(total=30)

    async with aiohttp.ClientSession(connector=connector, timeout=timeout) as session:
        tasks = []
        while request_count < total_requests:
            task = asyncio.create_task(make_request(session))
            tasks.append(task)

        done, _ = await asyncio.wait(tasks, return_when=asyncio.ALL_COMPLETED)

    total_time = time.perf_counter() - start_time

    for task in done:
        result = task.result()
        if result:
            results.append(result)

    return calculate_stats(results, total_time)

async def rpc_benchmark(
    url: str,
    concurrency: int,
    total_requests: int,
    method: str,
    params: dict
) -> BenchmarkStats:
    """RPC 压测"""

    payload = {
        "jsonrpc": "2.0",
        "method": method,
        "params": params,
        "id": 1
    }

    return await http_benchmark(
        url=url,
        concurrency=concurrency,
        total_requests=total_requests,
        method="POST",
        payload=payload
    )

def generate_markdown_report(
    http_stats: Optional[BenchmarkStats],
    rpc_json_stats: Optional[BenchmarkStats],
    rpc_pb_stats: Optional[BenchmarkStats],
    output_file: str
):
    """生成 Markdown 报告"""

    report = f"""# mymuduo-http 性能压测报告

生成时间: {time.strftime('%Y-%m-%d %H:%M:%S')}

## 测试环境

- 操作系统: Linux
- 并发数: 可配置
- 请求总数: 可配置

## HTTP 性能测试

"""

    if http_stats:
        report += f"""
| 指标 | 值 |
|------|-----|
| 总请求数 | {http_stats.total_requests:,} |
| 成功请求 | {http_stats.successful_requests:,} |
| 失败请求 | {http_stats.failed_requests:,} |
| 总耗时 | {http_stats.total_time_s:.2f}s |
| 平均延迟 | {http_stats.avg_latency_ms:.2f}ms |
| 最小延迟 | {http_stats.min_latency_ms:.2f}ms |
| 最大延迟 | {http_stats.max_latency_ms:.2f}ms |
| P50 延迟 | {http_stats.p50_latency_ms:.2f}ms |
| P90 延迟 | {http_stats.p90_latency_ms:.2f}ms |
| P99 延迟 | {http_stats.p99_latency_ms:.2f}ms |
| **QPS** | **{http_stats.qps:,.2f} req/s** |

"""
    else:
        report += "*测试未执行*\n\n"

    report += "## JSON-RPC 性能测试\n\n"

    if rpc_json_stats:
        report += f"""
| 指标 | 值 |
|------|-----|
| 总请求数 | {rpc_json_stats.total_requests:,} |
| 成功请求 | {rpc_json_stats.successful_requests:,} |
| 失败请求 | {rpc_json_stats.failed_requests:,} |
| 总耗时 | {rpc_json_stats.total_time_s:.2f}s |
| 平均延迟 | {rpc_json_stats.avg_latency_ms:.2f}ms |
| P50 延迟 | {rpc_json_stats.p50_latency_ms:.2f}ms |
| P90 延迟 | {rpc_json_stats.p90_latency_ms:.2f}ms |
| P99 延迟 | {rpc_json_stats.p99_latency_ms:.2f}ms |
| **QPS** | **{rpc_json_stats.qps:,.2f} req/s** |

"""
    else:
        report += "*测试未执行*\n\n"

    report += "## Protobuf-RPC 性能测试\n\n"

    if rpc_pb_stats:
        report += f"""
| 指标 | 值 |
|------|-----|
| 总请求数 | {rpc_pb_stats.total_requests:,} |
| 成功请求 | {rpc_pb_stats.successful_requests:,} |
| 失败请求 | {rpc_pb_stats.failed_requests:,} |
| 总耗时 | {rpc_pb_stats.total_time_s:.2f}s |
| 平均延迟 | {rpc_pb_stats.avg_latency_ms:.2f}ms |
| P50 延迟 | {rpc_pb_stats.p50_latency_ms:.2f}ms |
| P90 延迟 | {rpc_pb_stats.p90_latency_ms:.2f}ms |
| P99 延迟 | {rpc_pb_stats.p99_latency_ms:.2f}ms |
| **QPS** | **{rpc_pb_stats.qps:,.2f} req/s** |

"""
    else:
        report += "*测试未执行*\n\n"

    report += """## 性能对比

| 协议 | QPS | 平均延迟 | 适用场景 |
|------|-----|---------|---------|
"""

    if http_stats:
        report += f"| HTTP | {http_stats.qps:,.0f} | {http_stats.avg_latency_ms:.2f}ms | Web 服务 |\n"
    if rpc_json_stats:
        report += f"| JSON-RPC | {rpc_json_stats.qps:,.0f} | {rpc_json_stats.avg_latency_ms:.2f}ms | 内部服务调用 |\n"
    if rpc_pb_stats:
        report += f"| Protobuf-RPC | {rpc_pb_stats.qps:,.0f} | {rpc_pb_stats.avg_latency_ms:.2f}ms | 高性能场景 |\n"

    report += """
## 优化建议

1. **连接复用**: 启用 HTTP Keep-Alive 以减少连接建立开销
2. **线程数调优**: 根据 CPU 核心数调整工作线程数
3. **缓冲区大小**: 根据业务数据大小调整 TCP 缓冲区
4. **异步日志**: 使用异步日志避免 I/O 阻塞

## 结论

mymuduo-http 提供了高性能的 HTTP 和 RPC 服务能力，适用于：
- Web API 服务
- 微服务架构
- 高并发场景
"""

    with open(output_file, 'w', encoding='utf-8') as f:
        f.write(report)

    print(f"报告已保存到: {output_file}")

async def main():
    parser = argparse.ArgumentParser(description='mymuduo-http 性能压测工具')
    parser.add_argument('--type', choices=['http', 'rpc', 'all'], default='all',
                        help='测试类型')
    parser.add_argument('--host', default='127.0.0.1', help='服务器地址')
    parser.add_argument('--http-port', type=int, default=8080, help='HTTP 端口')
    parser.add_argument('--rpc-port', type=int, default=8081, help='RPC 端口')
    parser.add_argument('--rpc-pb-port', type=int, default=8082, help='Protobuf RPC 端口')
    parser.add_argument('--concurrency', type=int, default=100, help='并发数')
    parser.add_argument('--requests', type=int, default=10000, help='总请求数')
    parser.add_argument('--output', default='benchmark/report.md', help='报告输出路径')

    args = parser.parse_args()

    http_stats = None
    rpc_json_stats = None
    rpc_pb_stats = None

    print(f"开始性能压测 (并发: {args.concurrency}, 总请求: {args.requests})")
    print("=" * 50)

    # HTTP 测试
    if args.type in ['http', 'all']:
        print(f"\n[HTTP] 测试 http://{args.host}:{args.http_port}/api/hello")
        http_stats = await http_benchmark(
            url=f"http://{args.host}:{args.http_port}/api/hello",
            concurrency=args.concurrency,
            total_requests=args.requests,
            method="GET"
        )
        print(http_stats)

    # JSON-RPC 测试
    if args.type in ['rpc', 'all']:
        print(f"\n[JSON-RPC] 测试 http://{args.host}:{args.rpc_port}/rpc")
        rpc_json_stats = await rpc_benchmark(
            url=f"http://{args.host}:{args.rpc_port}/rpc",
            concurrency=args.concurrency,
            total_requests=args.requests,
            method="calc.add",
            params={"a": 10, "b": 20}
        )
        print(rpc_json_stats)

    # Protobuf-RPC 测试 (需要单独的客户端程序)
    # rpc_pb_stats 需要使用 C++ 客户端测试

    # 生成报告
    generate_markdown_report(http_stats, rpc_json_stats, rpc_pb_stats, args.output)

if __name__ == '__main__':
    asyncio.run(main())