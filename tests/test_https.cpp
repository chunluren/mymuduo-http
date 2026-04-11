/**
 * @file test_https.cpp
 * @brief HTTPS/TLS 模块测试
 *
 * 测试内容:
 * - SslContext 创建和配置
 * - HttpsServer 编译验证
 * - Memory BIO SSL 加解密验证（自签名证书）
 */

#include <iostream>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>

#include "src/http/SslContext.h"
#include "src/http/HttpsServer.h"

using namespace std;

// ==================== 测试宏 ====================
#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    cout << "  Running " #name "..." << endl; \
    test_##name(); \
    cout << "  PASSED: " #name << endl; \
} while(0)

// ==================== 辅助函数 ====================

/// 生成自签名测试证书（用于测试 SSL 功能）
static bool generateTestCert(const string& certFile, const string& keyFile) {
    // 使用 OpenSSL 命令行生成自签名证书
    string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + keyFile +
                 " -out " + certFile + " -days 1 -nodes -subj '/CN=localhost' 2>/dev/null";
    return system(cmd.c_str()) == 0;
}

/// 清理测试文件
static void cleanupTestFiles(const string& certFile, const string& keyFile) {
    remove(certFile.c_str());
    remove(keyFile.c_str());
}

// ==================== 测试用例 ====================

TEST(ssl_context_creation) {
    cout << "=== Testing SslContext Creation ===" << endl;
    SslContext ctx;
    assert(ctx.get() != nullptr);
    cout << "SslContext created successfully, SSL_CTX* is valid" << endl;
}

TEST(ssl_context_move) {
    cout << "=== Testing SslContext Move Semantics ===" << endl;
    SslContext ctx1;
    SSL_CTX* ptr = ctx1.get();
    assert(ptr != nullptr);

    // Move construct
    SslContext ctx2(std::move(ctx1));
    assert(ctx2.get() == ptr);
    assert(ctx1.get() == nullptr);

    // Move assign
    SslContext ctx3;
    ctx3 = std::move(ctx2);
    assert(ctx3.get() == ptr);
    assert(ctx2.get() == nullptr);
    cout << "Move semantics work correctly" << endl;
}

TEST(ssl_context_load_cert) {
    cout << "=== Testing SslContext Certificate Loading ===" << endl;
    string certFile = "/tmp/test_https_cert.pem";
    string keyFile = "/tmp/test_https_key.pem";

    // Generate test certificate
    if (!generateTestCert(certFile, keyFile)) {
        cout << "  SKIPPED: openssl command not available" << endl;
        return;
    }

    SslContext ctx;
    bool loaded = ctx.loadCert(certFile, keyFile);
    assert(loaded);
    cout << "Certificate loaded successfully" << endl;

    // Test with non-existent files
    bool badLoad = ctx.loadCert("/nonexistent/cert.pem", "/nonexistent/key.pem");
    assert(!badLoad);
    cout << "Correctly rejected non-existent certificate files" << endl;

    cleanupTestFiles(certFile, keyFile);
}

