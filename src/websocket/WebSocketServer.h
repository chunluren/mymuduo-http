/**
 * @file WebSocketServer.h
 * @brief WebSocket 服务器
 *
 * 本文件定义了 WebSocketServer 类，提供完整的 WebSocket 服务端实现。
 * 支持:
 * - RFC 6455 WebSocket 协议
 * - 文本和二进制消息
 * - Ping/Pong 心跳
 * - 自定义握手验证
 * - 广播消息
 *
 * @example 使用示例
 * @code
 * EventLoop loop;
 * InetAddress addr(8080);
 * WebSocketServer server(&loop, addr, "WsServer");
 *
 * // 设置消息处理回调
 * server.setMessageHandler([](const WsSessionPtr& session, const WsMessage& msg) {
 *     if (msg.opcode == WsOpcode::Text) {
 *         session->sendText("Echo: " + msg.data);
 *     }
 * });
 *
 * // 设置连接回调
 * server.setConnectionHandler([](const WsSessionPtr& session) {
 *     LOG_INFO << "New WebSocket connection";
 * });
 *
 * server.setThreadNum(4);
 * server.start();
 * loop.loop();
 * @endcode
 */

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
#include <algorithm>
#include <map>

/**
 * @struct WebSocketConfig
 * @brief WebSocket 配置
 */
struct WebSocketConfig {
    int maxMessageSize = 10 * 1024 * 1024;  ///< 最大消息大小 (10MB)
    int idleTimeoutMs = 60000;               ///< 空闲超时 (60秒)
    bool enablePingPong = true;              ///< 启用 Ping/Pong
    int pingIntervalMs = 30000;              ///< Ping 间隔 (30秒)
};

/**
 * @class WebSocketServer
 * @brief WebSocket 服务器
 *
 * 基于 TcpServer 实现的 WebSocket 服务器。
 *
 * 处理流程:
 * 1. 接收 HTTP 握手请求
 * 2. 验证并响应握手
 * 3. 接收并解析 WebSocket 帧
 * 4. 调用用户注册的回调处理消息
 */
class WebSocketServer {
public:
    /// 连接回调类型
    using ConnectionHandler = std::function<void(const WsSessionPtr&)>;
    /// 消息回调类型
    using MessageHandler = std::function<void(const WsSessionPtr&, const WsMessage&)>;
    /// 关闭回调类型
    using CloseHandler = std::function<void(const WsSessionPtr&)>;
    /// 错误回调类型
    using ErrorHandler = std::function<void(const WsSessionPtr&, const std::string&)>;

    /**
     * @brief 构造 WebSocket 服务器
     * @param loop 事件循环
     * @param addr 监听地址
     * @param name 服务器名称
     */
    WebSocketServer(EventLoop* loop, const InetAddress& addr, const std::string& name = "WebSocketServer")
        : server_(loop, addr, name)
        , loop_(loop)
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
        if (started_.exchange(true)) return;
        server_.start();

        // Ping timer: periodically send Ping to all open sessions
        if (config_.enablePingPong && config_.pingIntervalMs > 0) {
            loop_->runEvery(config_.pingIntervalMs / 1000.0, [this]() {
                auto sessions = getAllSessions();
                for (auto& session : sessions) {
                    session->ping();
                }
            });
        }

