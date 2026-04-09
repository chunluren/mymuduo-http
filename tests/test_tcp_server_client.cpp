// test_tcp_server_client.cpp - TcpServer + TcpClient integration test
//
// Tests the full client-server Reactor pipeline: connection, echo,
// multiple messages, and disconnect/reconnect.

#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <thread>
#include <future>
#include <chrono>
#include <atomic>
#include <mutex>

#include "net/TcpServer.h"
#include "net/TcpClient.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "net/InetAddress.h"
#include "net/Buffer.h"
#include "net/Callbacks.h"

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    test_##name(); \
    std::cout << "PASSED" << std::endl; \
} while(0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const uint16_t kBasePort = 19876;

// Build a simple echo server on the given port. Returns after the server
// is listening and ready to accept connections.
struct EchoServer {
    EventLoop* loop;
    std::unique_ptr<TcpServer> server;

    // Start the echo server on `port` using the provided EventLoop.
    // The server echoes every message it receives back to the sender.
    void start(EventLoop* baseLoop, uint16_t port) {
        loop = baseLoop;
        InetAddress addr(port);
        server = std::make_unique<TcpServer>(loop, addr, "EchoServer", TcpServer::kReusePort);
        server->setThreadNum(0); // all I/O in base loop

        server->setConnectionCallback([](const TcpConnectionPtr& conn) {
            if (conn->connected()) {
                // no-op on connect
            }
        });

        server->setMessageCallback([](const TcpConnectionPtr& conn,
                                      Buffer* buf,
                                      Timestamp) {
            std::string msg = buf->retrieveAllAsString();
            conn->send(msg); // echo back
        });

        server->start();
    }
};

// ---------------------------------------------------------------------------
// Test 1: Echo – client sends "hello", server echoes back
// ---------------------------------------------------------------------------
TEST(echo) {
    const uint16_t port = kBasePort;

    // -- Server side: run EventLoop in a dedicated thread --
    EventLoopThread serverThread;
    EventLoop* serverLoop = serverThread.startLoop();

    EchoServer echoSrv;
    serverLoop->runInLoop([&]() { echoSrv.start(serverLoop, port); });

    // Give the server a moment to bind and listen
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // -- Client side --
    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    std::promise<std::string> replyPromise;
    auto replyFuture = replyPromise.get_future();
    bool promiseFulfilled = false;

    InetAddress serverAddr(port, "127.0.0.1");
    auto client = std::make_shared<TcpClient>(clientLoop, serverAddr, "TestClient");

    client->setConnectionCallback([](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            conn->send("hello");
        }
    });

    client->setMessageCallback([&](const TcpConnectionPtr&,
                                   Buffer* buf,
                                   Timestamp) {
        std::string msg = buf->retrieveAllAsString();
        if (!promiseFulfilled) {
            promiseFulfilled = true;
            replyPromise.set_value(msg);
        }
    });

    clientLoop->runInLoop([&]() { client->connect(); });

    // Wait for the echo reply (timeout 3 seconds)
    auto status = replyFuture.wait_for(std::chrono::seconds(3));
    assert(status == std::future_status::ready);
    assert(replyFuture.get() == "hello");

    // Cleanup
    clientLoop->runInLoop([&]() { client->disconnect(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ---------------------------------------------------------------------------
// Test 2: Multiple messages – client sends several strings, all echoed
// ---------------------------------------------------------------------------
TEST(multiple_messages) {
    const uint16_t port = kBasePort + 1;

    EventLoopThread serverThread;
    EventLoop* serverLoop = serverThread.startLoop();

    EchoServer echoSrv;
    serverLoop->runInLoop([&]() { echoSrv.start(serverLoop, port); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    const std::vector<std::string> messages = {"msg1", "msg2", "msg3", "msg4", "msg5"};
    std::mutex mtx;
    std::string accumulated;
    std::promise<void> donePromise;
    auto doneFuture = donePromise.get_future();
    bool promiseFulfilled = false;

    // Total bytes we expect to receive back
    size_t totalBytes = 0;
    for (auto& m : messages) totalBytes += m.size();

    InetAddress serverAddr(port, "127.0.0.1");
    auto client = std::make_shared<TcpClient>(clientLoop, serverAddr, "MultiMsgClient");

    client->setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            for (auto& m : messages) {
                conn->send(m);
            }
        }
    });

    client->setMessageCallback([&](const TcpConnectionPtr&,
                                   Buffer* buf,
                                   Timestamp) {
        std::string data = buf->retrieveAllAsString();
        std::lock_guard<std::mutex> lk(mtx);
        accumulated += data;
        if (accumulated.size() >= totalBytes && !promiseFulfilled) {
            promiseFulfilled = true;
            donePromise.set_value();
        }
    });

    clientLoop->runInLoop([&]() { client->connect(); });

    auto status = doneFuture.wait_for(std::chrono::seconds(3));
    assert(status == std::future_status::ready);

    // The concatenation of all echoed data must equal the concatenation of
    // all sent messages (TCP is a stream, so boundaries may differ).
    std::string expected;
    for (auto& m : messages) expected += m;
    assert(accumulated == expected);

    clientLoop->runInLoop([&]() { client->disconnect(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ---------------------------------------------------------------------------
// Test 3: Client disconnect and reconnect
// ---------------------------------------------------------------------------
TEST(disconnect_reconnect) {
    const uint16_t port = kBasePort + 2;

    EventLoopThread serverThread;
    EventLoop* serverLoop = serverThread.startLoop();

    EchoServer echoSrv;
    serverLoop->runInLoop([&]() { echoSrv.start(serverLoop, port); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    InetAddress serverAddr(port, "127.0.0.1");
    auto client = std::make_shared<TcpClient>(clientLoop, serverAddr, "ReconnClient");

    // -- First connection: send "first", get echo --
    std::promise<std::string> firstPromise;
    auto firstFuture = firstPromise.get_future();
    bool firstFulfilled = false;
    std::atomic<int> connectCount{0};

    client->enableRetry();

    client->setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            int c = ++connectCount;
            if (c == 1) {
                conn->send("first");
            } else if (c == 2) {
                conn->send("second");
            }
        }
    });

    std::promise<std::string> secondPromise;
    auto secondFuture = secondPromise.get_future();
    bool secondFulfilled = false;

    client->setMessageCallback([&](const TcpConnectionPtr&,
                                   Buffer* buf,
                                   Timestamp) {
        std::string msg = buf->retrieveAllAsString();
        int c = connectCount.load();
        if (c == 1 && !firstFulfilled) {
            firstFulfilled = true;
            firstPromise.set_value(msg);
        } else if (c == 2 && !secondFulfilled) {
            secondFulfilled = true;
            secondPromise.set_value(msg);
        }
    });

    clientLoop->runInLoop([&]() { client->connect(); });

    // Wait for first echo
    auto s1 = firstFuture.wait_for(std::chrono::seconds(3));
    assert(s1 == std::future_status::ready);
    assert(firstFuture.get() == "first");

    // Disconnect
    clientLoop->runInLoop([&]() { client->disconnect(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Reconnect (enableRetry is already set, so just call connect again)
    clientLoop->runInLoop([&]() { client->connect(); });

    // Wait for second echo
    auto s2 = secondFuture.wait_for(std::chrono::seconds(3));
    assert(s2 == std::future_status::ready);
    assert(secondFuture.get() == "second");

    clientLoop->runInLoop([&]() { client->disconnect(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ---------------------------------------------------------------------------
// Test 4: Server with multiple I/O threads
// ---------------------------------------------------------------------------
TEST(server_multithreaded) {
    const uint16_t port = kBasePort + 3;

    EventLoopThread serverThread;
    EventLoop* serverLoop = serverThread.startLoop();

    // Set up server with 2 I/O threads
    std::unique_ptr<TcpServer> server;
    serverLoop->runInLoop([&]() {
        InetAddress addr(port);
        server = std::make_unique<TcpServer>(serverLoop, addr, "MTServer", TcpServer::kReusePort);
        server->setThreadNum(2);

        server->setMessageCallback([](const TcpConnectionPtr& conn,
                                      Buffer* buf,
                                      Timestamp) {
            std::string msg = buf->retrieveAllAsString();
            conn->send(msg);
        });

        server->start();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Launch 3 clients, each sends a unique message
    const int kNumClients = 3;
    std::vector<std::promise<std::string>> promises(kNumClients);
    std::vector<std::future<std::string>> futures;
    std::vector<bool> fulfilled(kNumClients, false);
    for (int i = 0; i < kNumClients; ++i) {
        futures.push_back(promises[i].get_future());
    }

    std::vector<EventLoopThread> clientThreads(kNumClients);
    std::vector<std::shared_ptr<TcpClient>> clients(kNumClients);

    for (int i = 0; i < kNumClients; ++i) {
        EventLoop* cloop = clientThreads[i].startLoop();
        InetAddress serverAddr(port, "127.0.0.1");
        std::string name = "Client-" + std::to_string(i);
        clients[i] = std::make_shared<TcpClient>(cloop, serverAddr, name);

        std::string payload = "client" + std::to_string(i);

        clients[i]->setConnectionCallback([payload](const TcpConnectionPtr& conn) {
            if (conn->connected()) {
                conn->send(payload);
            }
        });

        clients[i]->setMessageCallback([&promises, &fulfilled, i](const TcpConnectionPtr&,
                                                                    Buffer* buf,
                                                                    Timestamp) {
            std::string msg = buf->retrieveAllAsString();
            if (!fulfilled[i]) {
                fulfilled[i] = true;
                promises[i].set_value(msg);
            }
        });

        cloop->runInLoop([&clients, i]() { clients[i]->connect(); });
    }

    // Verify all clients get their echo
    for (int i = 0; i < kNumClients; ++i) {
        auto status = futures[i].wait_for(std::chrono::seconds(3));
        assert(status == std::future_status::ready);
        std::string expected = "client" + std::to_string(i);
        assert(futures[i].get() == expected);
    }

    // Cleanup
    for (int i = 0; i < kNumClients; ++i) {
        EventLoop* cloop = clients[i]->getLoop();
        auto cli = clients[i];
        cloop->runInLoop([cli]() { cli->disconnect(); });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=== TcpServer + TcpClient Integration Tests ===" << std::endl;

    RUN_TEST(echo);
    RUN_TEST(multiple_messages);
    RUN_TEST(disconnect_reconnect);
    RUN_TEST(server_multithreaded);

    std::cout << std::endl << "All TcpServer/TcpClient tests passed!" << std::endl;
    return 0;
}
