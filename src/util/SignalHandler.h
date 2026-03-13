// SignalHandler.h - 信号处理
#pragma once

#include <signal.h>
#include <functional>
#include <unordered_map>
#include <vector>

// 信号处理器
class SignalHandler {
public:
    using SignalCallback = std::function<void(int)>;
    
    static SignalHandler& instance() {
        static SignalHandler handler;
        return handler;
    }
    
    // 注册信号回调
    void registerHandler(int signum, SignalCallback cb) {
        callbacks_[signum].push_back(std::move(cb));
    }
    
    // 忽略信号
    void ignore(int signum) {
        signal(signum, SIG_IGN);
    }
    
    // 启动信号处理
    void start() {
        // 注册所有信号
        struct sigaction sa;
        sa.sa_handler = &SignalHandler::signalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        
        for (const auto& [signum, _] : callbacks_) {
            sigaction(signum, &sa, nullptr);
        }
    }
    
    // 优雅退出辅助函数
    static void setupGracefulExit(std::function<void()> onExit) {
        auto& handler = instance();
        handler.registerHandler(SIGINT, [onExit](int) {
            onExit();
        });
        handler.registerHandler(SIGTERM, [onExit](int) {
            onExit();
        });
        handler.start();
    }

private:
    SignalHandler() = default;
    
    static void signalHandler(int signum) {
        auto& handler = instance();
        auto it = handler.callbacks_.find(signum);
        if (it != handler.callbacks_.end()) {
            for (const auto& cb : it->second) {
                cb(signum);
            }
        }
    }
    
    std::unordered_map<int, std::vector<SignalCallback>> callbacks_;
};

// 常用信号设置
class Signals {
public:
    // 忽略 SIGPIPE（防止写入已关闭的 socket 导致进程退出）
    static void ignorePipe() {
        SignalHandler::instance().ignore(SIGPIPE);
    }
    
    // 设置优雅退出
    static void gracefulExit(std::function<void()> onExit) {
        SignalHandler::setupGracefulExit(onExit);
    }
};