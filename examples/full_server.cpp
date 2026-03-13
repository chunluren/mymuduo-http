// 完整示例：使用所有模块的 HTTP 服务器
#include "HttpServer.h"
#include "timer/TimerQueue.h"
#include "asynclogger/AsyncLogger.h"
#include "pool/ConnectionPool.h"
#include "util/SignalHandler.h"
#include "config/Config.h"
#include <iostream>
#include <signal.h>

EventLoop* g_loop = nullptr;

int main(int argc, char* argv[]) {
    // 1. 加载配置
    std::string configFile = "config/server.conf";
    if (argc > 1) configFile = argv[1];
    
    if (!Config::instance().load(configFile)) {
        std::cerr << "Warning: config file not found, using defaults" << std::endl;
    }
    
    // 2. 启动异步日志
    AsyncLogger::instance().setLogFile(CONFIG_STRING("log.file"));
    AsyncLogger::instance().setLogLevel(LogLevel::INFO);
    AsyncLogger::instance().start();
    
    LOG_INFO("Server starting...");
    
    // 3. 忽略 SIGPIPE
    Signals::ignorePipe();
    
    // 4. 创建定时器队列
    TimerQueue timerQueue;
    
    // 5. 创建 HTTP 服务器
    int port = CONFIG_INT("server.port");
    int threads = CONFIG_INT("server.threads");
    
    EventLoop loop;
    InetAddress addr(port);
    HttpServer server(&loop, addr, "MyHttpServer");
    server.setThreadNum(threads);
    
    // 6. 注册路由
    server.GET("/", [](const HttpRequest& req, HttpResponse& resp) {
        resp.setHtml(R"(
<!DOCTYPE html>
<html>
<head><title>mymuduo-http</title></head>
<body>
    <h1>Welcome to mymuduo-http</h1>
    <h2>Features</h2>
    <ul>
        <li>HTTP/1.1 with Keep-Alive</li>
        <li>JSON-RPC 2.0</li>
        <li>Async Logging</li>
        <li>Timer Support</li>
        <li>Connection Pool</li>
    </ul>
    <h2>API</h2>
    <ul>
        <li><a href="/api/hello">/api/hello</a></li>
        <li><a href="/api/time">/api/time</a></li>
        <li><a href="/api/status">/api/status</a></li>
    </ul>
</body>
</html>
        )");
    });
    
    server.GET("/api/hello", [](const HttpRequest& req, HttpResponse& resp) {
        resp.json(R"({"message": "Hello, World!"})");
    });
    
    server.GET("/api/time", [](const HttpRequest& req, HttpResponse& resp) {
        time_t now = time(nullptr);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
        resp.json("{\"time\": \"" + std::string(buf) + "\"}");
    });
    
    server.GET("/api/status", [](const HttpRequest& req, HttpResponse& resp) {
        resp.json(R"({
            "status": "ok",
            "version": "1.0.0",
            "features": ["http", "rpc", "async-log", "timer", "pool"]
        })");
    });
    
    // 7. 设置优雅退出
    g_loop = &loop;
    Signals::gracefulExit([]() {
        LOG_INFO("Shutting down gracefully...");
        if (g_loop) {
            g_loop->quit();
        }
        AsyncLogger::instance().stop();
    });
    
    // 8. 添加定时任务（每分钟打印状态）
    int port_for_timer = port;
    timerQueue.addTimer([port_for_timer]() {
        LOG_INFO("Server running, port: %d", port_for_timer);
    }, 60000, 60000);  // 60秒后开始，每60秒
    
    // 9. 启动服务器
    LOG_INFO("HTTP Server running on port %d with %d threads", port, threads);
    server.start();
    loop.loop();
    
    LOG_INFO("Server stopped");
    return 0;
}