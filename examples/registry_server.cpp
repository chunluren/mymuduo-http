// registry_server.cpp - 服务注册中心服务器示例
#include <iostream>
#include <signal.h>
#include "EventLoop.h"
#include "InetAddress.h"
#include "src/registry/RegistryServer.h"

using namespace std;

EventLoop* g_loop = nullptr;

void signalHandler(int) {
    if (g_loop) {
        g_loop->quit();
    }
}

int main() {
    cout << "=== Service Registry Server ===" << endl;

    EventLoop loop;
    g_loop = &loop;

    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    InetAddress addr(8500);  // 默认端口 8500
    RegistryServer server(&loop, addr, "RegistryServer");

    server.setThreadNum(4);
    server.start();

    cout << "Registry Server started on port 8500" << endl;
    cout << "API Endpoints:" << endl;
    cout << "  POST /api/v1/registry/register   - Register service instance" << endl;
    cout << "  POST /api/v1/registry/deregister - Deregister service instance" << endl;
    cout << "  POST /api/v1/registry/heartbeat  - Send heartbeat" << endl;
    cout << "  GET  /api/v1/registry/discover   - Discover services" << endl;
    cout << "  GET  /api/v1/registry/services   - List all services" << endl;
    cout << "  GET  /api/v1/registry/health     - Health check" << endl;
    cout << "  GET  /api/v1/registry/stats      - Statistics" << endl;

    loop.loop();

    cout << "Registry Server stopped" << endl;
    return 0;
}