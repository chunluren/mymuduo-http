#pragma once

#include <cstdint>

/**
 * @class TimerId
 * @brief 定时器标识符，用于取消定时器
 *
 * 由 EventLoop::runAfter() / runEvery() 返回，
 * 传给 EventLoop::cancel() 可以取消定时器。
 */
class TimerId {
public:
    TimerId() : id_(-1) {}
    explicit TimerId(int64_t id) : id_(id) {}

    int64_t id() const { return id_; }
    bool valid() const { return id_ >= 0; }

private:
    int64_t id_;
};
