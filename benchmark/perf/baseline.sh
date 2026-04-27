#!/usr/bin/env bash
# Phase 0 baseline harness for mymuduo-http perf optimization plan.
#
# Runs a fixed set of workloads against examples/http_server (which is
# already built into build/http_server) and writes a markdown report.
#
# Usage:
#   benchmark/perf/baseline.sh [tag]
#
#   tag: optional label appended to the output filename (e.g. "tcp-nodelay-on")
#        defaults to current git short HEAD.
#
# Requirements: wrk, ab, /usr/bin/time, curl
set -uo pipefail

cd "$(dirname "$0")/../.."
ROOT=$(pwd)
BUILD=${BUILD:-build}
SERVER_BIN="$BUILD/http_server"
PORT=${PORT:-8080}
URL="http://127.0.0.1:$PORT/api/hello"
TAG=${1:-$(git rev-parse --short HEAD 2>/dev/null || echo unknown)}
OUT="$ROOT/benchmark/perf/baseline-$(date +%Y%m%d-%H%M%S)-$TAG.md"

# Sanity
[[ -x $SERVER_BIN ]] || { echo "missing $SERVER_BIN; run: cd build && make http_server"; exit 1; }
command -v wrk >/dev/null || { echo "wrk not installed"; exit 1; }

cleanup() {
    [[ -n ${SERVER_PID:-} ]] && kill "$SERVER_PID" 2>/dev/null || true
    sleep 0.3
}
trap cleanup EXIT INT TERM

# Make sure no stale instance
fuser -k "$PORT"/tcp 2>/dev/null || true
sleep 0.3

# Start server (foreground binary; we'll capture its CPU/RSS after the runs)
"$SERVER_BIN" >/dev/null 2>&1 &
SERVER_PID=$!
sleep 0.5

# Wait for ready
for i in {1..20}; do
    curl -sf "$URL" >/dev/null && break || sleep 0.1
done

# Probe the response once for the report (gives us the body size context)
RESP_BYTES=$(curl -sI "$URL" | awk -F': ' '/Content-Length/ {gsub(/\r/,"",$2); print $2}')

run_section() {
    local title=$1; shift
    echo "## $title"
    echo
    echo '```'
    "$@" 2>&1
    echo '```'
    echo
}

# 1) wrk keep-alive: 4 threads × 200 conns × 30s
WRK_KEEP=$(wrk -t4 -c200 -d30s --latency "$URL" 2>&1)

# 2) wrk single-conn (this is where Nagle hurts most)
WRK_SINGLE=$(wrk -t1 -c1 -d10s --latency "$URL" 2>&1)

# 3) wrk short-conn (Connection: close header forces close per request — stresses accept/close path)
WRK_SHORT=$(wrk -t4 -c100 -d20s --latency -H 'Connection: close' "$URL" 2>&1)

# 4) wrk pipelined keep-alive (Lua script for back-to-back requests on a single conn)
PIPE_LUA=$(mktemp)
cat > "$PIPE_LUA" <<'LUA'
wrk.method = "GET"
init = function(args) end
request = function() return wrk.format("GET", "/api/hello") end
LUA
WRK_PIPE=$(wrk -t1 -c1 -d10s --latency -s "$PIPE_LUA" "$URL" 2>&1)
rm -f "$PIPE_LUA"

# 5) wrk large body keep-alive: 64KB body — sensitive to header/body memcpy in toString()
LARGE_URL="http://127.0.0.1:$PORT/api/large"
WRK_LARGE=$(wrk -t4 -c100 -d20s --latency "$LARGE_URL" 2>&1)

# 5) Resource snapshot of the server after the runs
RSS_KB=$(awk '/^VmRSS/ {print $2}' /proc/$SERVER_PID/status 2>/dev/null || echo "?")
THREADS=$(awk '/^Threads/ {print $2}' /proc/$SERVER_PID/status 2>/dev/null || echo "?")

# Write the report
{
  echo "# mymuduo-http perf baseline"
  echo
  echo "- **Tag**: \`$TAG\`"
  echo "- **Time**: $(date -Iseconds)"
  echo "- **Host**: $(uname -srm) | CPU $(nproc) cores | $(awk '/MemTotal/ {print int($2/1024)" MB RAM"}' /proc/meminfo)"
  echo "- **Server**: \`$SERVER_BIN\` PID $SERVER_PID"
  echo "- **Endpoint**: \`$URL\`  (response Content-Length=$RESP_BYTES bytes)"
  echo "- **Server RSS after run**: ${RSS_KB} kB; threads=${THREADS}"
  echo
  echo "## Workloads"
  echo
  echo "| # | Tool | Args | What it stresses |"
  echo "|---|------|------|------------------|"
  echo "| 1 | wrk  | -t4 -c200 -d30s | HTTP keep-alive throughput |"
  echo "| 2 | wrk  | -t1 -c1 -d10s   | Single-conn latency (Nagle visible here) |"
  echo "| 3 | wrk  | -t4 -c100 -d20s -H 'Connection: close' | Short-conn accept+close path |"
  echo "| 4 | wrk  | -t1 -c1 -d10s   | Single-conn (same as #2) for serial baseline |"
  echo "| 5 | wrk  | -t4 -c100 -d20s /api/large | 64 KB body — writev header/body split |"
  echo
  echo "---"
  echo
  echo "## 1) wrk keep-alive (4×200, 30s)"
  echo
  echo '```'
  echo "$WRK_KEEP"
  echo '```'
  echo
  echo "## 2) wrk single-conn (1×1, 10s)"
  echo
  echo '```'
  echo "$WRK_SINGLE"
  echo '```'
  echo
  echo "## 3) wrk short-conn (4×100, 20s, Connection: close)"
  echo
  echo '```'
  echo "$WRK_SHORT"
  echo '```'
  echo
  echo "## 4) wrk single-conn pipelined (same single-conn target, Lua harness)"
  echo
  echo '```'
  echo "$WRK_PIPE"
  echo '```'
  echo
  echo "## 5) wrk 64KB body keep-alive (4×100, 20s, /api/large)"
  echo
  echo '```'
  echo "$WRK_LARGE"
  echo '```'
  echo
  echo "## Notes"
  echo
  echo "- Workload 2 is the canonical 'is Nagle hurting us?' probe. Without"
  echo "  TCP_NODELAY, single-conn back-to-back small requests will show"
  echo "  visible latency from the 40 ms ACK delay timer."
  echo "- All numbers single-host loopback; absolute throughput is bounded by"
  echo "  loopback bandwidth and shared CPU. Use these only for **before/after**"
  echo "  comparisons against the same machine."
} > "$OUT"

echo "wrote $OUT"
