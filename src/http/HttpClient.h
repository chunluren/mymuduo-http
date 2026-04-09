/**
 * @file HttpClient.h
 * @brief Reactor-based HTTP client (header-only)
 *
 * HttpClient is the client-side counterpart of HttpServer, built on TcpClient.
 * It maintains a persistent TCP connection to the server and supports
 * synchronous and asynchronous HTTP requests over HTTP/1.1 keep-alive.
 *
 * Design follows the same Reactor pattern as ReactorRpcClient:
 * - TcpClient internally for persistent connection
 * - connectionCallback stores conn_, notifies condition variable
 * - messageCallback parses response, fulfills promise
 * - Synchronous methods send request + wait on future with timeout
 * - Async methods return future directly
 *
 * @example Basic usage
 * @code
 * EventLoop loop;
 * InetAddress serverAddr("127.0.0.1", 8080);
 * HttpClient client(&loop, serverAddr, "MyHttpClient");
 *
 * client.connect();
 *
 * // Synchronous GET
 * HttpClientResponse resp = client.GET("/api/users");
 * if (resp.success) {
 *     std::cout << "Status: " << resp.statusCode << std::endl;
 *     std::cout << "Body: " << resp.body << std::endl;
 * }
 *
 * // Synchronous POST
 * HttpClientResponse resp2 = client.POST("/api/users",
 *     R"({"name":"Alice"})", "application/json");
 *
 * // Asynchronous GET
 * auto future = client.asyncGET("/api/status");
 * // ... do other work ...
 * HttpClientResponse resp3 = future.get();
 *
 * client.disconnect();
 * @endcode
 */

#pragma once

#include "net/TcpClient.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/Buffer.h"
#include "net/Callbacks.h"

#include <string>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <future>
#include <algorithm>
#include <cstring>

/**
 * @struct HttpClientResponse
 * @brief Stores the result of an HTTP request
 *
 * Fields:
 * - statusCode: HTTP status code (200, 404, 500, etc.), 0 on failure
 * - statusMessage: Status text ("OK", "Not Found", etc.)
 * - headers: Response headers (keys are lowercase)
 * - body: Response body
 * - success: true if a valid HTTP response was received
 * - error: Human-readable error description on failure
 */
struct HttpClientResponse {
    int statusCode = 0;                                     ///< HTTP status code, 0 if no response
    std::string statusMessage;                              ///< HTTP status text
    std::unordered_map<std::string, std::string> headers;   ///< Response headers (lowercase keys)
    std::string body;                                       ///< Response body
    bool success = false;                                   ///< Whether the request succeeded
    std::string error;                                      ///< Error description on failure
};

/**
 * @class HttpClient
 * @brief Reactor-based HTTP/1.1 client with persistent connection
 *
 * HttpClient wraps TcpClient to provide a high-level HTTP client API.
 * It maintains a single persistent connection (HTTP/1.1 keep-alive)
 * and uses a FIFO promise queue to match responses to requests
 * (HTTP/1.1 guarantees response ordering).
 *
 * Features:
 * - Persistent TCP connection with automatic reconnect
 * - Synchronous methods with configurable timeout
 * - Asynchronous methods returning std::future
 * - Support for GET, POST, PUT, DELETE methods
 * - Custom default headers
 * - Incremental response parsing (handles partial reads)
 *
 * Thread safety:
 * - conn_, pendingQueue_, and recvBuffer_ are protected by mutex_
 * - Public methods are safe to call from any thread
 * - I/O callbacks run on the EventLoop thread
 *
 * @note This is a header-only implementation.
 */
class HttpClient {
public:
    /**
     * @brief Construct an HTTP client
     * @param loop EventLoop to run I/O on
     * @param serverAddr Server address to connect to
     * @param name Client name (for logging)
     *
     * Sets up TcpClient with connection and message callbacks.
     * Enables automatic reconnection on disconnect.
     * The Host header is derived from serverAddr.
     */
    HttpClient(EventLoop* loop, const InetAddress& serverAddr, const std::string& name)
        : client_(loop, serverAddr, name)
        , host_(serverAddr.toIpPort())
    {
        client_.setConnectionCallback(
            [this](const TcpConnectionPtr& conn) {
                onConnection(conn);
            });

        client_.setMessageCallback(
            [this](const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
                onMessage(conn, buf, time);
            });

        client_.enableRetry();
    }

