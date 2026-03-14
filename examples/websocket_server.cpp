// websocket_server.cpp - WebSocket 服务器示例
#include <iostream>
#include <signal.h>
#include "EventLoop.h"
#include "InetAddress.h"
#include "src/websocket/WebSocketServer.h"

using namespace std;

EventLoop* g_loop = nullptr;

void signalHandler(int) {
    if (g_loop) {
        g_loop->quit();
    }
}

int main() {
    cout << "=== WebSocket Server ===" << endl;

    EventLoop loop;
    g_loop = &loop;

    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    InetAddress addr(9500);  // 默认端口 9500
    WebSocketServer server(&loop, addr, "WebSocketServer");

    server.setThreadNum(4);

    // 设置连接处理器
    server.setConnectionHandler([](const WsSessionPtr& session) {
        cout << "New WebSocket connection from: " << session->clientAddress() << endl;
        session->sendText("Welcome to WebSocket Server!");
    });

    // 设置消息处理器
    server.setMessageHandler([](const WsSessionPtr& session, const WsMessage& msg) {
        if (msg.isText()) {
            cout << "Received text: " << msg.text() << endl;
            // Echo back
            session->sendText("Echo: " + msg.text());
        } else if (msg.isBinary()) {
            cout << "Received binary data, size: " << msg.data.size() << endl;
            session->sendBinary(msg.data);
        }
    });

    // 设置关闭处理器
    server.setCloseHandler([](const WsSessionPtr& session) {
        cout << "Connection closed: " << session->clientAddress() << endl;
    });

    server.start();

    cout << "WebSocket Server started on port 9500" << endl;
    cout << "Connect using: ws://localhost:9500" << endl;

    loop.loop();

    cout << "WebSocket Server stopped" << endl;
    return 0;
}