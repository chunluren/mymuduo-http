#!/bin/bash
# 运行性能压测的脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  mymuduo-http 性能压测${NC}"
echo -e "${GREEN}========================================${NC}"

# 检查是否已编译
if [ ! -f "$BUILD_DIR/http_server" ] || [ ! -f "$BUILD_DIR/rpc_server" ]; then
    echo -e "${YELLOW}项目未编译，正在编译...${NC}"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake ..
    make -j$(nproc)
fi

cd "$BUILD_DIR"

# 默认参数
HTTP_PORT=8080
RPC_PORT=8081
THREADS=4
REQUESTS=1000
CONCURRENCY=50

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --threads)
            THREADS="$2"
            shift 2
            ;;
        --requests)
            REQUESTS="$2"
            shift 2
            ;;
        --concurrency)
            CONCURRENCY="$2"
            shift 2
            ;;
        --http-port)
            HTTP_PORT="$2"
            shift 2
            ;;
        --rpc-port)
            RPC_PORT="$2"
            shift 2
            ;;
        *)
            echo "未知参数: $1"
            exit 1
            ;;
    esac
done

# 清理函数
cleanup() {
    echo -e "\n${YELLOW}清理进程...${NC}"
    kill $HTTP_PID 2>/dev/null || true
    kill $RPC_PID 2>/dev/null || true
    wait $HTTP_PID 2>/dev/null || true
    wait $RPC_PID 2>/dev/null || true
}

trap cleanup EXIT

# 启动 HTTP 服务器
echo -e "${GREEN}启动 HTTP 服务器 (端口: $HTTP_PORT)...${NC}"
./http_server &
HTTP_PID=$!
sleep 1

# 启动 RPC 服务器
echo -e "${GREEN}启动 RPC 服务器 (端口: $RPC_PORT)...${NC}"
./rpc_server &
RPC_PID=$!
sleep 1

echo ""

# HTTP 压测
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  HTTP 压测${NC}"
echo -e "${GREEN}========================================${NC}"
./benchmark_client --type http --port $HTTP_PORT --threads $THREADS --requests $REQUESTS

echo ""

# RPC 压测
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  JSON-RPC 压测${NC}"
echo -e "${GREEN}========================================${NC}"
./benchmark_client --type rpc --port $RPC_PORT --threads $THREADS --requests $REQUESTS

echo ""
echo -e "${GREEN}压测完成!${NC}"