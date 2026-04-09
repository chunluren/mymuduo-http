// test_http.cpp - HttpRequest and HttpResponse tests
#include <iostream>
#include <cassert>
#include <string>
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    test_##name(); \
    std::cout << "PASSED" << std::endl; \
} while(0)

// ==================== HttpRequest Tests ====================

TEST(parseGetRequestLine) {
    HttpRequest req;
    bool ok = req.parseRequestLine("GET /path?key=val HTTP/1.1");
    assert(ok);
    assert(req.method == HttpMethod::GET);
    assert(req.path == "/path");
    assert(req.query == "key=val");
    assert(req.params.count("key") == 1);
    assert(req.params["key"] == "val");
    assert(req.version == HttpVersion::HTTP_11);
}

TEST(parsePostRequestLine) {
    HttpRequest req;
    bool ok = req.parseRequestLine("POST /api/users HTTP/1.0");
    assert(ok);
    assert(req.method == HttpMethod::POST);
    assert(req.path == "/api/users");
    assert(req.query.empty());
    assert(req.version == HttpVersion::HTTP_10);
}

TEST(parseHeader) {
    HttpRequest req;
    bool ok = req.parseHeader("Content-Type: application/json");
    assert(ok);
    // Headers are stored with lowercase keys
    assert(req.getHeader("Content-Type") == "application/json");
    assert(req.getHeader("content-type") == "application/json");
    assert(req.getHeader("CONTENT-TYPE") == "application/json");
}

TEST(keepAlive) {
    // HTTP/1.1 defaults to keep-alive
    HttpRequest req11;
    req11.parseRequestLine("GET / HTTP/1.1");
    assert(req11.keepAlive() == true);

    // HTTP/1.1 with Connection: close
    HttpRequest req11close;
    req11close.parseRequestLine("GET / HTTP/1.1");
    req11close.parseHeader("Connection: close");
    assert(req11close.keepAlive() == false);

    // HTTP/1.0 defaults to close
    HttpRequest req10;
    req10.parseRequestLine("GET / HTTP/1.0");
    assert(req10.keepAlive() == false);

    // HTTP/1.0 with Connection: keep-alive
    HttpRequest req10ka;
    req10ka.parseRequestLine("GET / HTTP/1.0");
    req10ka.parseHeader("Connection: keep-alive");
    assert(req10ka.keepAlive() == true);
}

TEST(contentLength) {
    HttpRequest req;
    req.parseHeader("Content-Length: 42");
    assert(req.contentLength() == 42);

    // Missing Content-Length returns 0
    HttpRequest req2;
    assert(req2.contentLength() == 0);
}

TEST(cookies) {
    HttpRequest req;
    req.parseHeader("Cookie: session=abc; theme=dark");
    auto all = req.cookies();
    assert(all.size() == 2);
    assert(all.at("session") == "abc");
    assert(all.at("theme") == "dark");
}

TEST(cookieSingleLookup) {
    HttpRequest req;
    req.parseHeader("Cookie: session=abc; theme=dark");
    assert(req.cookie("session") == "abc");
    assert(req.cookie("theme") == "dark");
    assert(req.cookie("nonexistent").empty());
}

TEST(getParam) {
    HttpRequest req;
    req.parseRequestLine("GET /search?q=hello&page=2 HTTP/1.1");
    assert(req.getParam("q") == "hello");
    assert(req.getParam("page") == "2");
    assert(req.getParam("missing", "default") == "default");
}

// ==================== HttpResponse Tests ====================

TEST(statusCodeOK) {
    HttpResponse resp;
    resp.setStatusCode(HttpStatusCode::OK);
    assert(resp.statusCode == HttpStatusCode::OK);
    assert(resp.statusMessage == "OK");
}

TEST(statusCodeNotFound) {
    HttpResponse resp;
    resp.setStatusCode(HttpStatusCode::NOT_FOUND);
    assert(resp.statusCode == HttpStatusCode::NOT_FOUND);
    assert(resp.statusMessage == "Not Found");
}

TEST(statusCodeMovedPermanently) {
    HttpResponse resp;
    resp.setStatusCode(HttpStatusCode::MOVED_PERMANENTLY);
    assert(resp.statusCode == HttpStatusCode::MOVED_PERMANENTLY);
    assert(resp.statusMessage == "Moved Permanently");
}

TEST(toStringContainsStatusLine) {
    HttpResponse resp;
    resp.setStatusCode(HttpStatusCode::OK);
    resp.setBody("hello");
    std::string s = resp.toString();
    assert(s.find("HTTP/1.1 200 OK") != std::string::npos);
    assert(s.find("Content-Length: 5") != std::string::npos);
    assert(s.find("hello") != std::string::npos);
}

TEST(setJsonAutoContentType) {
    HttpResponse resp;
    resp.setJson(R"({"key":"value"})");
    assert(resp.headers["Content-Type"] == "application/json");
    assert(resp.body == R"({"key":"value"})");
}

TEST(setCorsHeaders) {
    HttpResponse resp;
    resp.setCors();
    assert(resp.headers["Access-Control-Allow-Origin"] == "*");
    assert(resp.headers["Access-Control-Allow-Methods"].find("GET") != std::string::npos);
    assert(resp.headers["Access-Control-Allow-Headers"].find("Content-Type") != std::string::npos);
    assert(resp.headers["Access-Control-Max-Age"] == "86400");
}

TEST(setCookie) {
    HttpResponse resp;
    resp.setCookie("session", "abc123");
    std::string cookie = resp.headers["Set-Cookie"];
    assert(cookie.find("session=abc123") != std::string::npos);
    assert(cookie.find("Path=/") != std::string::npos);
    assert(cookie.find("HttpOnly") != std::string::npos);
}

TEST(redirectFactory) {
    HttpResponse resp = HttpResponse::redirect("/new");
    assert(resp.statusCode == HttpStatusCode::FOUND);
    assert(resp.statusMessage == "Found");
    assert(resp.headers["Location"] == "/new");
}

TEST(notFoundFactory) {
    HttpResponse resp = HttpResponse::notFound();
    assert(resp.statusCode == HttpStatusCode::NOT_FOUND);
    assert(resp.statusMessage == "Not Found");
    assert(resp.body == "Not Found");
    assert(resp.headers["Content-Type"] == "text/plain; charset=utf-8");
}

int main() {
    std::cout << "=== HttpRequest & HttpResponse Tests ===" << std::endl;

    // HttpRequest tests
    RUN_TEST(parseGetRequestLine);
    RUN_TEST(parsePostRequestLine);
    RUN_TEST(parseHeader);
    RUN_TEST(keepAlive);
    RUN_TEST(contentLength);
    RUN_TEST(cookies);
    RUN_TEST(cookieSingleLookup);
    RUN_TEST(getParam);

    // HttpResponse tests
    RUN_TEST(statusCodeOK);
    RUN_TEST(statusCodeNotFound);
    RUN_TEST(statusCodeMovedPermanently);
    RUN_TEST(toStringContainsStatusLine);
    RUN_TEST(setJsonAutoContentType);
    RUN_TEST(setCorsHeaders);
    RUN_TEST(setCookie);
    RUN_TEST(redirectFactory);
    RUN_TEST(notFoundFactory);

    std::cout << std::endl << "All HTTP tests passed!" << std::endl;
    return 0;
}
