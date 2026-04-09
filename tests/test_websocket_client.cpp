// test_websocket_client.cpp - WebSocketClient integration test with inline echo server
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <future>
#include <cstring>

#include "websocket/WebSocketClient.h"
#include "websocket/WebSocketFrame.h"
#include "net/TcpServer.h"
#include "net/EventLoopThread.h"
#include "net/InetAddress.h"

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    test_##name(); \
    std::cout << "PASSED" << std::endl; \
} while(0)

// ---------------------------------------------------------------------------
//  Minimal inline WebSocket echo server
//
//  Built directly on TcpServer. For each connection it:
//    1. Parses the HTTP Upgrade request from the buffer
//    2. Computes Sec-WebSocket-Accept via WebSocketFrameCodec::computeAcceptKey()
//    3. Sends the 101 Switching Protocols response
//    4. Decodes incoming WebSocket frames
//    5. Echoes text frames back (unmasked, server -> client)
//    6. Replies to Ping with Pong
// ---------------------------------------------------------------------------

static const uint16_t kWsTestPort = 19871;

class InlineWsEchoServer {
public:
    InlineWsEchoServer(EventLoop* loop, const InetAddress& addr)
        : server_(loop, addr, "InlineWsEchoServer")
    {
        server_.setThreadNum(0);

        server_.setConnectionCallback([this](const TcpConnectionPtr& conn) {
            if (conn->connected()) {
                // Mark the connection as not yet upgraded
                upgradedConns_[conn->name()] = false;
            } else {
                // Clean up state on disconnect
                upgradedConns_.erase(conn->name());
                frameBuffers_.erase(conn->name());
            }
        });

        server_.setMessageCallback(
            [this](const TcpConnectionPtr& conn, Buffer* buf, Timestamp /*time*/) {
                onMessage(conn, buf);
            });
    }

    void start() { server_.start(); }

private:
    TcpServer server_;

    // Per-connection handshake state (keyed by connection name)
    std::unordered_map<std::string, bool> upgradedConns_;
    // Per-connection leftover frame bytes (keyed by connection name)
    std::unordered_map<std::string, std::vector<uint8_t>> frameBuffers_;

    void onMessage(const TcpConnectionPtr& conn, Buffer* buf) {
        std::string data = buf->retrieveAllAsString();

        // Check if this connection has completed the WebSocket handshake
        bool upgraded = false;
        auto it = upgradedConns_.find(conn->name());
        if (it != upgradedConns_.end()) {
            upgraded = it->second;
        }

        if (!upgraded) {
            // --- HTTP Upgrade handshake ---
            handleHandshake(conn, data);
        } else {
            // --- WebSocket frame processing ---
            handleFrames(conn, data);
        }
    }

    void handleHandshake(const TcpConnectionPtr& conn, const std::string& data) {
        // Find the end of HTTP headers
        size_t headerEnd = data.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            // Incomplete headers; in a real server we'd buffer, but for tests
            // the client sends the full handshake in one go.
            return;
        }

        // Extract Sec-WebSocket-Key from headers
        std::string key;
        std::string headerBlock = data.substr(0, headerEnd);
        size_t pos = 0;
        while (pos < headerBlock.size()) {
            size_t lineEnd = headerBlock.find("\r\n", pos);
            if (lineEnd == std::string::npos) lineEnd = headerBlock.size();
            std::string line = headerBlock.substr(pos, lineEnd - pos);

            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string hdrName = line.substr(0, colon);
                std::string hdrValue = line.substr(colon + 1);
                // Trim leading whitespace
                size_t vStart = hdrValue.find_first_not_of(" \t");
                if (vStart != std::string::npos) hdrValue = hdrValue.substr(vStart);

                // Case-insensitive comparison
                std::string lowerName = hdrName;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                if (lowerName == "sec-websocket-key") {
                    key = hdrValue;
                }
            }
            pos = (lineEnd == headerBlock.size()) ? lineEnd : lineEnd + 2;
        }

        if (key.empty()) {
            conn->shutdown();
            return;
        }

        // Compute accept key
        std::string acceptKey = WebSocketFrameCodec::computeAcceptKey(key);

        // Build and send 101 Switching Protocols response
        std::string response;
        response += "HTTP/1.1 101 Switching Protocols\r\n";
        response += "Upgrade: websocket\r\n";
        response += "Connection: Upgrade\r\n";
        response += "Sec-WebSocket-Accept: " + acceptKey + "\r\n";
        response += "\r\n";

        conn->send(response);
        upgradedConns_[conn->name()] = true;

        // Any bytes after the header belong to the first frame(s)
        size_t bodyStart = headerEnd + 4;
        if (bodyStart < data.size()) {
            std::string remaining = data.substr(bodyStart);
            handleFrames(conn, remaining);
        }
    }

    void handleFrames(const TcpConnectionPtr& conn, const std::string& data) {
        std::string connName = conn->name();
        auto& frameBuf = frameBuffers_[connName];
        frameBuf.insert(frameBuf.end(), data.begin(), data.end());

        while (frameBuf.size() >= 2) {
            auto result = WebSocketFrameCodec::decode(frameBuf.data(), frameBuf.size());

            if (result.status == WebSocketFrameCodec::DecodeResult::Incomplete) {
                break;
            }
            if (result.status == WebSocketFrameCodec::DecodeResult::Error) {
                frameBuf.clear();
                break;
            }

            const WebSocketFrame& frame = result.frame;

            switch (frame.opcode) {
                case WsOpcode::Text: {
                    // Echo back (unmasked, server -> client)
                    auto encoded = WebSocketFrameCodec::encodeText(frame.textPayload());
                    conn->send(std::string(encoded.begin(), encoded.end()));
                    break;
                }
                case WsOpcode::Binary: {
                    auto encoded = WebSocketFrameCodec::encodeBinary(frame.payload);
                    conn->send(std::string(encoded.begin(), encoded.end()));
                    break;
                }
                case WsOpcode::Ping: {
                    // Reply with Pong carrying same payload
                    auto encoded = WebSocketFrameCodec::encodePong(frame.payload);
                    conn->send(std::string(encoded.begin(), encoded.end()));
                    break;
                }
                case WsOpcode::Close: {
                    // Echo close frame back and shutdown
                    auto encoded = WebSocketFrameCodec::encodeClose(1000);
                    conn->send(std::string(encoded.begin(), encoded.end()));
                    conn->shutdown();
                    break;
                }
                default:
                    break;
            }

            frameBuf.erase(frameBuf.begin(),
                           frameBuf.begin() + static_cast<long>(result.consumed));
        }
    }
};

