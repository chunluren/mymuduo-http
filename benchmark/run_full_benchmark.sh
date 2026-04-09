#!/bin/bash
# run_full_benchmark.sh — 自动化压测：启动服务 → 跑多种配置 → 生成报告
set -e

cd "$(dirname "$0")/.."
BUILD_DIR="build"
REPORT_FILE="benchmark/BENCHMARK_REPORT.md"
PORT=19999
PID_FILE="/tmp/mymuduo_bench_server.pid"

# 颜色
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# ==================== 构建 ====================
echo -e "${GREEN}=== 构建项目 ===${NC}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
make -j$(nproc) http_server benchmark_client 2>&1 | tail -3
cd ..

# ==================== 清理 ====================
cleanup() {
    if [ -f "$PID_FILE" ]; then
        kill $(cat "$PID_FILE") 2>/dev/null || true
        rm -f "$PID_FILE"
    fi
}
trap cleanup EXIT

# ==================== 启动服务 ====================
start_server() {
    local threads=$1
    echo -e "${YELLOW}启动 HTTP 服务器 (threads=$threads, port=$PORT)${NC}"

    cleanup  # 先清理旧进程

    # 启动服务器（后台运行）
    # 由于 http_server 示例监听 8080，我们直接用它
    cd "$BUILD_DIR"
    ./http_server &
    echo $! > "$PID_FILE"
    cd ..

    sleep 1  # 等服务启动

    # 验证服务已启动
    if ! kill -0 $(cat "$PID_FILE") 2>/dev/null; then
        echo "服务启动失败！"
        exit 1
    fi
}

# ==================== 运行压测 ====================
run_benchmark() {
    local type=$1
    local threads=$2
    local requests=$3
    local port=$4

    echo -e "${GREEN}--- $type | $threads 线程 | $requests 请求/线程 | 总 $((threads * requests)) ---${NC}"

    "$BUILD_DIR/benchmark_client" \
        --type "$type" \
        --host 127.0.0.1 \
        --port "$port" \
        --threads "$threads" \
        --requests "$requests" 2>&1
}

# ==================== 生成报告 ====================
generate_report() {
    local date=$(date '+%Y-%m-%d %H:%M:%S')
    local cpu=$(grep -c processor /proc/cpuinfo)
    local mem=$(free -h | awk '/Mem:/ {print $2}')
    local kernel=$(uname -r)

    cat > "$REPORT_FILE" << EOF
# mymuduo-http 压测报告

> 生成时间: $date

## 测试环境

| 项目 | 值 |
|------|-----|
| CPU 核数 | $cpu |
| 内存 | $mem |
| 内核 | $kernel |
| 编译模式 | Release (-O2) |
| 服务器线程 | 4 (默认) |

## HTTP 压测结果

EOF
}

append_result() {
    local label=$1
    local output=$2

    # 解析输出中的关键指标
    local qps=$(echo "$output" | grep "QPS:" | awk '{print $2}')
    local avg=$(echo "$output" | grep "平均延迟:" | awk '{print $2}')
    local p50=$(echo "$output" | grep "P50" | awk '{print $3}')
    local p90=$(echo "$output" | grep "P90" | awk '{print $3}')
    local p99=$(echo "$output" | grep "P99" | awk '{print $3}')
    local success=$(echo "$output" | grep "成功请求:" | awk '{print $2}')
    local failed=$(echo "$output" | grep "失败请求:" | awk '{print $2}')

    cat >> "$REPORT_FILE" << EOF
### $label

| 指标 | 值 |
|------|-----|
| QPS | ${qps:-N/A} req/s |
| 平均延迟 | ${avg:-N/A} ms |
| P50 延迟 | ${p50:-N/A} ms |
| P90 延迟 | ${p90:-N/A} ms |
| P99 延迟 | ${p99:-N/A} ms |
| 成功 | ${success:-N/A} |
| 失败 | ${failed:-N/A} |

EOF
}

# ==================== 主流程 ====================
echo ""
echo "========================================"
echo "  mymuduo-http 自动化压测"
echo "========================================"
echo ""

# 初始化报告
generate_report

# 启动服务（http_server 默认 8080 端口，4 线程）
start_server 4

# HTTP 压测 — 不同并发度
echo ""
echo "========================================"
echo "  HTTP 压测"
echo "========================================"

for threads in 1 2 4 8; do
    output=$(run_benchmark http $threads 2000 8080)
    echo "$output"
    append_result "HTTP — ${threads} 并发线程 × 2000 请求" "$output"
    sleep 1
done

# HTTP 压测 — 大量请求
output=$(run_benchmark http 4 5000 8080)
echo "$output"
append_result "HTTP — 4 并发线程 × 5000 请求（长测）" "$output"

# 清理
cleanup

echo ""
cat >> "$REPORT_FILE" << 'EOF'
## 说明

- 压测工具使用阻塞 socket + 多线程模型
- 每个线程维持一个 keep-alive TCP 连接
- 测试路由: GET /api/hello（返回简单 JSON）
- QPS 会因测试环境（CPU、内存、网络）不同而变化

## 架构优势

| 优化手段 | 说明 |
|---------|------|
| Reactor 模式 | epoll LT + 非阻塞 I/O |
| One Loop Per Thread | 无锁设计，每线程独立 EventLoop |
| readv | 一次系统调用读尽数据 |
| 双缓冲异步日志 | 日志不阻塞业务 |
| swap 优化 | doPendingFunctors 最小化锁持有时间 |
EOF

echo ""
echo -e "${GREEN}========================================"
echo "  压测完成！报告已生成: $REPORT_FILE"
echo -e "========================================${NC}"
echo ""
cat "$REPORT_FILE"
