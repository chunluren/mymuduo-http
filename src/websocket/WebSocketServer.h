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
    bool handleHandshake(const TcpConnectionPtr& conn, const WsSessionPtr& session, Buffer* buf);

    /// 处理 WebSocket 帧
    void handleWsFrames(const WsSessionPtr& session, Buffer* buf);

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
    std::map<std::string, std::string> parseHeaders(const std::string& header);

    std::string getHeader(const std::map<std::string, std::string>& headers, const std::string& key);
    std::string strToLower(const std::string& s);
    std::string trim(const std::string& s);

    void sendBadRequest(const TcpConnectionPtr& conn, const std::string& msg = "Bad Request");
    void sendForbidden(const TcpConnectionPtr& conn);

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
    std::unordered_map<std::string, WsSessionPtr> connSessions_;
};