// ---------------------------------------------------------------------------
//  Helper: start inline WS echo server on an EventLoopThread
// ---------------------------------------------------------------------------

static EventLoop* startWsEchoServer(EventLoopThread& serverThread,
                                     std::unique_ptr<InlineWsEchoServer>& serverOut) {
    EventLoop* loop = serverThread.startLoop();

    loop->runInLoop([&serverOut, loop]() {
        InetAddress addr(kWsTestPort);
        serverOut.reset(new InlineWsEchoServer(loop, addr));
        serverOut->start();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return loop;
}

// ---------------------------------------------------------------------------
//  Test cases
// ---------------------------------------------------------------------------

TEST(handshake) {
    // Start echo server
    EventLoopThread serverThread;
    std::unique_ptr<InlineWsEchoServer> server;
    startWsEchoServer(serverThread, server);

    // Start client
    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    InetAddress serverAddr(kWsTestPort, "127.0.0.1");
    WebSocketClient client(clientLoop, serverAddr, "WsTestClient-Handshake", "/");

    std::promise<bool> openPromise;
    auto openFuture = openPromise.get_future();

    client.setOpenCallback([&openPromise]() {
        openPromise.set_value(true);
    });

    client.setErrorCallback([&openPromise](const std::string& err) {
        std::cerr << "WS error: " << err << std::endl;
        try { openPromise.set_value(false); } catch (...) {}
    });

    client.connect();

    auto status = openFuture.wait_for(std::chrono::seconds(3));
    assert(status == std::future_status::ready);
    assert(openFuture.get() == true);
    assert(client.isOpen());

    client.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

TEST(send_receive) {
    EventLoopThread serverThread;
    std::unique_ptr<InlineWsEchoServer> server;
    startWsEchoServer(serverThread, server);

    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    InetAddress serverAddr(kWsTestPort, "127.0.0.1");
    WebSocketClient client(clientLoop, serverAddr, "WsTestClient-Echo", "/");

    std::promise<bool> openPromise;
    auto openFuture = openPromise.get_future();

    std::promise<std::string> msgPromise;
    auto msgFuture = msgPromise.get_future();

    client.setOpenCallback([&openPromise]() {
        openPromise.set_value(true);
    });

    client.setMessageCallback([&msgPromise](const WsMessage& msg) {
        if (msg.isText()) {
            try { msgPromise.set_value(msg.text()); } catch (...) {}
        }
    });

    client.connect();

    // Wait for handshake
    auto openStatus = openFuture.wait_for(std::chrono::seconds(3));
    assert(openStatus == std::future_status::ready);
    assert(openFuture.get() == true);

    // Send a text message and wait for echo
    client.sendText("hello");

    auto msgStatus = msgFuture.wait_for(std::chrono::seconds(3));
    assert(msgStatus == std::future_status::ready);
    std::string echoed = msgFuture.get();
    assert(echoed == "hello");

    client.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

TEST(ping) {
    EventLoopThread serverThread;
    std::unique_ptr<InlineWsEchoServer> server;
    startWsEchoServer(serverThread, server);

    EventLoopThread clientThread;
    EventLoop* clientLoop = clientThread.startLoop();

    InetAddress serverAddr(kWsTestPort, "127.0.0.1");
    WebSocketClient client(clientLoop, serverAddr, "WsTestClient-Ping", "/");

    std::promise<bool> openPromise;
    auto openFuture = openPromise.get_future();

    client.setOpenCallback([&openPromise]() {
        openPromise.set_value(true);
    });

    client.connect();

    auto openStatus = openFuture.wait_for(std::chrono::seconds(3));
    assert(openStatus == std::future_status::ready);
    assert(openFuture.get() == true);

    // Send a ping — the server should reply with pong.
    // WebSocketClient automatically handles the pong internally (silently ignored).
    // The main goal here is to verify no crash occurs.
    client.ping();

    // Give time for the round-trip
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Connection should still be open after ping/pong exchange
    assert(client.isOpen());

    client.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------

int main() {
    std::cout << "========== WebSocketClient Integration Tests ==========" << std::endl;

    RUN_TEST(handshake);
    RUN_TEST(send_receive);
    RUN_TEST(ping);

    std::cout << "========== All WebSocketClient tests PASSED ==========" << std::endl;
    return 0;
}