        // Idle timeout timer: check at twice the timeout frequency, close idle sessions
        if (config_.idleTimeoutMs > 0) {
            loop_->runEvery(config_.idleTimeoutMs / 2000.0, [this]() {
                auto sessions = getAllSessions();
                for (auto& session : sessions) {
                    if (session->idleTimeMs() > config_.idleTimeoutMs) {
                        session->close(1000, "Idle timeout");
                    }
                }
            });
        }
    }

    /// 设置配置
    void setConfig(const WebSocketConfig& config) {
        if (started_) return;
        config_ = config;
    }

    /// 设置连接回调
    void setConnectionHandler(ConnectionHandler handler) {
        connectionHandler_ = std::move(handler);
    }

    /// 设置消息回调
    void setMessageHandler(MessageHandler handler) {
        messageHandler_ = std::move(handler);
    }

    /// 设置关闭回调
    void setCloseHandler(CloseHandler handler) {
        closeHandler_ = std::move(handler);
    }

    /// 设置错误回调
    void setErrorHandler(ErrorHandler handler) {
        errorHandler_ = std::move(handler);
    }

    /// 握手验证器类型
    using HandshakeValidator = std::function<bool(const TcpConnectionPtr&, const std::string& path,
                                                   const std::map<std::string, std::string>& headers)>;

    /// 设置握手验证器
    void setHandshakeValidator(HandshakeValidator validator) {
        handshakeValidator_ = std::move(validator);
    }

    /// 广播文本消息给所有会话
    void broadcast(const std::string& message) {
        auto sessions = getAllSessions();
        for (auto& session : sessions) {
            session->sendText(message);
        }
    }

    /// 广播二进制消息
    void broadcastBinary(const std::vector<uint8_t>& data) {
        auto sessions = getAllSessions();
        for (auto& session : sessions) {
            session->sendBinary(data);
        }
    }

    /// 获取所有会话
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

    /// 获取会话数量
    size_t sessionCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.size();
    }

