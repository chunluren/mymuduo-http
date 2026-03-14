// WsSession.h - WebSocket 会话管理
// 管理单个 WebSocket 连接的生命周期和状态

#pragma once

#include "WebSocketFrame.h"
#include "net/TcpConnection.h"
#include "net/Buffer.h"
#include <memory>
#include <functional>
#include <string>
#include <chrono>
#include <atomic>
#include <map>

class WsSession;
using WsSessionPtr = std::shared_ptr<WsSession>;

// WebSocket 会话状态
enum class WsState {
    Connecting,   // 连接中（HTTP 握手前）
    Open,         // 已打开（握手完成）
    Closing,      // 关闭中
    Closed        // 已关闭
};

// WebSocket 消息类型
struct WsMessage {
    WsOpcode opcode;
    std::vector<uint8_t> data;

    bool isText() const { return opcode == WsOpcode::Text; }
    bool isBinary() const { return opcode == WsOpcode::Binary; }
    bool isClose() const { return opcode == WsOpcode::Close; }
    bool isPing() const { return opcode == WsOpcode::Ping; }
    bool isPong() const { return opcode == WsOpcode::Pong; }

    std::string text() const {
        return std::string(data.begin(), data.end());
    }
};

// WebSocket 会话
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    using MessageHandler = std::function<void(const WsSessionPtr&, const WsMessage&)>;
    using CloseHandler = std::function<void(const WsSessionPtr&)>;
    using ErrorHandler = std::function<void(const WsSessionPtr&, const std::string&)>;

    WsSession(const TcpConnectionPtr& conn)
        : conn_(conn)
        , state_(WsState::Connecting)
        , lastActivityMs_(currentTimeMs())
    {}

    ~WsSession() = default;

    // 发送文本消息
    void sendText(const std::string& text) {
        if (state_ != WsState::Open) return;
        auto data = WebSocketFrameCodec::encodeText(text);
        sendRaw(data);
    }

    // 发送二进制消息
    void sendBinary(const std::vector<uint8_t>& data) {
        if (state_ != WsState::Open) return;
        auto encoded = WebSocketFrameCodec::encodeBinary(data);
        sendRaw(encoded);
    }

    // 发送 Ping
    void ping(const std::vector<uint8_t>& data = {}) {
        if (state_ != WsState::Open) return;
        auto encoded = WebSocketFrameCodec::encodePing(data);
        sendRaw(encoded);
    }

    // 发送 Pong
    void pong(const std::vector<uint8_t>& data = {}) {
        if (state_ != WsState::Open) return;
        auto encoded = WebSocketFrameCodec::encodePong(data);
        sendRaw(encoded);
    }

    // 关闭连接
    void close(uint16_t code = 1000, const std::string& reason = "") {
        if (state_ == WsState::Closed || state_ == WsState::Closing) {
            return;
        }

        state_ = WsState::Closing;

        // 发送关闭帧
        auto encoded = WebSocketFrameCodec::encodeClose(code, reason);
        sendRaw(encoded);

        // 延迟关闭连接
        if (conn_) {
            conn_->shutdown();
        }
    }

    // 强制关闭
    void forceClose() {
        if (state_ == WsState::Closed) return;
        state_ = WsState::Closed;
        if (conn_) {
            conn_->shutdown();
        }
    }

    // 获取状态
    WsState state() const { return state_; }
    bool isOpen() const { return state_ == WsState::Open; }
    bool isClosed() const { return state_ == WsState::Closed; }

    // 获取底层连接
    TcpConnectionPtr connection() const { return conn_; }

    // 获取客户端地址
    std::string clientAddress() const {
        if (conn_) {
            return conn_->peerAddress().toIpPort();
        }
        return "";
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

    // 设置上下文数据
    void setContext(const std::string& key, const std::string& value) {
        context_[key] = value;
    }

    // 获取上下文数据
    std::string getContext(const std::string& key, const std::string& defaultValue = "") const {
        auto it = context_.find(key);
        return it != context_.end() ? it->second : defaultValue;
    }

    // 更新活动时间
    void updateActivity() {
        lastActivityMs_ = currentTimeMs();
    }

    // 获取空闲时间（毫秒）
    int64_t idleTimeMs() const {
        return currentTimeMs() - lastActivityMs_;
    }

    // 内部方法：处理消息
    void handleMessage(const WsMessage& msg) {
        if (messageHandler_) {
            messageHandler_(shared_from_this(), msg);
        }
    }

    // 内部方法：处理关闭
    void handleClose() {
        if (state_ == WsState::Closed) return;
        state_ = WsState::Closed;
        if (closeHandler_) {
            closeHandler_(shared_from_this());
        }
    }

    // 内部方法：处理错误
    void handleError(const std::string& error) {
        if (errorHandler_) {
            errorHandler_(shared_from_this(), error);
        }
    }

    // 内部方法：设置状态
    void setState(WsState state) {
        state_ = state;
    }

private:
    void sendRaw(const std::vector<uint8_t>& data) {
        if (conn_ && conn_->connected()) {
            conn_->send(std::string(data.begin(), data.end()));
        }
    }

    static int64_t currentTimeMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }

    TcpConnectionPtr conn_;
    std::atomic<WsState> state_;
    int64_t lastActivityMs_;

    MessageHandler messageHandler_;
    CloseHandler closeHandler_;
    ErrorHandler errorHandler_;

    std::map<std::string, std::string> context_;
};