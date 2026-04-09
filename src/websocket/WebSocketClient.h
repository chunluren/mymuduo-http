/**
 * @file WebSocketClient.h
 * @brief Header-only Reactor-based WebSocket client
 *
 * Implements a WebSocket client built on top of TcpClient, following
 * RFC 6455. Handles the HTTP Upgrade handshake, frame codec, masking
 * (client -> server), and automatic reconnection with re-handshake.
 *
 * Supported features:
 * - Text and binary messages
 * - Ping/Pong heartbeat
 * - Close handshake with status code and reason
 * - Auto-reconnect with re-handshake (via enableRetry)
 * - Client-side frame masking (RFC 6455 requirement)
 *
 * @example Usage
 * @code
 * EventLoop loop;
 * InetAddress serverAddr("127.0.0.1", 8080);
 * WebSocketClient client(&loop, serverAddr, "MyWsClient", "/chat");
 *
 * client.setOpenCallback([]() {
 *     LOG_INFO("WebSocket connected");
 * });
 *
 * client.setMessageCallback([](const WsMessage& msg) {
 *     if (msg.isText()) {
 *         LOG_INFO("Received: %s", msg.text().c_str());
 *     }
 * });
 *
 * client.setCloseCallback([](uint16_t code, const std::string& reason) {
 *     LOG_INFO("Closed: %d %s", code, reason.c_str());
 * });
 *
 * client.connect();
 * loop.loop();
 * @endcode
 */

#pragma once

#include "net/TcpClient.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/Buffer.h"
#include "net/Callbacks.h"
#include "WebSocketFrame.h"
#include "WsSession.h"

#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <random>
#include <atomic>
#include <cstring>
#include <algorithm>

/**
 * @class WebSocketClient
 * @brief Reactor-based WebSocket client
 *
 * Wraps a TcpClient to provide a high-level WebSocket API.
 *
 * State machine:
 * @code
 *   Connecting  -->  Open  -->  Closing  -->  Closed
 *       ^                                        |
 *       +---- (retry enabled: auto-reconnect) ---+
 * @endcode
 *
 * Processing flow:
 * 1. TcpClient connects to the server
 * 2. On TCP connect, send HTTP Upgrade handshake
 * 3. Parse the server's 101 Switching Protocols response
 * 4. Transition to Open state, begin frame-based communication
 * 5. All client-to-server frames are masked per RFC 6455
 */
class WebSocketClient {
public:
    /// Callback fired when the WebSocket handshake completes successfully.
    using OpenCallback    = std::function<void()>;
    /// Callback fired when a text or binary message is received.
    using MessageCallback = std::function<void(const WsMessage&)>;
    /// Callback fired when a close frame is received or the connection closes.
    using CloseCallback   = std::function<void(uint16_t code, const std::string& reason)>;
    /// Callback fired on protocol or connection errors.
    using ErrorCallback   = std::function<void(const std::string& error)>;

    /**
     * @brief Construct a WebSocket client
     * @param loop   Event loop to run on
     * @param serverAddr  Server address (host:port)
     * @param name   Client name (for logging)
     * @param path   WebSocket endpoint path (e.g. "/ws" or "/chat")
     */
    WebSocketClient(EventLoop* loop,
                    const InetAddress& serverAddr,
                    const std::string& name,
                    const std::string& path = "/")
        : client_(loop, serverAddr, name)
        , serverAddr_(serverAddr)
        , path_(path)
        , state_(WsState::Closed)
    {
        // Build the Host header from the address
        host_ = serverAddr.toIpPort();

        client_.setConnectionCallback(
            [this](const TcpConnectionPtr& conn) {
                onConnection(conn);
            });

        client_.setMessageCallback(
            [this](const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
                onMessage(conn, buf, time);
            });
    }

    ~WebSocketClient() = default;

    // ---------------------------------------------------------------
    //  Connection control
    // ---------------------------------------------------------------

