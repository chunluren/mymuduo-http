// 示例 HTTP 服务器
#include "HttpServer.h"
#include <iostream>
#include <signal.h>

HttpServer* g_server = nullptr;

void signalHandler(int) {
    if (g_server) {
        std::cout << "\nShutting down..." << std::endl;
        exit(0);
    }
}

int main() {
    signal(SIGINT, signalHandler);
    
    EventLoop loop;
    InetAddress addr(8080);
    HttpServer server(&loop, addr, "MyHttpServer");
    g_server = &server;
    
    // 注册路由
    // GET /
    server.GET("/", [](const HttpRequest& req, HttpResponse& resp) {
        resp.setHtml(R"(
<!DOCTYPE html>
<html>
<head><title>mymuduo HTTP Server</title></head>
<body>
    <h1>Welcome to mymuduo HTTP Server</h1>
    <p>This is a simple HTTP server built on mymuduo network library.</p>
    <ul>
        <li><a href="/api/hello">/api/hello</a> - Hello API</li>
        <li><a href="/api/time">/api/time</a> - Current Time</li>
        <li><a href="/api/echo?msg=test">/api/echo</a> - Echo API</li>
    </ul>
</body>
</html>
        )");
    });
    
    // GET /api/hello
    server.GET("/api/hello", [](const HttpRequest& req, HttpResponse& resp) {
        resp.setJson(R"({"message": "Hello, World!", "status": "ok"})");
    });
    
    // GET /api/time
    server.GET("/api/time", [](const HttpRequest& req, HttpResponse& resp) {
        time_t now = time(nullptr);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
        
        resp.setJson("{\"time\": \"" + std::string(buf) + "\"}");
    });
    
    // GET /api/echo
    server.GET("/api/echo", [](const HttpRequest& req, HttpResponse& resp) {
        std::string msg = req.params.count("msg") ? req.params.at("msg") : "empty";
        resp.setJson("{\"echo\": \"" + msg + "\"}");
    });
    
    // POST /api/json
    server.POST("/api/json", [](const HttpRequest& req, HttpResponse& resp) {
        // Echo back the posted JSON
        resp.setJson(req.body);
    });

    // GET /api/large — 64KB body, used to measure the writev (header+body) split
    {
        std::string bigBody(64 * 1024, 'x');
        server.GET("/api/large", [bigBody](const HttpRequest& /*req*/, HttpResponse& resp) {
            resp.setBody(bigBody);
            resp.setHeader("Content-Type", "text/plain");
        });
    }
    
    // 静态文件
    server.serveStatic("/static", "./static");
    // 用于 sendfile zero-copy 测试
    server.serveStatic("/tmp", "/tmp/static-test");
    
    std::cout << "HTTP Server running on http://localhost:8080" << std::endl;
    
    server.start();
    loop.loop();
    
    return 0;
}