TEST(ssl_memory_bio_roundtrip) {
    cout << "=== Testing SSL Memory BIO Encrypt/Decrypt ===" << endl;
    string certFile = "/tmp/test_https_cert.pem";
    string keyFile = "/tmp/test_https_key.pem";

    if (!generateTestCert(certFile, keyFile)) {
        cout << "  SKIPPED: openssl command not available" << endl;
        return;
    }

    // Create server-side SSL
    SslContext serverCtx;
    assert(serverCtx.loadCert(certFile, keyFile));

    SSL* serverSsl = SSL_new(serverCtx.get());
    assert(serverSsl != nullptr);
    BIO* serverRbio = BIO_new(BIO_s_mem());
    BIO* serverWbio = BIO_new(BIO_s_mem());
    BIO_set_nbio(serverRbio, 1);
    BIO_set_nbio(serverWbio, 1);
    SSL_set_bio(serverSsl, serverRbio, serverWbio);
    SSL_set_accept_state(serverSsl);

    // Create client-side SSL (using TLS_client_method)
    SSL_CTX* clientCtx = SSL_CTX_new(TLS_client_method());
    assert(clientCtx != nullptr);
    // Don't verify server cert in test
    SSL_CTX_set_verify(clientCtx, SSL_VERIFY_NONE, nullptr);

    SSL* clientSsl = SSL_new(clientCtx);
    assert(clientSsl != nullptr);
    BIO* clientRbio = BIO_new(BIO_s_mem());
    BIO* clientWbio = BIO_new(BIO_s_mem());
    BIO_set_nbio(clientRbio, 1);
    BIO_set_nbio(clientWbio, 1);
    SSL_set_bio(clientSsl, clientRbio, clientWbio);
    SSL_set_connect_state(clientSsl);

    // Perform handshake via memory BIOs (shuttle data between client and server)
    auto shuttleData = [](BIO* fromWbio, BIO* toRbio) -> int {
        char buf[16384];
        int total = 0;
        int pending;
        while ((pending = BIO_ctrl_pending(fromWbio)) > 0) {
            int n = BIO_read(fromWbio, buf, min(pending, (int)sizeof(buf)));
            if (n > 0) {
                BIO_write(toRbio, buf, n);
                total += n;
            }
        }
        return total;
    };

    // Handshake loop
    bool clientDone = false, serverDone = false;
    for (int i = 0; i < 20 && !(clientDone && serverDone); ++i) {
        if (!clientDone) {
            int ret = SSL_do_handshake(clientSsl);
            if (ret == 1) clientDone = true;
        }
        // Client -> Server
        shuttleData(clientWbio, serverRbio);

        if (!serverDone) {
            int ret = SSL_do_handshake(serverSsl);
            if (ret == 1) serverDone = true;
        }
        // Server -> Client
        shuttleData(serverWbio, clientRbio);
    }

    assert(clientDone && serverDone);
    cout << "SSL handshake completed via memory BIOs" << endl;

    // Test data exchange: client sends, server receives
    const string testMessage = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    int written = SSL_write(clientSsl, testMessage.c_str(), (int)testMessage.size());
    assert(written == (int)testMessage.size());

    // Shuttle encrypted data: client -> server
    shuttleData(clientWbio, serverRbio);

    // Server reads decrypted data
    char recvBuf[4096];
    int recvLen = SSL_read(serverSsl, recvBuf, sizeof(recvBuf));
    assert(recvLen == (int)testMessage.size());
    assert(string(recvBuf, recvLen) == testMessage);
    cout << "Client -> Server data exchange verified" << endl;

    // Server sends response
    const string response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
    written = SSL_write(serverSsl, response.c_str(), (int)response.size());
    assert(written == (int)response.size());

    // Shuttle: server -> client
    shuttleData(serverWbio, clientRbio);

    // Client reads
    recvLen = SSL_read(clientSsl, recvBuf, sizeof(recvBuf));
    assert(recvLen == (int)response.size());
    assert(string(recvBuf, recvLen) == response);
    cout << "Server -> Client data exchange verified" << endl;

    // Cleanup
    SSL_free(clientSsl);
    SSL_free(serverSsl);
    SSL_CTX_free(clientCtx);
    cleanupTestFiles(certFile, keyFile);
}

TEST(https_server_compilation) {
    cout << "=== Testing HttpsServer Compilation ===" << endl;
    // Verify the class compiles and the API is usable
    // We can't actually start the server without a valid cert binding to a port,
    // but we verify the types and method signatures exist.

    string certFile = "/tmp/test_https_cert.pem";
    string keyFile = "/tmp/test_https_key.pem";

    if (!generateTestCert(certFile, keyFile)) {
        cout << "  SKIPPED: openssl command not available" << endl;
        return;
    }

    EventLoop loop;
    InetAddress addr(0);  // port 0 = OS picks available port
    HttpsServer server(&loop, addr, certFile, keyFile, "TestHttpsServer");

    // Verify API methods exist and compile
    server.setThreadNum(2);
    server.setIdleTimeout(30.0);
    server.enableGzip(512);
    server.enableCors("*");

    server.GET("/test", [](const HttpRequest& /*req*/, HttpResponse& resp) {
        resp.setJson(R"({"status":"ok"})");
    });
    server.POST("/data", [](const HttpRequest& req, HttpResponse& resp) {
        resp.setText("Received: " + req.body);
    });
    server.PUT("/update", [](const HttpRequest& /*req*/, HttpResponse& resp) {
        resp.setStatusCode(HttpStatusCode::NO_CONTENT);
    });
    server.DELETE("/remove", [](const HttpRequest& /*req*/, HttpResponse& resp) {
        resp.setStatusCode(HttpStatusCode::OK);
        resp.setText("Deleted");
    });

    server.use([](const HttpRequest& /*req*/, HttpResponse& resp) {
        resp.setHeader("X-Test", "middleware-works");
    });

    cout << "HttpsServer API compilation verified" << endl;
    cleanupTestFiles(certFile, keyFile);
}

// ==================== Main ====================

int main() {
    cout << "Starting HTTPS/TLS Tests..." << endl << endl;

    RUN_TEST(ssl_context_creation);
    RUN_TEST(ssl_context_move);
    RUN_TEST(ssl_context_load_cert);
    RUN_TEST(ssl_memory_bio_roundtrip);
    RUN_TEST(https_server_compilation);

    cout << endl << "All HTTPS tests passed!" << endl;
    return 0;
}
