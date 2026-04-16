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
 *
 * 生命周期安全设计（防止 UAF）：
 * 内部状态 PoolCore 由 shared_ptr 管理，Deleter 持有 weak_ptr。
 * 即使 ObjectPool 在 Ptr 之前析构，Deleter 通过 weak_ptr::lock() 检测
 * 池是否存活：存活则归还，已销毁则直接 delete 对象。
 */
template<typename T>
class ObjectPool {
public:
    /**
     * @struct PoolCore
     * @brief 池内部共享状态，由 shared_ptr 管理生命周期
     *
     * 将数据从 ObjectPool 剥离到 PoolCore，使得即使 ObjectPool 析构后，
     * 仍未归还的 Ptr 持有的 weak_ptr<PoolCore> 可以安全检测池是否存活。
     */
    struct PoolCore {
        std::vector<T*> pool;               ///< 空闲对象列表
        std::mutex mutex;                   ///< 保护空闲列表
        size_t maxSize = 0;                 ///< 最大对象数
        size_t totalCreated = 0;            ///< 已创建对象总数
        std::function<void(T&)> resetFunc;  ///< 归还时的重置回调

        ~PoolCore() {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto* obj : pool) delete obj;
        }
    };

    /**
     * @struct Deleter
     * @brief 自定义删除器，持有 weak_ptr 避免 UAF
     *
     * Ptr 析构时：若 weak_ptr::lock() 成功（池仍存活），归还到池；
     * 否则（池已销毁）直接 delete 对象。
     */
    struct Deleter {
        std::weak_ptr<PoolCore> coreWeak;

        Deleter() = default;
        explicit Deleter(std::shared_ptr<PoolCore> c) : coreWeak(c) {}

        void operator()(T* ptr) {
            if (!ptr) return;
            if (auto core = coreWeak.lock()) {
                // Pool 仍存活：重置 + 归还
                if (core->resetFunc) core->resetFunc(*ptr);
                std::lock_guard<std::mutex> lock(core->mutex);
                core->pool.push_back(ptr);
            } else {
                // Pool 已销毁：直接 delete（避免 UAF）
                delete ptr;
            }
        }
    };

    /// 池化对象的智能指针类型，析构时自动归还池中（若池仍存活）
    using Ptr = std::unique_ptr<T, Deleter>;

    /**
     * @brief 构造对象池并预分配对象
     * @param initialSize 初始预分配对象数量
     * @param maxSize     最大允许对象数量（0 表示使用 initialSize * 2）
     */
    explicit ObjectPool(size_t initialSize, size_t maxSize = 0)
        : core_(std::make_shared<PoolCore>())
    {
        core_->maxSize = (maxSize == 0 ? initialSize * 2 : maxSize);
        for (size_t i = 0; i < initialSize; ++i) {
            core_->pool.push_back(new T());
            core_->totalCreated++;
        }
    }

    /// 禁止拷贝
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    /**
     * @brief 从池中获取一个对象
     * @return 智能指针 Ptr，析构时自动归还；若池为空且已达上限则返回 nullptr
     */
    Ptr acquire() {
        std::lock_guard<std::mutex> lock(core_->mutex);
        if (!core_->pool.empty()) {
            T* obj = core_->pool.back();
            core_->pool.pop_back();
            return Ptr(obj, Deleter(core_));
        }
        if (core_->totalCreated < core_->maxSize) {
            core_->totalCreated++;
            return Ptr(new T(), Deleter(core_));
        }
        return Ptr(nullptr, Deleter{});
    }

    /**
     * @brief 显式归还对象到池中
     * @param obj 要归还的智能指针（调用后 obj 变为 nullptr）
     */
    void release(Ptr obj) {
        if (obj) obj.reset();
    }

    /**
     * @brief 设置对象回收重置函数
     */
    void setResetFunc(std::function<void(T&)> func) {
        core_->resetFunc = std::move(func);
    }

    /**
     * @brief 获取当前池中空闲对象数量
     */
    size_t available() const {
        std::lock_guard<std::mutex> lock(core_->mutex);
        return core_->pool.size();
    }

private:
    std::shared_ptr<PoolCore> core_;  ///< 共享内部状态（Deleter 持 weak_ptr）
};