    /**
     * @brief Initiate connection to the server
     *
     * Non-blocking. The actual TCP handshake happens asynchronously.
     * Use the synchronous methods (GET, POST, etc.) which will wait
     * for the connection to be established before sending.
     */
    void connect() { client_.connect(); }

    /**
     * @brief Gracefully disconnect from the server
     *
     * Outstanding requests in the pending queue will receive error responses.
     */
    void disconnect() { client_.disconnect(); }

    /**
     * @brief Set a default header to include in every request
     * @param key Header name (e.g. "Authorization")
     * @param value Header value (e.g. "Bearer token123")
     *
     * Default headers are appended to every outgoing request.
     * Per-request headers (Content-Type, Content-Length, Host, Connection)
     * are set automatically and take precedence.
     *
     * @example
     * @code
     * client.setHeader("Authorization", "Bearer mytoken");
     * client.setHeader("Accept", "application/json");
     * @endcode
     */
    void setHeader(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        defaultHeaders_[key] = value;
    }

    // ==================== Synchronous Methods ====================

    /**
     * @brief Perform a synchronous HTTP GET request
     * @param path Request path (e.g. "/api/users")
     * @param timeoutMs Timeout in milliseconds (default 5000)
     * @return HttpClientResponse with the server's response
     *
     * Blocks until the response is received or the timeout expires.
     *
     * @example
     * @code
     * HttpClientResponse resp = client.GET("/api/users?page=1");
     * if (resp.success) {
     *     std::cout << resp.body << std::endl;
     * }
     * @endcode
     */
    HttpClientResponse GET(const std::string& path, int timeoutMs = 5000) {
        return request("GET", path, "", "", timeoutMs);
    }

    /**
     * @brief Perform a synchronous HTTP POST request
     * @param path Request path (e.g. "/api/users")
     * @param body Request body
     * @param contentType Content-Type header (default "application/json")
     * @param timeoutMs Timeout in milliseconds (default 5000)
     * @return HttpClientResponse with the server's response
     *
     * @example
     * @code
     * HttpClientResponse resp = client.POST("/api/users",
     *     R"({"name":"Alice","age":30})", "application/json");
     * @endcode
     */
    HttpClientResponse POST(const std::string& path, const std::string& body,
                            const std::string& contentType = "application/json",
                            int timeoutMs = 5000) {
        return request("POST", path, body, contentType, timeoutMs);
    }

    /**
     * @brief Perform a synchronous HTTP PUT request
     * @param path Request path (e.g. "/api/users/1")
     * @param body Request body
     * @param contentType Content-Type header (default "application/json")
     * @param timeoutMs Timeout in milliseconds (default 5000)
     * @return HttpClientResponse with the server's response
     *
     * @example
     * @code
     * HttpClientResponse resp = client.PUT("/api/users/1",
     *     R"({"name":"Bob"})", "application/json");
     * @endcode
     */
    HttpClientResponse PUT(const std::string& path, const std::string& body,
                           const std::string& contentType = "application/json",
                           int timeoutMs = 5000) {
        return request("PUT", path, body, contentType, timeoutMs);
    }

    /**
     * @brief Perform a synchronous HTTP DELETE request
     * @param path Request path (e.g. "/api/users/1")
     * @param timeoutMs Timeout in milliseconds (default 5000)
     * @return HttpClientResponse with the server's response
     *
     * @note Named DELETE_METHOD to avoid collision with the C preprocessor
     *       macro DELETE defined in some system headers.
     *
     * @example
     * @code
     * HttpClientResponse resp = client.DELETE_METHOD("/api/users/1");
     * @endcode
     */
    HttpClientResponse DELETE_METHOD(const std::string& path, int timeoutMs = 5000) {
        return request("DELETE", path, "", "", timeoutMs);
    }

    // ==================== Asynchronous Methods ====================

    /**
     * @brief Perform an asynchronous HTTP GET request
     * @param path Request path (e.g. "/api/users")
     * @return std::future<HttpClientResponse> that will hold the response
     *
     * @example
     * @code
     * auto future = client.asyncGET("/api/status");
     * // ... do other work ...
     * HttpClientResponse resp = future.get();
     * @endcode
     */
    std::future<HttpClientResponse> asyncGET(const std::string& path) {
        return asyncRequest("GET", path, "", "");
    }

