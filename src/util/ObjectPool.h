#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <functional>

template<typename T>
class ObjectPool {
public:
    struct Deleter {
        ObjectPool<T>* pool;
        Deleter() : pool(nullptr) {}
        explicit Deleter(ObjectPool<T>* p) : pool(p) {}
        void operator()(T* ptr) {
            if (pool) {
                pool->releaseRaw(ptr);
            } else {
                delete ptr;
            }
        }
    };

    using Ptr = std::unique_ptr<T, Deleter>;

    explicit ObjectPool(size_t initialSize, size_t maxSize = 0)
        : maxSize_(maxSize == 0 ? initialSize * 2 : maxSize)
        , totalCreated_(0)
    {
        for (size_t i = 0; i < initialSize; ++i) {
            pool_.push_back(new T());
            totalCreated_++;
        }
    }

    ~ObjectPool() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* obj : pool_) { delete obj; }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    Ptr acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pool_.empty()) {
            T* obj = pool_.back();
            pool_.pop_back();
            return Ptr(obj, Deleter(this));
        }
        if (totalCreated_ < maxSize_) {
            totalCreated_++;
            return Ptr(new T(), Deleter(this));
        }
        return Ptr(nullptr, Deleter(nullptr));
    }

    void release(Ptr obj) {
        if (obj) obj.reset();
    }

    void setResetFunc(std::function<void(T&)> func) {
        resetFunc_ = std::move(func);
    }

    size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pool_.size();
    }

private:
    void releaseRaw(T* ptr) {
        if (!ptr) return;
        if (resetFunc_) resetFunc_(*ptr);
        std::lock_guard<std::mutex> lock(mutex_);
        pool_.push_back(ptr);
    }

    std::vector<T*> pool_;
    size_t maxSize_;
    size_t totalCreated_;
    mutable std::mutex mutex_;
    std::function<void(T&)> resetFunc_;
};
