/**
 * @file bench_ws_encode.cpp
 * @brief Micro-benchmark for WebSocketFrameCodec::encodeText
 *
 * Measures encode throughput at several payload sizes. Run before/after
 * the encode() rewrite to show the per-byte push_back removal effect.
 */
#include "WebSocketFrame.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

static double bench(size_t payloadBytes, size_t iters) {
    std::string payload(payloadBytes, 'x');
    auto t0 = std::chrono::steady_clock::now();
    size_t totalBytes = 0;
    for (size_t i = 0; i < iters; ++i) {
        auto frame = WebSocketFrameCodec::encodeText(payload);
        totalBytes += frame.size();
    }
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    double mbPerSec = (totalBytes / 1e6) / sec;
    double opsPerSec = iters / sec;
    fprintf(stdout, "  payload=%-8zu iters=%-9zu  %8.0f ops/s   %7.0f MB/s   total=%.2fs\n",
            payloadBytes, iters, opsPerSec, mbPerSec, sec);
    return sec;
}

int main() {
    fprintf(stdout, "WebSocketFrameCodec::encodeText micro-benchmark\n");
    bench(64,        2'000'000);
    bench(1024,      1'000'000);
    bench(4096,        500'000);
    bench(64 * 1024,    50'000);
    bench(1024 * 1024,   2'000);
    return 0;
}