    /**
     * @brief Perform an asynchronous HTTP POST request
     * @param path Request path (e.g. "/api/users")
     * @param body Request body
     * @param contentType Content-Type header (default "application/json")
     * @return std::future<HttpClientResponse> that will hold the response
     *
     * @example
     * @code
     * auto future = client.asyncPOST("/api/users",
     *     R"({"name":"Alice"})", "application/json");
     * HttpClientResponse resp = future.get();
     * @endcode
     */
    std::future<HttpClientResponse> asyncPOST(const std::string& path,
                                               const std::string& body,
                                               const std::string& contentType = "application/json") {
        return asyncRequest("POST", path, body, contentType);
    }

    // ==================== Generic Methods ====================

    /**
     * @brief Perform a synchronous HTTP request with any method
     * @param method HTTP method string ("GET", "POST", "PUT", "DELETE", etc.)
     * @param path Request path
     * @param body Request body (empty for GET/DELETE)
     * @param contentType Content-Type header (empty if no body)
     * @param timeoutMs Timeout in milliseconds (default 5000)
     * @return HttpClientResponse with the server's response
     *
     * This is the core synchronous method. All convenience methods
     * (GET, POST, PUT, DELETE_METHOD) delegate to this.
     *
     * @example
     * @code
     * HttpClientResponse resp = client.request("PATCH", "/api/users/1",
     *     R"({"name":"Charlie"})", "application/json", 3000);
     * @endcode
     */
    HttpClientResponse request(const std::string& method, const std::string& path,
                               const std::string& body = "",
                               const std::string& contentType = "",
                               int timeoutMs = 5000) {
        auto future = asyncRequest(method, path, body, contentType);
        if (future.wait_for(std::chrono::milliseconds(timeoutMs)) ==
            std::future_status::timeout) {
            HttpClientResponse resp;
            resp.error = "HTTP request timeout after " + std::to_string(timeoutMs) + "ms";
            return resp;
        }
        return future.get();
    }

    /**
     * @brief Perform an asynchronous HTTP request with any method
     * @param method HTTP method string ("GET", "POST", "PUT", "DELETE", etc.)
     * @param path Request path
     * @param body Request body (empty for GET/DELETE)
     * @param contentType Content-Type header (empty if no body)
     * @return std::future<HttpClientResponse> that will hold the response
     *
     * This is the core asynchronous method. All convenience methods
     * and the synchronous request() delegate to this.
     *
     * The method:
     * 1. Creates a promise/future pair
     * 2. Pushes the promise onto the FIFO pending queue
     * 3. Waits up to 3 seconds for a connection to be established
     * 4. Builds and sends the HTTP request
     * 5. Returns the future (fulfilled when the response arrives)
     *
     * @example
     * @code
     * auto future = client.asyncRequest("PATCH", "/api/users/1",
     *     R"({"name":"Charlie"})", "application/json");
     * HttpClientResponse resp = future.get();
     * @endcode
     */
    std::future<HttpClientResponse> asyncRequest(const std::string& method,
                                                  const std::string& path,
                                                  const std::string& body = "",
                                                  const std::string& contentType = "") {
        std::promise<HttpClientResponse> promise;
        auto future = promise.get_future();

        std::string httpReq = buildRequest(method, path, body, contentType);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingQueue_.push(std::move(promise));
        }

        // Wait for connection to be established (up to 3 seconds)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!conn_) {
                connCv_.wait_for(lock, std::chrono::milliseconds(3000),
                                 [this] { return conn_ != nullptr; });
            }
            if (conn_) {
                conn_->send(httpReq);
            } else {
                // Connection not established; pop the promise and set error
                if (!pendingQueue_.empty()) {
                    auto& frontPromise = pendingQueue_.front();
                    HttpClientResponse resp;
                    resp.error = "not connected";
                    frontPromise.set_value(std::move(resp));
                    pendingQueue_.pop();
                }
            }
        }

        return future;
    }

