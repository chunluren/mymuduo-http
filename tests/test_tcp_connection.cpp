// test_tcp_connection.cpp - TcpConnection behavior tests
//
// TcpConnection is driven by TcpServer/TcpClient in practice, so we exercise
// it end-to-end: a server runs on one EventLoop thread, a client runs on
// another, and we observe the TcpConnectionPtr callbacks (connect, message,
// close).

#include <cassert>
#include <iostream>
#include <atomic>
#include <chrono>
#include <future>
#include <string>
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

static const uint16_t kBasePort = 18890;

// Test 1: Echo round-trip exercises TcpConnection::send() + handleRead()
TEST(basic_echo)
{
    const uint16_t port = kBasePort;

    // Server loop
    EventLoopThread serverThread;
    EventLoop* serverLoop = serverThread.startLoop();

    std::unique_ptr<TcpServer> server;
    serverLoop->runInLoop([&]() {
        InetAddress addr(port);
        server = std::make_unique<TcpServer>(serverLoop, addr, "EchoServer", TcpServer::kReusePort);
        server->setThreadNum(0);
        server->setConnectionCallback([](const TcpConnectionPtr&) { /* no-op */ });
        server->setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
            // Echo back whatever we received
            conn->send(buf->retrieveAllAsString());
        });
        server->start();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Client loop
    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    std::promise<std::string> replyPromise;
    auto replyFuture = replyPromise.get_future();
    std::atomic<bool> fulfilled{false};

    InetAddress serverAddr(port, "127.0.0.1");
    auto client = std::make_shared<TcpClient>(clientLoop, serverAddr, "EchoClient");

    client->setConnectionCallback([](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            // Exercise TcpConnection::send(const std::string&)
            conn->send(std::string("hello"));
        }
    });
    client->setMessageCallback([&](const TcpConnectionPtr&, Buffer* buf, Timestamp) {
        if (!fulfilled.exchange(true)) {
            replyPromise.set_value(buf->retrieveAllAsString());
        }
    });

    clientLoop->runInLoop([&]() { client->connect(); });

    auto status = replyFuture.wait_for(std::chrono::seconds(3));
    assert(status == std::future_status::ready);
    assert(replyFuture.get() == "hello");

    clientLoop->runInLoop([&]() { client->disconnect(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
}

// Test 2: Connection lifecycle - server observes connect + disconnect
TEST(connection_lifecycle)
{
    const uint16_t port = kBasePort + 1;

    EventLoopThread serverThread;
    EventLoop* serverLoop = serverThread.startLoop();

    std::atomic<int> serverConnects{0};
    std::atomic<int> serverDisconnects{0};
    std::promise<void> disconnectPromise;
    auto disconnectFuture = disconnectPromise.get_future();
    std::atomic<bool> fulfilled{false};

    std::unique_ptr<TcpServer> server;
    serverLoop->runInLoop([&]() {
        InetAddress addr(port);
        server = std::make_unique<TcpServer>(serverLoop, addr, "LifecycleServer", TcpServer::kReusePort);
        server->setThreadNum(0);
        server->setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (conn->connected()) {
                ++serverConnects;
                // Sanity: TcpConnection exposes its addresses + name
                assert(!conn->name().empty());
                assert(conn->peerAddress().toIpPort().find("127.0.0.1") != std::string::npos);
            } else {
                ++serverDisconnects;
                if (!fulfilled.exchange(true)) {
                    disconnectPromise.set_value();
                }
            }
        });
        server->setMessageCallback([](const TcpConnectionPtr&, Buffer* buf, Timestamp) {
            buf->retrieveAll();
        });
        server->start();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    InetAddress serverAddr(port, "127.0.0.1");
    auto client = std::make_shared<TcpClient>(clientLoop, serverAddr, "LifecycleClient");

    client->setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            // Disconnect immediately from the loop thread
            clientLoop->runInLoop([&client]() { client->disconnect(); });
        }
    });
    clientLoop->runInLoop([&]() { client->connect(); });

    // Wait for the server to observe the disconnect
    auto status = disconnectFuture.wait_for(std::chrono::seconds(3));
    assert(status == std::future_status::ready);
    assert(serverConnects.load() >= 1);
    assert(serverDisconnects.load() >= 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Test 3: shutdown() - server half-closes; client sees disconnect
TEST(server_shutdown)
{
    const uint16_t port = kBasePort + 2;

    EventLoopThread serverThread;
    EventLoop* serverLoop = serverThread.startLoop();

    std::unique_ptr<TcpServer> server;
    serverLoop->runInLoop([&]() {
        InetAddress addr(port);
        server = std::make_unique<TcpServer>(serverLoop, addr, "ShutdownServer", TcpServer::kReusePort);
        server->setThreadNum(0);
        server->setConnectionCallback([](const TcpConnectionPtr&) { /* no-op */ });
        server->setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
            std::string msg = buf->retrieveAllAsString();
            // Echo then shutdown the write side
            conn->send(msg);
            conn->shutdown();
        });
        server->start();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    std::promise<void> disconnectPromise;
    auto disconnectFuture = disconnectPromise.get_future();
    std::atomic<bool> fulfilled{false};

    InetAddress serverAddr(port, "127.0.0.1");
    auto client = std::make_shared<TcpClient>(clientLoop, serverAddr, "ShutdownClient");

    client->setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            conn->send("ping");
        } else {
            if (!fulfilled.exchange(true)) {
                disconnectPromise.set_value();
            }
        }
    });
    client->setMessageCallback([](const TcpConnectionPtr&, Buffer* buf, Timestamp) {
        buf->retrieveAll();
    });
    clientLoop->runInLoop([&]() { client->connect(); });

    auto status = disconnectFuture.wait_for(std::chrono::seconds(3));
    assert(status == std::future_status::ready);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

int main()
{
    std::cout << "=== TcpConnection Unit Tests ===" << std::endl;

    RUN_TEST(basic_echo);
    RUN_TEST(connection_lifecycle);
    RUN_TEST(server_shutdown);

    std::cout << std::endl;
    std::cout << "All TcpConnection tests passed!" << std::endl;
    return 0;
}
