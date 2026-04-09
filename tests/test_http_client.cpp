// test_http_client.cpp - Integration test for HttpClient + HttpServer
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>

#include "http/HttpClient.h"
#include "http/HttpServer.h"
#include "net/EventLoopThread.h"
#include "net/InetAddress.h"

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    test_##name(); \
    std::cout << "PASSED" << std::endl; \
} while(0)

// ---------------------------------------------------------------------------
//  Shared server infrastructure
// ---------------------------------------------------------------------------

static const uint16_t kTestPort = 19870;

// Starts an HttpServer on an EventLoopThread and returns the loop pointer.
// The server pointer is assigned via the out-parameter so the caller can
// keep it alive for the duration of the tests.
static EventLoop* startTestServer(EventLoopThread& serverThread,
                                  std::unique_ptr<HttpServer>& serverOut) {
    EventLoop* serverLoop = serverThread.startLoop();

    serverLoop->runInLoop([&serverOut, serverLoop]() {
        InetAddress listenAddr(kTestPort);
        serverOut.reset(new HttpServer(serverLoop, listenAddr, "TestHttpServer"));
        serverOut->setThreadNum(0);  // single-threaded for simplicity

        // Route: GET /hello  ->  {"msg":"hello"}
        serverOut->GET("/hello", [](const HttpRequest& /*req*/, HttpResponse& resp) {
            resp.setStatusCode(HttpStatusCode::OK);
            resp.setJson(R"({"msg":"hello"})");
        });

        // Route: POST /echo  ->  echo the request body back
        serverOut->POST("/echo", [](const HttpRequest& req, HttpResponse& resp) {
            resp.setStatusCode(HttpStatusCode::OK);
            resp.setJson(req.body);
        });

        // Route: GET /status  ->  200 with status info
        serverOut->GET("/status", [](const HttpRequest& /*req*/, HttpResponse& resp) {
            resp.setStatusCode(HttpStatusCode::OK);
            resp.setJson(R"({"status":"ok","uptime":42})");
        });

        serverOut->start();
    });

    // Give the server a moment to bind and start listening
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return serverLoop;
}

// ---------------------------------------------------------------------------
//  Test cases
// ---------------------------------------------------------------------------

TEST(get_request) {
    // Start server
    EventLoopThread serverThread;
    std::unique_ptr<HttpServer> server;
    startTestServer(serverThread, server);

    // Start client
    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    InetAddress serverAddr(kTestPort, "127.0.0.1");
    HttpClient client(clientLoop, serverAddr, "TestClient-GET");
    client.connect();

    // Allow connection to establish
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    HttpClientResponse resp = client.GET("/hello", 3000);
    assert(resp.success);
    assert(resp.statusCode == 200);
    assert(resp.body.find("hello") != std::string::npos);

    client.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(post_request) {
    EventLoopThread serverThread;
    std::unique_ptr<HttpServer> server;
    startTestServer(serverThread, server);

    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    InetAddress serverAddr(kTestPort, "127.0.0.1");
    HttpClient client(clientLoop, serverAddr, "TestClient-POST");
    client.connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::string jsonBody = R"({"name":"Alice","age":30})";
    HttpClientResponse resp = client.POST("/echo", jsonBody, "application/json", 3000);
    assert(resp.success);
    assert(resp.statusCode == 200);
    assert(resp.body.find("Alice") != std::string::npos);
    assert(resp.body.find("30") != std::string::npos);

    client.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(not_found) {
    EventLoopThread serverThread;
    std::unique_ptr<HttpServer> server;
    startTestServer(serverThread, server);

    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    InetAddress serverAddr(kTestPort, "127.0.0.1");
    HttpClient client(clientLoop, serverAddr, "TestClient-404");
    client.connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    HttpClientResponse resp = client.GET("/nonexistent", 3000);
    assert(resp.success);
    assert(resp.statusCode == 404);

    client.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(multiple_requests) {
    EventLoopThread serverThread;
    std::unique_ptr<HttpServer> server;
    startTestServer(serverThread, server);

    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    InetAddress serverAddr(kTestPort, "127.0.0.1");
    HttpClient client(clientLoop, serverAddr, "TestClient-Multi");
    client.connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Send 3 GET requests on the same keep-alive connection
    for (int i = 0; i < 3; ++i) {
        HttpClientResponse resp = client.GET("/status", 3000);
        assert(resp.success);
        assert(resp.statusCode == 200);
        assert(resp.body.find("ok") != std::string::npos);
        // Small delay between requests to be safe
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    client.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------

int main() {
    std::cout << "========== HttpClient Integration Tests ==========" << std::endl;

    RUN_TEST(get_request);
    RUN_TEST(post_request);
    RUN_TEST(not_found);
    RUN_TEST(multiple_requests);

    std::cout << "========== All HttpClient tests PASSED ==========" << std::endl;
    return 0;
}
