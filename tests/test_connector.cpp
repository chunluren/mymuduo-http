// test_connector.cpp - Connector behavior tests
//
// Connector is used internally by TcpClient. We exercise it through
// TcpClient to verify:
//   - Successful connect fires the connection callback
//   - No server at the target port => connection callback never fires
//     (with retry disabled)

#include <cassert>
#include <iostream>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "net/TcpServer.h"
#include "net/TcpClient.h"
#include "net/InetAddress.h"
#include "net/Buffer.h"

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    test_##name(); \
    std::cout << "PASSED" << std::endl; \
} while(0)

static const uint16_t kBasePort = 18990;

// Test 1: Connector successfully connects to a live server
TEST(connect_success)
{
    const uint16_t port = kBasePort;

    EventLoopThread serverThread;
    EventLoop* serverLoop = serverThread.startLoop();

    std::unique_ptr<TcpServer> server;
    serverLoop->runInLoop([&]() {
        InetAddress addr(port);
        server = std::make_unique<TcpServer>(serverLoop, addr, "ConnSrv", TcpServer::kReusePort);
        server->setThreadNum(0);
        server->setConnectionCallback([](const TcpConnectionPtr&) { /* no-op */ });
        server->setMessageCallback([](const TcpConnectionPtr&, Buffer* buf, Timestamp) { buf->retrieveAll(); });
        server->start();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    std::promise<void> connectedPromise;
    auto connectedFuture = connectedPromise.get_future();
    std::atomic<bool> fulfilled{false};

    InetAddress serverAddr(port, "127.0.0.1");
    auto client = std::make_shared<TcpClient>(clientLoop, serverAddr, "ConnCli");
    client->setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected() && !fulfilled.exchange(true)) {
            connectedPromise.set_value();
        }
    });
    clientLoop->runInLoop([&]() { client->connect(); });

    auto status = connectedFuture.wait_for(std::chrono::seconds(3));
    assert(status == std::future_status::ready);

    clientLoop->runInLoop([&]() { client->disconnect(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
}

// Test 2: Connection refused - no server listening, retry disabled.
// The connection callback should never fire within our observation window.
TEST(connection_refused_no_retry)
{
    const uint16_t port = kBasePort + 99; // intentionally no server

    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    std::atomic<bool> connected{false};
    InetAddress serverAddr(port, "127.0.0.1");
    auto client = std::make_shared<TcpClient>(clientLoop, serverAddr, "NoSrvCli");
    // Do NOT enableRetry() - we want a single failed attempt.
    client->setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            connected.store(true);
        }
    });
    clientLoop->runInLoop([&]() { client->connect(); });

    // Loopback connect-refused resolves almost immediately. Wait long
    // enough that any spurious callback would have fired.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    assert(!connected.load());

    clientLoop->runInLoop([&]() { client->disconnect(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Test 3: TcpClient::stop() before any successful connection - no callback fires
TEST(stop_before_connect)
{
    const uint16_t port = kBasePort + 50; // no server

    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    std::atomic<int> cbCount{0};
    InetAddress serverAddr(port, "127.0.0.1");
    auto client = std::make_shared<TcpClient>(clientLoop, serverAddr, "StopCli");
    client->setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            cbCount.fetch_add(1);
        }
    });

    clientLoop->runInLoop([&]() { client->connect(); });
    // Immediately request stop - Connector should be torn down cleanly
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    clientLoop->runInLoop([&]() { client->stop(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(cbCount.load() == 0);
}

int main()
{
    std::cout << "=== Connector Unit Tests ===" << std::endl;

    RUN_TEST(connect_success);
    RUN_TEST(connection_refused_no_retry);
    RUN_TEST(stop_before_connect);

    std::cout << std::endl;
    std::cout << "All Connector tests passed!" << std::endl;
    return 0;
}
