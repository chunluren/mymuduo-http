#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include "src/http/HttpServer.h"

using namespace std;

void testShutdownCompiles() {
    cout << "=== Testing Graceful Shutdown ===" << endl;

    EventLoop loop;
    InetAddress addr(0);  // random port
    HttpServer server(&loop, addr, "TestServer");

    // Start server in background thread
    thread t([&loop]() {
        loop.loop();
    });

    // Schedule shutdown after 100ms
    loop.runAfter(0.1, [&server]() {
        server.shutdown(0.1);
    });

    t.join();  // Should complete within ~200ms
    cout << "Graceful shutdown test passed!" << endl;
}

int main() {
    cout << "Starting Graceful Shutdown Tests..." << endl << endl;
    testShutdownCompiles();
    cout << endl << "All Graceful Shutdown tests passed!" << endl;
    return 0;
}
