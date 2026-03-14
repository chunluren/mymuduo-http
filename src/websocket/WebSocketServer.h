// WebSocketServer.h - WebSocket 服务器
// 提供完整的 WebSocket 服务端实现

#pragma once

#include "WebSocketFrame.h"
#include "WsSession.h"
#include "net/TcpServer.h"
#include "net/EventLoop.h"
#include "net/Buffer.h"
#include "net/InetAddress.h"
#include <memory>
#include <functional>
#include <unordered_map>
#include <string>
#include <mutex>
#include <regex>

// WebSocket 配置
struct WebSocketConfig {
    int maxMessageSize = 10 * 1024 * 1024;  // 最大消息大小 10MB
    int idleTimeoutMs = 60000;               // 空闲超时 60秒
    bool enablePingPong = true;              // 启用 Ping/Pong
    int pingIntervalMs = 30000;              // Ping 间隔 30秒
};

// WebSocket 服务器
class WebSocketServer {
public:
    using ConnectionHandler = std::function<void(const WsSessionPtr&)>;
    using MessageHandler = std::function<void(const WsSessionPtr&, const WsMessage&)>;
    using CloseHandler = std::function<void(const WsSessionPtr&)>;
    using ErrorHandler = std::function<void(const WsSessionPtr&, const std::string&)>;

    WebSocketServer(EventLoop* loop, const InetAddress& addr, const std::string& name = "WebSocketServer")
        : server_(loop, addr, name)
        , config_()
        , started_(false)
    {
        server_.setConnectionCallback([this](const TcpConnectionPtr& conn) {
            onConnection(conn);
        });
        server_.setMessageCallback([this](const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
            onMessage(conn, buf, time);
        });
        server_.setThreadNum(4);
    }

    ~WebSocketServer() = default;

    void setThreadNum(int num) { server_.setThreadNum(num); }

    void start() {
        if (started_.exchange(true)) {
            return;
        }
        server_.start();
    }

    // 设置配置
    void setConfig(const WebSocketConfig& config) {
        if (started_) return;
        config_ = config;
    }

    // 设置新连接处理器
    void setConnectionHandler(ConnectionHandler handler) {
        connectionHandler_ = std::move(handler);
    }

    // 设置消息处理器
    void setMessageHandler(MessageHandler handler) {
        messageHandler_ = std::move(handler);
    }

    // 设置关闭处理器
    void setCloseHandler(CloseHandler handler) {
        closeHandler_ = std::move(handler);
    }

    // 设置错误处理器
    void setErrorHandler(ErrorHandler handler) {
        errorHandler_ = std::move(handler);
    }

    // 设置握手验证器（返回 true 允许连接）
    using HandshakeValidator = std::function<bool(const TcpConnectionPtr&, const std::string& path,
                                                   const std::map<std::string, std::string>& headers)>;
    void setHandshakeValidator(HandshakeValidator validator) {
        handshakeValidator_ = std::move(validator);
    }

    // 广播消息给所有会话
    void broadcast(const std::string& message) {
        auto sessions = getAllSessions();
        for (auto& session : sessions) {
            session->sendText(message);
        }
    }

    void broadcastBinary(const std::vector<uint8_t>& data) {
        auto sessions = getAllSessions();
        for (auto& session : sessions) {
            session->sendBinary(data);
        }
    }

    // 获取所有会话
    std::vector<WsSessionPtr> getAllSessions() const {
        std::vector<WsSessionPtr> sessions;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, session] : sessions_) {
            if (session->isOpen()) {
                sessions.push_back(session);
            }
        }
        return sessions;
    }

    // 获取会话数量
    size_t sessionCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.size();
    }