private:
    TcpClient client_;                                              ///< Underlying TCP client
    std::string host_;                                              ///< Host header value (ip:port)

    mutable std::mutex mutex_;                                      ///< Protects conn_, pendingQueue_, recvBuffer_, defaultHeaders_
    std::condition_variable connCv_;                                 ///< Notified when connection is established
    TcpConnectionPtr conn_;                                         ///< Current TCP connection (nullptr if disconnected)

    std::queue<std::promise<HttpClientResponse>> pendingQueue_;     ///< FIFO queue of pending request promises
    std::string recvBuffer_;                                        ///< Accumulates partial response data across onMessage calls
    std::unordered_map<std::string, std::string> defaultHeaders_;   ///< User-configured default headers

    /**
     * @brief Connection callback
     * @param conn The TCP connection
     *
     * Called by TcpClient when the connection state changes.
     * On connect: stores conn_ and notifies waiting threads via connCv_.
     * On disconnect: clears conn_ and fails all pending requests.
     */
    void onConnection(const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            std::lock_guard<std::mutex> lock(mutex_);
            conn_ = conn;
            connCv_.notify_all();
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            conn_.reset();
            recvBuffer_.clear();

            // Fail all pending requests on disconnect
            while (!pendingQueue_.empty()) {
                HttpClientResponse resp;
                resp.error = "connection closed";
                pendingQueue_.front().set_value(std::move(resp));
                pendingQueue_.pop();
            }
        }
    }

    /**
     * @brief Message callback — incrementally parses HTTP responses
     * @param conn The TCP connection (unused)
     * @param buf Input buffer from TcpClient
     * @param time Timestamp (unused)
     *
     * Parsing algorithm (mirrors server-side parseRequest logic):
     * 1. Append all available data from buf to recvBuffer_
     * 2. Find "\r\n\r\n" to locate header/body boundary
     * 3. Parse status line: "HTTP/1.1 200 OK" -> statusCode + statusMessage
     * 4. Parse headers: each "Key: Value" line, store with lowercase key
     * 5. Read Content-Length to determine expected body size
     * 6. If recvBuffer_ does not have enough bytes, return and wait for more data
     * 7. Extract body, remove consumed bytes from recvBuffer_
     * 8. Pop front promise from pendingQueue_ and fulfill it
     * 9. Loop to handle pipelined responses in a single read
     */
    void onMessage(const TcpConnectionPtr& /*conn*/, Buffer* buf, Timestamp /*time*/) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Step 1: Append new data to the accumulation buffer
        recvBuffer_.append(buf->retrieveAllAsString());

        // Loop to handle multiple pipelined responses in one read
        while (true) {
            // Step 2: Find header/body boundary
            size_t headerEnd = recvBuffer_.find("\r\n\r\n");
            if (headerEnd == std::string::npos) {
                // Headers not yet complete, wait for more data
                return;
            }

            size_t headerLen = headerEnd + 4;  // Include the "\r\n\r\n"
            std::string headerSection = recvBuffer_.substr(0, headerEnd);

            // Step 3: Parse status line
            HttpClientResponse resp;
            size_t firstLineEnd = headerSection.find("\r\n");
            std::string statusLine;
            if (firstLineEnd == std::string::npos) {
                statusLine = headerSection;
            } else {
                statusLine = headerSection.substr(0, firstLineEnd);
            }

            if (!parseStatusLine(statusLine, resp)) {
                // Malformed response; fail the front promise
                resp.error = "malformed HTTP status line: " + statusLine;
                if (!pendingQueue_.empty()) {
                    pendingQueue_.front().set_value(std::move(resp));
                    pendingQueue_.pop();
                }
                recvBuffer_.erase(0, headerLen);
                continue;
            }

            // Step 4: Parse response headers
            if (firstLineEnd != std::string::npos) {
                parseHeaders(headerSection.substr(firstLineEnd + 2), resp);
            }

            // Step 5: Determine expected body length from Content-Length
            size_t contentLength = 0;
            auto it = resp.headers.find("content-length");
            if (it != resp.headers.end()) {
                try {
                    contentLength = std::stoul(it->second);
                } catch (...) {
                    contentLength = 0;
                }
            }

            // Step 6: Check if we have enough data for the full body
            size_t totalLen = headerLen + contentLength;
            if (recvBuffer_.size() < totalLen) {
                // Body not yet complete, wait for more data
                return;
            }

            // Step 7: Extract body and consume from recvBuffer_
            if (contentLength > 0) {
                resp.body = recvBuffer_.substr(headerLen, contentLength);
            }
            resp.success = true;

            recvBuffer_.erase(0, totalLen);

            // Step 8: Fulfill the front promise in the FIFO queue
            if (!pendingQueue_.empty()) {
                pendingQueue_.front().set_value(std::move(resp));
                pendingQueue_.pop();
            }

            // Step 9: Continue loop to check for more complete responses
        }
    }

    /**
     * @brief Parse the HTTP status line
     * @param line Status line string, e.g. "HTTP/1.1 200 OK"
     * @param resp Output response to populate statusCode and statusMessage
     * @return true if parsing succeeded
     *
     * Expected format: "HTTP/x.y STATUS_CODE STATUS_MESSAGE"
     * The status code is extracted as an integer.
     * The status message is everything after the status code.
     */
    bool parseStatusLine(const std::string& line, HttpClientResponse& resp) {
        // "HTTP/1.1 200 OK"
        //  ^        ^   ^
        //  |        |   statusMessage
        //  |        statusCode
        //  version (ignored)

        size_t firstSpace = line.find(' ');
        if (firstSpace == std::string::npos) {
            return false;
        }

        size_t secondSpace = line.find(' ', firstSpace + 1);
        if (secondSpace == std::string::npos) {
            // No status message, just code (e.g. "HTTP/1.1 200")
            std::string codeStr = line.substr(firstSpace + 1);
            try {
                resp.statusCode = std::stoi(codeStr);
            } catch (...) {
                return false;
            }
            return true;
        }

        std::string codeStr = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        try {
            resp.statusCode = std::stoi(codeStr);
        } catch (...) {
            return false;
        }

        resp.statusMessage = line.substr(secondSpace + 1);
        return true;
    }

    /**
     * @brief Parse response headers from the header block
     * @param headerBlock String containing all header lines separated by "\r\n"
     * @param resp Output response to populate headers map
     *
     * Each header line is parsed as "Key: Value".
     * Keys are converted to lowercase for case-insensitive lookup,
     * matching the convention used in HttpRequest.
     */
    void parseHeaders(const std::string& headerBlock, HttpClientResponse& resp) {
        size_t pos = 0;
        while (pos < headerBlock.size()) {
            size_t lineEnd = headerBlock.find("\r\n", pos);
            if (lineEnd == std::string::npos) {
                lineEnd = headerBlock.size();
            }

            std::string line = headerBlock.substr(pos, lineEnd - pos);
            if (!line.empty()) {
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string key = line.substr(0, colon);
                    std::string value = line.substr(colon + 1);

                    // Trim leading whitespace from value
                    size_t start = value.find_first_not_of(' ');
                    if (start != std::string::npos) {
                        value = value.substr(start);
                    }

                    // Convert key to lowercase (safe for unsigned char)
                    std::transform(key.begin(), key.end(), key.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                    resp.headers[key] = value;
                }
            }

            pos = lineEnd + 2;
        }
    }

    /**
     * @brief Build a raw HTTP request string
     * @param method HTTP method ("GET", "POST", "PUT", "DELETE", etc.)
     * @param path Request path (e.g. "/api/users")
     * @param body Request body (empty for bodyless methods)
     * @param contentType Content-Type value (empty if no body)
     * @return Complete HTTP request string ready to send over TCP
     *
     * Request format:
     * @code
     * METHOD PATH HTTP/1.1\r\n
     * Host: host_\r\n
     * Connection: keep-alive\r\n
     * Content-Type: contentType\r\n   (if body is non-empty)
     * Content-Length: body.size()\r\n  (if body is non-empty)
     * [default headers]\r\n
     * \r\n
     * body
     * @endcode
     */
    std::string buildRequest(const std::string& method, const std::string& path,
                             const std::string& body, const std::string& contentType) {
        std::string req;
        req.reserve(256 + body.size());

        // Request line
        req += method + " " + path + " HTTP/1.1\r\n";

        // Required headers
        req += "Host: " + host_ + "\r\n";
        req += "Connection: keep-alive\r\n";

        // Body-related headers
        if (!body.empty()) {
            if (!contentType.empty()) {
                req += "Content-Type: " + contentType + "\r\n";
            }
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }

        // Append user-configured default headers
        // (lock already held by caller or snapshot needed)
        std::unordered_map<std::string, std::string> hdrs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hdrs = defaultHeaders_;
        }
        for (const auto& [key, value] : hdrs) {
            req += key + ": " + value + "\r\n";
        }

        // End of headers
        req += "\r\n";

        // Body
        req += body;

        return req;
    }
};