    /**
     * @brief Initiate the TCP connection (and subsequent WS handshake)
     *
     * The handshake is sent automatically once the TCP connection succeeds.
     */
    void connect() {
        state_ = WsState::Connecting;
        client_.connect();
    }

    /**
     * @brief Gracefully disconnect the WebSocket
     *
     * Sends a close frame (if open) and then tears down the TCP connection.
     */
    void disconnect() {
        if (state_ == WsState::Open) {
            close();
        }
        client_.disconnect();
    }

    /**
     * @brief Enable automatic reconnection
     *
     * When enabled, TcpClient will reconnect on disconnect, and the
     * WebSocket handshake will be re-performed automatically.
     */
    void enableRetry() {
        client_.enableRetry();
    }

    // ---------------------------------------------------------------
    //  Callback setters
    // ---------------------------------------------------------------

    /** @brief Set callback for successful handshake */
    void setOpenCallback(OpenCallback cb)       { openCallback_ = std::move(cb); }

    /** @brief Set callback for incoming messages */
    void setMessageCallback(MessageCallback cb) { messageCallback_ = std::move(cb); }

    /** @brief Set callback for connection close */
    void setCloseCallback(CloseCallback cb)     { closeCallback_ = std::move(cb); }

    /** @brief Set callback for errors */
    void setErrorCallback(ErrorCallback cb)     { errorCallback_ = std::move(cb); }

    // ---------------------------------------------------------------
    //  Sending
    // ---------------------------------------------------------------

    /**
     * @brief Send a text message
     * @param text  UTF-8 encoded text payload
     *
     * Frames are masked (client -> server) per RFC 6455.
     */
    void sendText(const std::string& text) {
        if (state_ != WsState::Open) return;
        WebSocketFrame frame;
        frame.fin = true;
        frame.opcode = WsOpcode::Text;
        frame.payload.assign(text.begin(), text.end());
        auto data = WebSocketFrameCodec::encode(frame, true);  // masked
        sendRaw(data);
    }

    /**
     * @brief Send a binary message
     * @param data  Binary payload
     */
    void sendBinary(const std::vector<uint8_t>& data) {
        if (state_ != WsState::Open) return;
        WebSocketFrame frame;
        frame.fin = true;
        frame.opcode = WsOpcode::Binary;
        frame.payload = data;
        auto encoded = WebSocketFrameCodec::encode(frame, true);  // masked
        sendRaw(encoded);
    }

    /**
     * @brief Send a Ping frame
     * @param data  Optional ping payload
     */
    void ping(const std::vector<uint8_t>& data = {}) {
        if (state_ != WsState::Open) return;
        WebSocketFrame frame;
        frame.fin = true;
        frame.opcode = WsOpcode::Ping;
        frame.payload = data;
        auto encoded = WebSocketFrameCodec::encode(frame, true);  // masked
        sendRaw(encoded);
    }

    /**
     * @brief Send a Close frame and begin graceful shutdown
     * @param code   Status code (default 1000 = normal closure)
     * @param reason Human-readable reason string
     */
    void close(uint16_t code = 1000, const std::string& reason = "") {
        if (state_ != WsState::Open) return;
        state_ = WsState::Closing;

        WebSocketFrame frame;
        frame.fin = true;
        frame.opcode = WsOpcode::Close;
        // Close frame payload: 2-byte status code + optional reason
        frame.payload.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
        frame.payload.push_back(static_cast<uint8_t>(code & 0xFF));
        frame.payload.insert(frame.payload.end(), reason.begin(), reason.end());
        auto encoded = WebSocketFrameCodec::encode(frame, true);  // masked
        sendRaw(encoded);
    }

    // ---------------------------------------------------------------
    //  State queries
    // ---------------------------------------------------------------

    /** @brief Get the current WebSocket state */
    WsState state() const { return state_; }