private:
    void onConnection(const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            // 创建新会话
            auto session = std::make_shared<WsSession>(conn);

            // 设置处理器
            session->setMessageHandler([this](const WsSessionPtr& s, const WsMessage& msg) {
                handleWsMessage(s, msg);
            });
            session->setCloseHandler([this](const WsSessionPtr& s) {
                handleWsClose(s);
            });
            session->setErrorHandler([this](const WsSessionPtr& s, const std::string& err) {
                handleWsError(s, err);
            });

            // 保存会话
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sessions_[conn->name()] = session;
                connSessions_[conn->name()] = session;
            }
        } else {
            // 移除会话
            WsSessionPtr session;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = sessions_.find(conn->name());
                if (it != sessions_.end()) {
                    session = it->second;
                    sessions_.erase(it);
                }
                connSessions_.erase(conn->name());
            }

            if (session) {
                session->handleClose();
            }
        }
    }

    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp /*time*/) {
        WsSessionPtr session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = connSessions_.find(conn->name());
            if (it != connSessions_.end()) {
                session = it->second;
            }
        }

        if (!session) {
            conn->shutdown();
            return;
        }

        // 检查是否已完成握手
        if (session->state() == WsState::Connecting) {
            // 处理 HTTP 握手
            if (!handleHandshake(conn, session, buf)) {
                return;
            }
        } else if (session->state() == WsState::Open) {
            // 处理 WebSocket 帧
            handleWsFrames(session, buf);
        }
    }

    // 处理握手
    bool handleHandshake(const TcpConnectionPtr& conn, const WsSessionPtr& session, Buffer* buf) {
        // 查找 HTTP 头结束
        const char* data = buf->peek();
        size_t len = buf->readableBytes();

        const char* headerEnd = static_cast<const char*>(
            memmem(data, len, "\r\n\r\n", 4));

        if (!headerEnd) {
            // 请求头不完整
            if (len > 8192) {
                // 请求头太大
                sendBadRequest(conn);
                return false;
            }
            return true;  // 等待更多数据
        }

        // 解析 HTTP 请求
        std::string header(data, headerEnd - data);
        buf->retrieve(headerEnd - data + 4);  // 消费掉 HTTP 头

        // 解析请求行
        std::istringstream iss(header);
        std::string method, path, version;
        iss >> method >> path >> version;

        if (method != "GET") {
            sendBadRequest(conn, "Method not allowed");
            return false;
        }

        // 检查 Upgrade 和 Connection 头
        std::map<std::string, std::string> headers = parseHeaders(header);

        std::string upgrade = getHeader(headers, "upgrade");
        std::string connection = getHeader(headers, "connection");
        std::string wsKey = getHeader(headers, "sec-websocket-key");
        std::string wsVersion = getHeader(headers, "sec-websocket-version");

        if (upgrade.empty() || connection.empty() ||
            strToLower(upgrade) != "websocket" ||
            connection.find("Upgrade") == std::string::npos) {
            sendBadRequest(conn, "Invalid WebSocket handshake");
            return false;
        }

        if (wsKey.empty()) {
            sendBadRequest(conn, "Missing Sec-WebSocket-Key");
            return false;
        }

        if (wsVersion != "13") {
            sendBadRequest(conn, "Unsupported WebSocket version");
            return false;
        }

        // 调用握手验证器
        if (handshakeValidator_ && !handshakeValidator_(conn, path, headers)) {
            sendForbidden(conn);
            return false;
        }

        // 发送握手响应
        std::string acceptKey = WebSocketFrameCodec::computeAcceptKey(wsKey);
        std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + acceptKey + "\r\n"
            "\r\n";
        conn->send(response);

        // 更新会话状态
        session->setState(WsState::Open);
        session->updateActivity();

        // 保存路径到会话上下文
        session->setContext("path", path);

        // 调用连接处理器
        if (connectionHandler_) {
            connectionHandler_(session);
        }

        return true;
    }

    // 处理 WebSocket 帧
    void handleWsFrames(const WsSessionPtr& session, Buffer* buf) {
        while (buf->readableBytes() > 0) {
            const uint8_t* data = reinterpret_cast<const uint8_t*>(buf->peek());
            size_t len = buf->readableBytes();

            auto result = WebSocketFrameCodec::decode(data, len);

            if (result.status == WebSocketFrameCodec::DecodeResult::Incomplete) {
                // 数据不完整，等待更多数据
                return;
            } else if (result.status == WebSocketFrameCodec::DecodeResult::Error) {
                // 解析错误，关闭连接
                session->handleError("Frame decode error: " + result.error);
                session->close(1002, "Protocol error");
                return;
            }

            // 消费已处理的数据
            buf->retrieve(result.consumed);

            // 更新活动时间
            session->updateActivity();

            // 处理帧
            const WebSocketFrame& frame = result.frame;

            // RFC 6455: 客户端发送的帧必须设置 mask
            if (!frame.mask) {
                session->close(1002, "Protocol error: unmasked client frame");
                return;
            }

            // 检查消息大小
            if (frame.payloadSize() > static_cast<size_t>(config_.maxMessageSize)) {
                session->handleError("Message too large");
                session->close(1009, "Message too large");
                return;
            }

            // 根据操作码处理
            switch (frame.opcode) {
                case WsOpcode::Text:
                case WsOpcode::Binary: {
                    WsMessage msg;
                    msg.opcode = frame.opcode;
                    msg.data = frame.payload;
                    session->handleMessage(msg);
                    break;
                }
                case WsOpcode::Ping: {
                    // 自动回复 Pong
                    session->pong(frame.payload);
                    break;
                }
                case WsOpcode::Pong: {
                    // Pong 收到，更新活动时间
                    break;
                }
                case WsOpcode::Close: {
                    // 收到关闭帧
                    uint16_t code = 1000;
                    if (frame.payloadSize() >= 2) {
                        code = (static_cast<uint16_t>(frame.payload[0]) << 8) |
                               static_cast<uint16_t>(frame.payload[1]);
                    }
                    session->close(code);
                    return;
                }
                case WsOpcode::Continuation: {
                    // 分片消息，暂不支持
                    session->close(1003, "Continuation frames not supported");
                    return;
                }
                default: {
                    session->close(1002, "Invalid opcode");
                    return;
                }
            }
        }
    }

    void handleWsMessage(const WsSessionPtr& session, const WsMessage& msg) {
        if (messageHandler_) {
            messageHandler_(session, msg);
        }
    }

    void handleWsClose(const WsSessionPtr& session) {
        if (closeHandler_) {
            closeHandler_(session);
        }
    }

    void handleWsError(const WsSessionPtr& session, const std::string& error) {
        if (errorHandler_) {
            errorHandler_(session, error);
        }
    }

    // 解析 HTTP 头
    std::map<std::string, std::string> parseHeaders(const std::string& header) {
        std::map<std::string, std::string> headers;
        std::istringstream iss(header);
        std::string line;

        // 跳过请求行
        std::getline(iss, line);

        while (std::getline(iss, line)) {
            // 移除 \r
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 1);

                // 去除前后空格
                key = trim(key);
                value = trim(value);

                headers[strToLower(key)] = value;
            }
        }

        return headers;
    }

    std::string getHeader(const std::map<std::string, std::string>& headers,
                          const std::string& key) {
        auto it = headers.find(strToLower(key));
        return it != headers.end() ? it->second : "";
    }

    std::string strToLower(const std::string& s) {
        std::string result = s;
        for (char& c : result) {
            c = std::tolower(static_cast<unsigned char>(c));
        }
        return result;
    }

    std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    void sendBadRequest(const TcpConnectionPtr& conn, const std::string& msg = "Bad Request") {
        std::string response = "HTTP/1.1 400 " + msg + "\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: " + std::to_string(msg.size()) + "\r\n"
                              "Connection: close\r\n"
                              "\r\n" + msg;
        conn->send(response);
        conn->shutdown();
    }

    void sendForbidden(const TcpConnectionPtr& conn) {
        std::string response = "HTTP/1.1 403 Forbidden\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: 9\r\n"
                              "Connection: close\r\n"
                              "\r\nForbidden";
        conn->send(response);
        conn->shutdown();
    }

private:
    TcpServer server_;
    WebSocketConfig config_;
    std::atomic<bool> started_;

    ConnectionHandler connectionHandler_;
    MessageHandler messageHandler_;
    CloseHandler closeHandler_;
    ErrorHandler errorHandler_;
    HandshakeValidator handshakeValidator_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, WsSessionPtr> sessions_;
    std::unordered_map<std::string, WsSessionPtr> connSessions_;  // 连接名到会话的映射
};