/**
 * @file ObjectPool.h
 * @brief 通用对象池 -- 预分配 + 自动回收，减少频繁 new/delete 开销
 *
 * 设计要点:
 * - 构造时预分配 initialSize 个对象，后续按需增长，总数不超过 maxSize
 * - acquire() 返回 std::unique_ptr<T, Deleter>，对象离开作用域时自动归还池中
 * - 可通过 setResetFunc() 注册回收重置函数，归还时自动清理对象状态
 * - 线程安全: 内部通过 std::mutex 保护空闲列表，可在多线程环境中使用
 *
 * 生命周期:
 * @verbatim
 *   ObjectPool 构造 -> 预分配 initialSize 个 T
 *       │
 *       v
 *   acquire() ─── 池中有空闲对象 ──> 取出并返回 Ptr
 *       │                             │
 *       │  池为空且未达上限 ──> new T  │
 *       │                             │
 *       │  池为空且已达上限 ──> nullptr │
 *       │                             v
 *       │                   Ptr 析构 / reset()
 *       │                             │
 *       │                   Deleter::operator()
 *       │                             │
 *       │                   resetFunc_(obj)  <-- 可选的状态重置
 *       │                             │
 *       └─────────── 归还到池中 <──────┘
 * @endverbatim
 *
 * @example 使用示例
 * @code
 * ObjectPool<Buffer> pool(16, 64);
 * pool.setResetFunc([](Buffer& buf) { buf.clear(); });
 *
 * {
 *     auto buf = pool.acquire();  // 从池中获取
 *     buf->append("hello");
 * }  // 离开作用域时自动归还
 * @endcode
 */

#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <functional>

/**
 * @class ObjectPool
 * @brief 通用线程安全对象池，基于预分配和自定义 Deleter 实现零开销自动回收
 * @tparam T 池化对象类型（需支持默认构造）
 *
 * 通过自定义 Deleter 将 unique_ptr 的析构行为从 delete 改为归还池中，
 * 使用者无需手动调用 release()，RAII 风格自动管理对象生命周期。
 */
template<typename T>
class ObjectPool {
public:
    /**
     * @struct Deleter
     * @brief 自定义删除器，使 unique_ptr 析构时将对象归还到池中而非 delete
     *
     * 当 pool 指针有效时，调用 pool->releaseRaw() 归还对象；
     * 当 pool 为 nullptr 时（池已满或池已销毁），直接 delete 对象。
     */
    struct Deleter {
        ObjectPool<T>* pool;       ///< 所属对象池指针，nullptr 表示无池关联
        Deleter() : pool(nullptr) {}
        /**
         * @brief 构造删除器
         * @param p 关联的对象池指针
         */
        explicit Deleter(ObjectPool<T>* p) : pool(p) {}
        /**
         * @brief 删除/归还操作
         * @param ptr 待处理的对象指针
         */
        void operator()(T* ptr) {
            if (pool) {
                pool->releaseRaw(ptr);
            } else {
                delete ptr;
            }
        }
    };

    /// 池化对象的智能指针类型，析构时自动归还池中
    using Ptr = std::unique_ptr<T, Deleter>;

    /**
     * @brief 构造对象池并预分配对象
     * @param initialSize 初始预分配对象数量
     * @param maxSize     最大允许对象数量（0 表示使用 initialSize * 2）
     *
     * 构造时立即 new 出 initialSize 个对象放入空闲列表。
     */
    explicit ObjectPool(size_t initialSize, size_t maxSize = 0)
        : maxSize_(maxSize == 0 ? initialSize * 2 : maxSize)
        , totalCreated_(0)
    {
        for (size_t i = 0; i < initialSize; ++i) {
            pool_.push_back(new T());
            totalCreated_++;
        }
    }

    /**
     * @brief 析构函数，释放池中所有空闲对象
     *
     * @note 已被 acquire() 取出且尚未归还的对象不在池中，
     *       它们会在 Ptr 析构时由 Deleter 处理（此时 pool 指针已失效，将直接 delete）。
     */
    ~ObjectPool() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* obj : pool_) { delete obj; }
    }

    /// 禁止拷贝
    ObjectPool(const ObjectPool&) = delete;
    /// 禁止拷贝赋值
    ObjectPool& operator=(const ObjectPool&) = delete;

    /**
     * @brief 从池中获取一个对象
     * @return 智能指针 Ptr，析构时自动归还；若池为空且已达上限则返回 nullptr
     *
     * 优先从空闲列表取出对象（O(1) 操作）；
     * 空闲列表为空但未达 maxSize_ 时动态创建新对象；
     * 已达上限时返回包含 nullptr 的 Ptr。
     */
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

    /**
     * @brief 显式归还对象到池中
     * @param obj 要归还的智能指针（调用后 obj 变为 nullptr）
     *
     * @note 通常不需要手动调用，Ptr 析构时会自动归还。
     *       此方法提供显式提前归还的能力。
     */
    void release(Ptr obj) {
        if (obj) obj.reset();
    }

    /**
     * @brief 设置对象回收重置函数
     * @param func 回收时调用的重置函数，用于清理对象内部状态
     *
     * 对象被归还到池中之前会调用此函数，确保下次 acquire() 得到干净的对象。
     * @code
     * pool.setResetFunc([](MyObj& obj) { obj.clear(); });
     * @endcode
     */
    void setResetFunc(std::function<void(T&)> func) {
        resetFunc_ = std::move(func);
    }

    /**
     * @brief 获取当前池中空闲对象数量
     * @return 空闲对象数量
     */
    size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pool_.size();
    }

private:
    /**
     * @brief 内部归还操作（由 Deleter 调用）
     * @param ptr 待归还的裸指针
     *
     * 流程: 先调用 resetFunc_ 重置状态，再放回空闲列表。
     */
    void releaseRaw(T* ptr) {
        if (!ptr) return;
        if (resetFunc_) resetFunc_(*ptr);
        std::lock_guard<std::mutex> lock(mutex_);
        pool_.push_back(ptr);
    }

    std::vector<T*> pool_;              ///< 空闲对象列表（栈式后进先出）
    size_t maxSize_;                     ///< 对象池最大容量上限
    size_t totalCreated_;                ///< 已创建的对象总数（含已取出的）
    mutable std::mutex mutex_;           ///< 保护空闲列表的互斥锁（mutable 以支持 const 方法）
    std::function<void(T&)> resetFunc_;  ///< 对象归还时的重置回调（可选）
};