    /** @brief Check whether the connection is in the Open state */
    bool isOpen() const { return state_ == WsState::Open; }

private:
    // ---------------------------------------------------------------
    //  Base64 encoding (for the 16-byte handshake nonce)
    // ---------------------------------------------------------------

    /**
     * @brief Base64-encode a byte buffer
     * @param data  Input bytes
     * @param len   Number of bytes
     * @return Base64-encoded string
     */
    static std::string base64Encode(const uint8_t* data, size_t len) {
        static const char table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        result.reserve(((len + 2) / 3) * 4);

        for (size_t i = 0; i < len; i += 3) {
            uint32_t n = static_cast<uint32_t>(data[i]) << 16;
            if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
            if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);

            result += table[(n >> 18) & 0x3F];
            result += table[(n >> 12) & 0x3F];
            result += (i + 1 < len) ? table[(n >> 6) & 0x3F] : '=';
            result += (i + 2 < len) ? table[n & 0x3F] : '=';
        }

        return result;
    }

    /**
     * @brief Generate a random 16-byte nonce and return its Base64 encoding
     * @return Base64-encoded Sec-WebSocket-Key
     */
    static std::string generateWebSocketKey() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dist(0, 255);

        uint8_t nonce[16];
        for (int i = 0; i < 16; ++i) {
            nonce[i] = static_cast<uint8_t>(dist(gen));
        }
        return base64Encode(nonce, 16);
    }

    // ---------------------------------------------------------------
    //  TCP callbacks
    // ---------------------------------------------------------------

    /**
     * @brief Called when the TCP connection succeeds or is lost
     *
     * On connect: sends the HTTP Upgrade request.
     * On disconnect: transitions to Closed and fires the close callback.
     */
    void onConnection(const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            conn_ = conn;
            state_ = WsState::Connecting;
            recvBuffer_.clear();

            // Generate the handshake key
            pendingKey_ = generateWebSocketKey();

            // Build and send the HTTP Upgrade request
            std::string request;
            request += "GET " + path_ + " HTTP/1.1\r\n";
            request += "Host: " + host_ + "\r\n";
            request += "Upgrade: websocket\r\n";
            request += "Connection: Upgrade\r\n";
            request += "Sec-WebSocket-Key: " + pendingKey_ + "\r\n";
            request += "Sec-WebSocket-Version: 13\r\n";
            request += "\r\n";

            conn->send(request);
        } else {
            // TCP disconnected
            WsState prevState = state_;
            state_ = WsState::Closed;
            conn_.reset();

            if (prevState == WsState::Open || prevState == WsState::Closing) {
                if (closeCallback_) {
                    closeCallback_(1006, "TCP connection lost");
                }
            } else if (prevState == WsState::Connecting) {
                if (errorCallback_) {
                    errorCallback_("Connection failed during handshake");
                }
            }
            // If retry is enabled, TcpClient will auto-reconnect and
            // onConnection will fire again, triggering a new handshake.
        }
    }

    /**
     * @brief Called when data arrives on the TCP connection
     *
     * Routes to the appropriate handler based on the current state:
     * - Connecting: parse the HTTP 101 handshake response
     * - Open: decode WebSocket frames
     */
    void onMessage(const TcpConnectionPtr& /*conn*/, Buffer* buf, Timestamp /*time*/) {
        // Move all readable bytes into our internal buffer
        std::string incoming = buf->retrieveAllAsString();

        if (state_ == WsState::Connecting) {
            handleHandshakeResponse(incoming);
        } else if (state_ == WsState::Open) {
            handleWsFrames(incoming);
        }
        // In Closing/Closed states, ignore incoming data
    }

    // ---------------------------------------------------------------
    //  Handshake handling
    // ---------------------------------------------------------------

    /**
     * @brief Parse the server's HTTP 101 Switching Protocols response
     *
     * Validates:
     * - Status line is "HTTP/1.1 101 ..."
     * - Sec-WebSocket-Accept matches the expected value
     *
     * On success, transitions to Open and fires the open callback.
     * Any bytes remaining after the HTTP headers are fed into frame parsing.
     */
    void handleHandshakeResponse(const std::string& data) {
        recvBuffer_.insert(recvBuffer_.end(), data.begin(), data.end());

        // Look for the end of the HTTP headers
        std::string bufStr(recvBuffer_.begin(), recvBuffer_.end());
        size_t headerEnd = bufStr.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            // Headers not yet complete; wait for more data
            return;
        }

        std::string headerBlock = bufStr.substr(0, headerEnd);
        size_t bodyStart = headerEnd + 4;  // skip "\r\n\r\n"

        // --- Parse status line ---
        size_t firstLineEnd = headerBlock.find("\r\n");
        std::string statusLine = (firstLineEnd != std::string::npos)
                                     ? headerBlock.substr(0, firstLineEnd)
                                     : headerBlock;

        // Must start with "HTTP/1.1 101"
        if (statusLine.find("HTTP/1.1 101") == std::string::npos) {
            state_ = WsState::Closed;
            if (errorCallback_) {
                errorCallback_("Handshake failed: " + statusLine);
            }
            if (conn_) {
                conn_->shutdown();
            }
            return;
        }

        // --- Parse headers ---
        std::string headerSection = (firstLineEnd != std::string::npos)
                                        ? headerBlock.substr(firstLineEnd + 2)
                                        : "";
        std::string acceptValue;
        size_t pos = 0;
        while (pos < headerSection.size()) {
            size_t lineEnd = headerSection.find("\r\n", pos);
            std::string line = (lineEnd != std::string::npos)
                                   ? headerSection.substr(pos, lineEnd - pos)
                                   : headerSection.substr(pos);

            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                // Trim leading/trailing whitespace from value
                size_t vStart = value.find_first_not_of(" \t");
                if (vStart != std::string::npos) {
                    value = value.substr(vStart);
                }
                size_t vEnd = value.find_last_not_of(" \t");
                if (vEnd != std::string::npos) {
                    value = value.substr(0, vEnd + 1);
                }

                // Case-insensitive header name comparison
                std::string lowerKey = key;
                std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(),
                               [](unsigned char c) { return std::tolower(c); });

                if (lowerKey == "sec-websocket-accept") {
                    acceptValue = value;
                }
            }

            if (lineEnd == std::string::npos) break;
            pos = lineEnd + 2;
        }

        // --- Validate Sec-WebSocket-Accept ---
        std::string expectedAccept = WebSocketFrameCodec::computeAcceptKey(pendingKey_);
        if (acceptValue != expectedAccept) {
            state_ = WsState::Closed;
            if (errorCallback_) {
                errorCallback_("Handshake failed: invalid Sec-WebSocket-Accept");
            }
            if (conn_) {
                conn_->shutdown();
            }
            return;
        }

        // --- Handshake succeeded ---
        state_ = WsState::Open;
        if (openCallback_) {
            openCallback_();
        }

        // Any remaining bytes after the HTTP headers belong to frame data
        std::vector<uint8_t> remaining(recvBuffer_.begin() + bodyStart,
                                       recvBuffer_.end());
        recvBuffer_.clear();

        if (!remaining.empty()) {
            std::string leftover(remaining.begin(), remaining.end());
            handleWsFrames(leftover);
        }
    }

    // ---------------------------------------------------------------
    //  WebSocket frame handling
    // ---------------------------------------------------------------

    /**
     * @brief Decode and dispatch incoming WebSocket frames
     *
     * Handles:
     * - Text/Binary: wraps in WsMessage and fires messageCallback
     * - Ping: auto-responds with masked Pong
     * - Pong: silently ignored
     * - Close: responds with masked Close frame, transitions to Closed
     */
    void handleWsFrames(const std::string& data) {
        recvBuffer_.insert(recvBuffer_.end(), data.begin(), data.end());

        while (recvBuffer_.size() >= 2) {
            auto result = WebSocketFrameCodec::decode(
                recvBuffer_.data(), recvBuffer_.size());

            if (result.status == WebSocketFrameCodec::DecodeResult::Incomplete) {
                break;
            }

            if (result.status == WebSocketFrameCodec::DecodeResult::Error) {
                if (errorCallback_) {
                    errorCallback_("Frame decode error: " + result.error);
                }
                break;
            }

            // DecodeResult::Ok -- dispatch by opcode
            const WebSocketFrame& frame = result.frame;

            switch (frame.opcode) {
                case WsOpcode::Text:
                case WsOpcode::Binary: {
                    if (messageCallback_) {
                        WsMessage msg;
                        msg.opcode = frame.opcode;
                        msg.data = frame.payload;
                        messageCallback_(msg);
                    }
                    break;
                }

                case WsOpcode::Ping: {
                    // Auto-respond with a masked Pong carrying the same payload
                    WebSocketFrame pong;
                    pong.fin = true;
                    pong.opcode = WsOpcode::Pong;
                    pong.payload = frame.payload;
                    auto encoded = WebSocketFrameCodec::encode(pong, true);  // masked
                    sendRaw(encoded);
                    break;
                }

                case WsOpcode::Pong: {
                    // Silently ignored
                    break;
                }

                case WsOpcode::Close: {
                    uint16_t code = 1005;  // No Status Rcvd (default)
                    std::string reason;
                    if (frame.payload.size() >= 2) {
                        code = (static_cast<uint16_t>(frame.payload[0]) << 8)
                             | static_cast<uint16_t>(frame.payload[1]);
                        if (frame.payload.size() > 2) {
                            reason.assign(frame.payload.begin() + 2,
                                          frame.payload.end());
                        }
                    }

                    // Send a masked Close frame back if we haven't already
                    if (state_ == WsState::Open) {
                        WebSocketFrame closeFrame;
                        closeFrame.fin = true;
                        closeFrame.opcode = WsOpcode::Close;
                        closeFrame.payload = frame.payload;
                        auto encoded = WebSocketFrameCodec::encode(closeFrame, true);
                        sendRaw(encoded);
                    }

                    state_ = WsState::Closed;
                    if (closeCallback_) {
                        closeCallback_(code, reason);
                    }
                    break;
                }

                default:
                    break;
            }

            // Remove consumed bytes from the receive buffer
            recvBuffer_.erase(recvBuffer_.begin(),
                              recvBuffer_.begin() + result.consumed);
        }
    }

    // ---------------------------------------------------------------
    //  Low-level send
    // ---------------------------------------------------------------

    /**
     * @brief Send raw bytes over the TCP connection
     * @param data  Encoded frame bytes to send
     */
    void sendRaw(const std::vector<uint8_t>& data) {
        if (conn_ && conn_->connected()) {
            conn_->send(std::string(data.begin(), data.end()));
        }
    }

    // ---------------------------------------------------------------
    //  Member variables
    // ---------------------------------------------------------------

    TcpClient client_;                   ///< Underlying TCP client
    InetAddress serverAddr_;             ///< Server address
    std::string path_;                   ///< WebSocket endpoint path
    std::string host_;                   ///< Host header value
    TcpConnectionPtr conn_;              ///< Current TCP connection

    std::atomic<WsState> state_;         ///< Current WebSocket state
    std::string pendingKey_;             ///< Sec-WebSocket-Key awaiting validation
    std::vector<uint8_t> recvBuffer_;    ///< Accumulates incoming bytes

    OpenCallback    openCallback_;       ///< Handshake-complete callback
    MessageCallback messageCallback_;    ///< Message-received callback
    CloseCallback   closeCallback_;      ///< Connection-closed callback
    ErrorCallback   errorCallback_;      ///< Error callback
};