private:
    /// 连接回调处理
    void onConnection(const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            // 创建新会话
            auto session = std::make_shared<WsSession>(conn);

            session->setMessageHandler([this](const WsSessionPtr& s, const WsMessage& msg) {
                handleWsMessage(s, msg);
            });
            session->setCloseHandler([this](const WsSessionPtr& s) {
                handleWsClose(s);
            });
            session->setErrorHandler([this](const WsSessionPtr& s, const std::string& err) {
                handleWsError(s, err);
            });

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

    /// 消息回调处理
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
            handleHandshake(conn, session, buf);
        } else if (session->state() == WsState::Open) {
            handleWsFrames(session, buf);
        }
    }

    /// 处理 HTTP 握手
    bool handleHandshake(const TcpConnectionPtr& conn, const WsSessionPtr& session, Buffer* buf) {
        // 需要完整的 HTTP 请求头（以 \r\n\r\n 结尾）
        std::string request(buf->peek(), buf->readableBytes());
        auto headerEnd = request.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            return false; // 数据不完整，等待更多数据
        }

        // 消费已读取的数据
        buf->retrieve(headerEnd + 4);

        // 解析请求行
        auto firstLineEnd = request.find("\r\n");
        if (firstLineEnd == std::string::npos) {
            sendBadRequest(conn, "Invalid HTTP request");
            return false;
        }

        std::string requestLine = request.substr(0, firstLineEnd);

        // 解析请求路径
        auto spacePos1 = requestLine.find(' ');
        auto spacePos2 = requestLine.find(' ', spacePos1 + 1);
        if (spacePos1 == std::string::npos || spacePos2 == std::string::npos) {
            sendBadRequest(conn, "Invalid request line");
            return false;
        }
        std::string path = requestLine.substr(spacePos1 + 1, spacePos2 - spacePos1 - 1);

        // 解析 HTTP 头
        std::string headerSection = request.substr(firstLineEnd + 2, headerEnd - firstLineEnd - 2);
        auto headers = parseHeaders(headerSection);

        // 验证 WebSocket 升级请求
        std::string upgrade = getHeader(headers, "Upgrade");
        std::string connection = getHeader(headers, "Connection");
        std::string wsKey = getHeader(headers, "Sec-WebSocket-Key");

        if (strToLower(upgrade) != "websocket") {
            sendBadRequest(conn, "Missing or invalid Upgrade header");
            return false;
        }

        if (strToLower(connection).find("upgrade") == std::string::npos) {
            sendBadRequest(conn, "Missing or invalid Connection header");
            return false;
        }

        if (wsKey.empty()) {
            sendBadRequest(conn, "Missing Sec-WebSocket-Key header");
            return false;
        }

        // 自定义握手验证
        if (handshakeValidator_ && !handshakeValidator_(conn, path, headers)) {
            sendForbidden(conn);
            return false;
        }

        // 计算 Accept Key
        std::string acceptKey = WebSocketFrameCodec::computeAcceptKey(wsKey);

        // 发送 101 响应
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
        session->setContext("path", path);

        // 通知连接建立
        if (connectionHandler_) {
            connectionHandler_(session);
        }

        return true;
    }

    /// 处理 WebSocket 帧
    void handleWsFrames(const WsSessionPtr& session, Buffer* buf) {
        while (buf->readableBytes() > 0) {
            auto result = WebSocketFrameCodec::decode(
                reinterpret_cast<const uint8_t*>(buf->peek()),
                buf->readableBytes()
            );

            if (result.status == WebSocketFrameCodec::DecodeResult::Incomplete) {
                break; // 等待更多数据
            }

            if (result.status == WebSocketFrameCodec::DecodeResult::Error) {
                session->handleError("Frame decode error: " + result.error);
                session->forceClose();
                return;
            }

            // 消费已解码的字节
            buf->retrieve(result.consumed);
            session->updateActivity();

            const auto& frame = result.frame;

            // 检查消息大小限制
            if (config_.maxMessageSize > 0 &&
                static_cast<int>(frame.payload.size()) > config_.maxMessageSize) {
                session->handleError("Message too large");
                session->close(1009, "Message too large");
                return;
            }

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
                    session->pong(frame.payload);
                    break;
                }
                case WsOpcode::Pong: {
                    // Pong 收到，无需处理
                    break;
                }
                case WsOpcode::Close: {
                    session->close();
                    break;
                }
                default:
                    break;
            }
        }
    }

    void handleWsMessage(const WsSessionPtr& session, const WsMessage& msg) {
        if (messageHandler_) messageHandler_(session, msg);
    }

    void handleWsClose(const WsSessionPtr& session) {
        if (closeHandler_) closeHandler_(session);
    }

    void handleWsError(const WsSessionPtr& session, const std::string& error) {
        if (errorHandler_) errorHandler_(session, error);
    }

    /// 解析 HTTP 头
    std::map<std::string, std::string> parseHeaders(const std::string& header) {
        std::map<std::string, std::string> headers;
        std::string::size_type pos = 0;
        while (pos < header.size()) {
            auto lineEnd = header.find("\r\n", pos);
            if (lineEnd == std::string::npos) {
                lineEnd = header.size();
            }
            std::string line = header.substr(pos, lineEnd - pos);
            pos = lineEnd + 2;

            auto colonPos = line.find(':');
            if (colonPos != std::string::npos) {
                std::string key = trim(line.substr(0, colonPos));
                std::string value = trim(line.substr(colonPos + 1));
                headers[key] = value;
            }
        }
        return headers;
    }

    std::string getHeader(const std::map<std::string, std::string>& headers, const std::string& key) {
        // Case-insensitive lookup
        std::string lowerKey = strToLower(key);
        for (const auto& [k, v] : headers) {
            if (strToLower(k) == lowerKey) {
                return v;
            }
        }
        return "";
    }

    std::string strToLower(const std::string& s) {
        std::string result = s;
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return result;
    }

    std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    void sendBadRequest(const TcpConnectionPtr& conn, const std::string& msg = "Bad Request") {
        std::string response =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: " + std::to_string(msg.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + msg;
        conn->send(response);
        conn->shutdown();
    }

    void sendForbidden(const TcpConnectionPtr& conn) {
        std::string body = "Forbidden";
        std::string response =
            "HTTP/1.1 403 Forbidden\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + body;
        conn->send(response);
        conn->shutdown();
    }

private:
    TcpServer server_;
    EventLoop* loop_;
    WebSocketConfig config_;
    std::atomic<bool> started_;

    ConnectionHandler connectionHandler_;
    MessageHandler messageHandler_;
    CloseHandler closeHandler_;
    ErrorHandler errorHandler_;
    HandshakeValidator handshakeValidator_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, WsSessionPtr> sessions_;
    std::unordered_map<std::string, WsSessionPtr> connSessions_;
};