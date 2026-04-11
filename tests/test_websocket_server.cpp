// test_websocket_server.cpp - WebSocketServer compilation/link test
#include <iostream>
#include <cassert>
#include "src/websocket/WebSocketServer.h"

using namespace std;

void testCompilation() {
    cout << "=== Testing WebSocketServer Compilation ===" << endl;

    EventLoop loop;
    InetAddress addr(0);
    WebSocketServer server(&loop, addr, "TestWsServer");

    WebSocketConfig config;
    config.maxMessageSize = 1024;
    config.idleTimeoutMs = 5000;
    config.enablePingPong = true;
    config.pingIntervalMs = 3000;
    server.setConfig(config);

    server.setConnectionHandler([](const WsSessionPtr&) {});
    server.setMessageHandler([](const WsSessionPtr&, const WsMessage&) {});
    server.setCloseHandler([](const WsSessionPtr&) {});
    server.setErrorHandler([](const WsSessionPtr&, const std::string&) {});
    server.setHandshakeValidator([](const TcpConnectionPtr&, const std::string&,
                                    const std::map<std::string, std::string>&) {
        return true;
    });

    assert(server.sessionCount() == 0);
    assert(server.getAllSessions().empty());

    cout << "WebSocketServer compilation test passed!" << endl;
}

int main() {
    cout << "Starting WebSocketServer Tests..." << endl << endl;
    testCompilation();
    cout << endl << "All WebSocketServer tests passed!" << endl;
    return 0;
}
