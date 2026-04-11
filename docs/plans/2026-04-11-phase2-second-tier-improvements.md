# Phase 2: mymuduo-http 第二梯队改进 实施计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 为 mymuduo-http 添加 Gzip 压缩、Chunked Transfer Encoding、对象池、熔断器，提升性能和可靠性。

**Architecture:** Gzip 和 Chunked 作为 HttpResponse 的后处理中间件（在 `toString()` 时生效）集成。对象池用通用模板实现。熔断器作为独立工具类，可用于 RPC 调用保护。所有模块保持 header-only。

**Tech Stack:** C++17, zlib (libz), 现有 EventLoop timer API

**依赖:** `sudo apt install zlib1g-dev`（大多数 Linux 已自带）

---

## Task 1: Gzip 压缩中间件

**Files:**
- Create: `src/http/GzipMiddleware.h`
- Modify: `src/http/HttpServer.h` (添加 `enableGzip()` 方法)
- Modify: `CMakeLists.txt` (链接 zlib)
- Test: `tests/test_gzip.cpp`

### Step 1: 写测试

创建 `tests/test_gzip.cpp`:

```cpp
// test_gzip.cpp - Gzip 压缩测试
#include <iostream>
#include <cassert>
#include <cstring>
#include "src/http/GzipMiddleware.h"

using namespace std;

void testCompress() {
    cout << "=== Testing Gzip Compress ===" << endl;

    string input = "Hello World! This is a test string for gzip compression. "
                   "Repeated content helps compression ratio. "
                   "Hello World! This is a test string for gzip compression.";

    auto compressed = GzipCodec::compress(input);
    assert(!compressed.empty());
    assert(compressed.size() < input.size());

    cout << "Original: " << input.size() << " bytes, Compressed: "
         << compressed.size() << " bytes" << endl;
    cout << "Compress test passed!" << endl;
}

void testDecompress() {
    cout << "=== Testing Gzip Decompress ===" << endl;

    string input = "Hello World! Compress then decompress should return original.";

    auto compressed = GzipCodec::compress(input);
    auto decompressed = GzipCodec::decompress(compressed);
    assert(decompressed == input);

    cout << "Decompress test passed!" << endl;
}

void testEmptyInput() {
    cout << "=== Testing Empty Input ===" << endl;

    auto compressed = GzipCodec::compress("");
    assert(compressed.empty());

    cout << "Empty input test passed!" << endl;
}

void testLargeData() {
    cout << "=== Testing Large Data ===" << endl;

    // 1MB 重复数据
    string large(1024 * 1024, 'A');
    auto compressed = GzipCodec::compress(large);
    assert(compressed.size() < large.size() / 100);  // 高压缩比

    auto decompressed = GzipCodec::decompress(compressed);
    assert(decompressed == large);

    cout << "Large data test passed!" << endl;
}

void testShouldCompress() {
    cout << "=== Testing shouldCompress ===" << endl;

    // text/html 应该压缩
    assert(GzipCodec::shouldCompress("text/html"));
    assert(GzipCodec::shouldCompress("text/plain; charset=utf-8"));
    assert(GzipCodec::shouldCompress("application/json"));
    assert(GzipCodec::shouldCompress("application/javascript"));

    // image/png 不应该压缩
    assert(!GzipCodec::shouldCompress("image/png"));
    assert(!GzipCodec::shouldCompress("image/jpeg"));
    assert(!GzipCodec::shouldCompress("application/octet-stream"));

    cout << "shouldCompress test passed!" << endl;
}

int main() {
    cout << "Starting Gzip Tests..." << endl << endl;

    testCompress();
    testDecompress();
    testEmptyInput();
    testLargeData();
    testShouldCompress();

    cout << endl << "All Gzip tests passed!" << endl;
    return 0;
}
```

### Step 2: 实现 GzipMiddleware

创建 `src/http/GzipMiddleware.h`:

```cpp
/**
 * @file GzipMiddleware.h
 * @brief HTTP Gzip 压缩
 *
 * 提供 Gzip 压缩/解压功能，可作为 HttpServer 后处理中间件使用。
 *
 * @example
 * @code
 * server.enableGzip(1024);  // 响应体 > 1KB 时压缩
 * @endcode
 */

#pragma once

#include <string>
#include <zlib.h>
#include <cstring>

/// Gzip 编解码工具
class GzipCodec {
public:
    /// 压缩数据（gzip 格式）
    static std::string compress(const std::string& data, int level = Z_DEFAULT_COMPRESSION) {
        if (data.empty()) return "";

        z_stream zs;
        memset(&zs, 0, sizeof(zs));

        // windowBits = 15 + 16 表示 gzip 格式
        if (deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            return "";
        }

        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        zs.avail_in = static_cast<uInt>(data.size());

        std::string output;
        output.resize(deflateBound(&zs, data.size()));

        zs.next_out = reinterpret_cast<Bytef*>(&output[0]);
        zs.avail_out = static_cast<uInt>(output.size());

        int ret = deflate(&zs, Z_FINISH);
        deflateEnd(&zs);

        if (ret != Z_STREAM_END) return "";

        output.resize(zs.total_out);
        return output;
    }

    /// 解压数据（gzip 格式）
    static std::string decompress(const std::string& data) {
        if (data.empty()) return "";

        z_stream zs;
        memset(&zs, 0, sizeof(zs));

        // windowBits = 15 + 16 表示 gzip 格式
        if (inflateInit2(&zs, 15 + 16) != Z_OK) {
            return "";
        }

        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        zs.avail_in = static_cast<uInt>(data.size());

        std::string output;
        char buffer[32768];

        int ret;
        do {
            zs.next_out = reinterpret_cast<Bytef*>(buffer);
            zs.avail_out = sizeof(buffer);

            ret = inflate(&zs, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                inflateEnd(&zs);
                return "";
            }

            output.append(buffer, sizeof(buffer) - zs.avail_out);
        } while (ret != Z_STREAM_END);

        inflateEnd(&zs);
        return output;
    }

    /// 判断 Content-Type 是否值得压缩
    static bool shouldCompress(const std::string& contentType) {
        // 文本类型和 JSON/JS 压缩效果好
        if (contentType.find("text/") == 0) return true;
        if (contentType.find("application/json") != std::string::npos) return true;
        if (contentType.find("application/javascript") != std::string::npos) return true;
        if (contentType.find("application/xml") != std::string::npos) return true;
        return false;
    }
};
```

### Step 3: HttpServer 集成

在 `HttpServer.h` 的 `enableCors()` 后添加 `enableGzip()` 方法:

```cpp
    /**
     * @brief 启用 Gzip 压缩
     * @param minSize 最小压缩阈值（字节），默认 1024
     *
     * 请求包含 Accept-Encoding: gzip 且响应体超过阈值时自动压缩。
     */
    void enableGzip(size_t minSize = 1024) {
        gzipEnabled_ = true;
        gzipMinSize_ = minSize;
    }
```

添加成员变量（private 区域）:
```cpp
    bool gzipEnabled_ = false;
    size_t gzipMinSize_ = 1024;
```

在 `onMessage` 中发送响应之前（`conn->send(response.toString())` 行前），添加 gzip 后处理:

```cpp
            // Gzip 压缩
            if (gzipEnabled_ && response.body.size() >= gzipMinSize_) {
                std::string acceptEncoding = request.getHeader("accept-encoding");
                if (acceptEncoding.find("gzip") != std::string::npos) {
                    auto it = response.headers.find("Content-Type");
                    std::string contentType = (it != response.headers.end()) ? it->second : "";
                    if (GzipCodec::shouldCompress(contentType)) {
                        std::string compressed = GzipCodec::compress(response.body);
                        if (!compressed.empty() && compressed.size() < response.body.size()) {
                            response.body = std::move(compressed);
                            response.setContentLength(response.body.size());
                            response.setHeader("Content-Encoding", "gzip");
                            response.setHeader("Vary", "Accept-Encoding");
                        }
                    }
                }
            }
```

需在 HttpServer.h 顶部添加 `#include "GzipMiddleware.h"`。

### Step 4: CMakeLists.txt 添加 zlib

在 `find_package(OpenSSL REQUIRED)` 后添加:
```cmake
# 查找 zlib
find_package(ZLIB REQUIRED)
```

在测试链接和主目标链接中加入 `ZLIB::ZLIB`。修改测试 foreach:
```cmake
    target_link_libraries(${TEST_NAME} mymuduo nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB pthread)
```

也修改 http_server、full_server 的链接:
```cmake
target_link_libraries(http_server mymuduo nlohmann_json::nlohmann_json ZLIB::ZLIB pthread)
target_link_libraries(full_server mymuduo nlohmann_json::nlohmann_json ZLIB::ZLIB pthread)
```

### Step 5: 编译运行测试

```bash
cd build && cmake .. && make test_gzip http_server -j$(nproc) && ./test_gzip
```

### Step 6: Commit

```bash
git add src/http/GzipMiddleware.h src/http/HttpServer.h tests/test_gzip.cpp CMakeLists.txt
git commit -m "feat: add Gzip compression middleware

GzipCodec: compress/decompress using zlib (gzip format).
HttpServer::enableGzip(minSize): auto-compress responses when client
supports gzip and body exceeds threshold. Only compresses text-based
content types."
```

---

## Task 2: Chunked Transfer Encoding

**Files:**
- Modify: `src/http/HttpResponse.h` (添加 chunked 支持)
- Modify: `src/http/HttpServer.h` (修改发送逻辑)
- Test: `tests/test_chunked.cpp`

### Step 1: 写测试

创建 `tests/test_chunked.cpp`:

```cpp
// test_chunked.cpp - Chunked Transfer Encoding 测试
#include <iostream>
#include <cassert>
#include "src/http/HttpResponse.h"

using namespace std;

void testChunkedEncode() {
    cout << "=== Testing Chunked Encoding ===" << endl;

    HttpResponse resp;
    resp.setStatusCode(HttpStatusCode::OK);
    resp.setContentType("text/plain");
    resp.setChunked(true);

    // 添加 chunk
    resp.addChunk("Hello ");
    resp.addChunk("World!");
    resp.addChunk("");  // 结束 chunk

    string result = resp.toString();

    // 验证 Transfer-Encoding: chunked
    assert(result.find("Transfer-Encoding: chunked") != string::npos);
    // 不应该有 Content-Length
    assert(result.find("Content-Length") == string::npos);
    // 验证 chunk 格式
    assert(result.find("6\r\nHello \r\n") != string::npos);
    assert(result.find("6\r\nWorld!\r\n") != string::npos);
    assert(result.find("0\r\n\r\n") != string::npos);

    cout << "Chunked encoding test passed!" << endl;
}

void testNonChunkedUnchanged() {
    cout << "=== Testing Non-Chunked Unchanged ===" << endl;

    HttpResponse resp;
    resp.setStatusCode(HttpStatusCode::OK);
    resp.setText("Hello");

    string result = resp.toString();
    assert(result.find("Content-Length: 5") != string::npos);
    assert(result.find("Transfer-Encoding") == string::npos);

    cout << "Non-chunked test passed!" << endl;
}

void testChunkedHexLength() {
    cout << "=== Testing Chunked Hex Length ===" << endl;

    HttpResponse resp;
    resp.setStatusCode(HttpStatusCode::OK);
    resp.setContentType("text/plain");
    resp.setChunked(true);

    // 256 字节 chunk
    string data(256, 'A');
    resp.addChunk(data);
    resp.addChunk("");

    string result = resp.toString();
    // 256 = 0x100
    assert(result.find("100\r\n") != string::npos);

    cout << "Chunked hex length test passed!" << endl;
}

int main() {
    cout << "Starting Chunked Transfer Tests..." << endl << endl;

    testChunkedEncode();
    testNonChunkedUnchanged();
    testChunkedHexLength();

    cout << endl << "All Chunked tests passed!" << endl;
    return 0;
}
```

### Step 2: 修改 HttpResponse

在 `HttpResponse` 类中添加:

**新成员变量** (private/public 区域，headers 之后):
```cpp
    bool chunked_ = false;                              ///< 是否 chunked
    std::vector<std::string> chunks_;                   ///< chunk 数据
```

**新方法** (在 `setText()` 之后):
```cpp
    /**
     * @brief 启用 Chunked Transfer Encoding
     * @param enabled 是否启用
     */
    void setChunked(bool enabled) {
        chunked_ = enabled;
    }

    /**
     * @brief 添加 chunk 数据
     * @param data chunk 内容，空字符串表示结束
     */
    void addChunk(const std::string& data) {
        chunks_.push_back(data);
    }
```

**修改 `toString()`** — 在响应头输出部分处理 chunked:

```cpp
    std::string toString() const {
        std::ostringstream oss;

        // 状态行
        oss << "HTTP/1.1 " << static_cast<int>(statusCode) << " " << statusMessage << "\r\n";

        // 响应头
        for (const auto& [key, value] : headers) {
            if (chunked_ && key == "Content-Length") continue;  // chunked 不输出 Content-Length
            oss << key << ": " << value << "\r\n";
        }

        // Chunked 或 Content-Length
        if (chunked_) {
            oss << "Transfer-Encoding: chunked\r\n";
        }

        // Connection 头
        oss << "Connection: " << (closeConnection ? "close" : "keep-alive") << "\r\n";

        // Server 头
        oss << "Server: mymuduo-http/1.0\r\n";

        // 空行
        oss << "\r\n";

        // 响应体
        if (chunked_ && !chunks_.empty()) {
            for (const auto& chunk : chunks_) {
                if (chunk.empty()) {
                    oss << "0\r\n\r\n";  // 结束标记
                } else {
                    oss << std::hex << chunk.size() << "\r\n";
                    oss << chunk << "\r\n";
                }
            }
        } else {
            oss << body;
        }

        return oss.str();
    }
```

### Step 3: 编译运行测试

```bash
cd build && cmake .. && make test_chunked -j$(nproc) && ./test_chunked
```

### Step 4: Commit

```bash
git add src/http/HttpResponse.h tests/test_chunked.cpp
git commit -m "feat: add Chunked Transfer Encoding support

HttpResponse::setChunked(true) + addChunk() enables chunked mode.
toString() outputs Transfer-Encoding: chunked header and hex-length
formatted chunks. Useful for streaming/large responses."
```

---

## Task 3: 对象池 ObjectPool

**Files:**
- Create: `src/util/ObjectPool.h`
- Test: `tests/test_object_pool.cpp`

### Step 1: 写测试

创建 `tests/test_object_pool.cpp`:

```cpp
// test_object_pool.cpp - ObjectPool 单元测试
#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include "src/util/ObjectPool.h"

using namespace std;

struct TestObject {
    int value = 0;
    static atomic<int> createCount;
    static atomic<int> destroyCount;

    TestObject() { createCount++; }
    ~TestObject() { destroyCount++; }
};
atomic<int> TestObject::createCount{0};
atomic<int> TestObject::destroyCount{0};

void testBasicAcquireRelease() {
    cout << "=== Testing Basic Acquire/Release ===" << endl;

    ObjectPool<TestObject> pool(5);  // 预创建 5 个
    assert(pool.available() == 5);

    auto obj = pool.acquire();
    assert(obj != nullptr);
    assert(pool.available() == 4);

    obj->value = 42;
    pool.release(std::move(obj));
    assert(pool.available() == 5);

    // 再获取应该是同一个对象（复用）
    auto obj2 = pool.acquire();
    assert(obj2->value == 42);
    pool.release(std::move(obj2));

    cout << "Basic acquire/release test passed!" << endl;
}

void testPoolExhaustion() {
    cout << "=== Testing Pool Exhaustion ===" << endl;

    ObjectPool<TestObject> pool(3, 5);  // 初始 3，最大 5

    vector<unique_ptr<TestObject, typename ObjectPool<TestObject>::Deleter>> objects;
    for (int i = 0; i < 5; ++i) {
        auto obj = pool.acquire();
        assert(obj != nullptr);
        objects.push_back(std::move(obj));
    }
    assert(pool.available() == 0);

    // 第 6 个应该返回 nullptr（超出最大容量）
    auto overflow = pool.acquire();
    assert(overflow == nullptr);

    // 归还后可以再获取
    objects.clear();
    assert(pool.available() == 5);

    cout << "Pool exhaustion test passed!" << endl;
}

void testThreadSafety() {
    cout << "=== Testing Thread Safety ===" << endl;

    ObjectPool<TestObject> pool(10, 20);
    atomic<int> successCount{0};

    vector<thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&pool, &successCount]() {
            for (int i = 0; i < 100; ++i) {
                auto obj = pool.acquire();
                if (obj) {
                    obj->value = i;
                    successCount++;
                    pool.release(std::move(obj));
                }
            }
        });
    }

    for (auto& t : threads) t.join();

    assert(successCount == 400);
    cout << "Thread safety test passed!" << endl;
}

void testCustomReset() {
    cout << "=== Testing Custom Reset ===" << endl;

    ObjectPool<TestObject> pool(3);

    // 设置重置函数
    pool.setResetFunc([](TestObject& obj) { obj.value = 0; });

    auto obj = pool.acquire();
    obj->value = 99;
    pool.release(std::move(obj));

    auto obj2 = pool.acquire();
    assert(obj2->value == 0);  // 归还时已重置
    pool.release(std::move(obj2));

    cout << "Custom reset test passed!" << endl;
}

int main() {
    cout << "Starting ObjectPool Tests..." << endl << endl;

    testBasicAcquireRelease();
    testPoolExhaustion();
    testThreadSafety();
    testCustomReset();

    cout << endl << "All ObjectPool tests passed!" << endl;
    return 0;
}
```

### Step 2: 实现 ObjectPool

创建 `src/util/ObjectPool.h`:

```cpp
/**
 * @file ObjectPool.h
 * @brief 通用对象池模板
 *
 * 预分配对象，避免频繁 new/delete。线程安全。
 *
 * @example
 * @code
 * ObjectPool<Buffer> pool(10);  // 预创建 10 个 Buffer
 *
 * auto buf = pool.acquire();
 * buf->append("hello", 5);
 * pool.release(std::move(buf));  // 归还到池中
 * @endcode
 */

#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <functional>
#include <cassert>

template<typename T>
class ObjectPool {
public:
    /// 自定义 deleter，归还时放回池中
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

    /**
     * @brief 构造对象池
     * @param initialSize 预创建对象数量
     * @param maxSize 最大对象数量（0 = 无限）
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

    ~ObjectPool() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* obj : pool_) {
            delete obj;
        }
        pool_.clear();
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    /**
     * @brief 获取对象
     * @return 对象智能指针，池耗尽时返回空指针
     */
    Ptr acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pool_.empty()) {
            T* obj = pool_.back();
            pool_.pop_back();
            return Ptr(obj, Deleter(this));
        }

        // 池为空，尝试创建新对象
        if (totalCreated_ < maxSize_) {
            totalCreated_++;
            return Ptr(new T(), Deleter(this));
        }

        return Ptr(nullptr, Deleter(nullptr));
    }

    /**
     * @brief 归还对象
     * @param obj 对象智能指针
     */
    void release(Ptr obj) {
        if (obj) {
            obj.reset();  // 触发 Deleter
        }
    }

    /// 设置重置函数（归还时调用）
    void setResetFunc(std::function<void(T&)> func) {
        resetFunc_ = std::move(func);
    }

    /// 当前可用对象数
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
```

### Step 3: 编译运行测试

```bash
cd build && cmake .. && make test_object_pool -j$(nproc) && ./test_object_pool
```

### Step 4: Commit

```bash
git add src/util/ObjectPool.h tests/test_object_pool.cpp
git commit -m "feat: add generic ObjectPool template

Thread-safe object pool with pre-allocation, max size limit,
custom reset function on release, and unique_ptr-based RAII.
Useful for Buffer, TcpConnection, etc."
```

---

## Task 4: 熔断器 Circuit Breaker

**Files:**
- Create: `src/util/CircuitBreaker.h`
- Test: `tests/test_circuit_breaker.cpp`

### Step 1: 写测试

创建 `tests/test_circuit_breaker.cpp`:

```cpp
// test_circuit_breaker.cpp - CircuitBreaker 单元测试
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include "src/util/CircuitBreaker.h"

using namespace std;

void testNormalOperation() {
    cout << "=== Testing Normal Operation ===" << endl;

    CircuitBreaker cb(5, 2, 1);  // 5次失败打开, 2次成功恢复, 1秒超时

    // 正常状态下应该放行
    assert(cb.state() == CircuitBreaker::Closed);
    assert(cb.allow());
    cb.recordSuccess();
    assert(cb.state() == CircuitBreaker::Closed);

    cout << "Normal operation test passed!" << endl;
}

void testOpenOnFailure() {
    cout << "=== Testing Open on Failure ===" << endl;

    CircuitBreaker cb(3, 1, 1);  // 3次失败打开

    // 3 次连续失败
    for (int i = 0; i < 3; ++i) {
        assert(cb.allow());
        cb.recordFailure();
    }

    // 熔断器应该打开
    assert(cb.state() == CircuitBreaker::Open);
    assert(!cb.allow());  // 拒绝请求

    cout << "Open on failure test passed!" << endl;
}

void testHalfOpenRecovery() {
    cout << "=== Testing Half-Open Recovery ===" << endl;

    CircuitBreaker cb(2, 2, 1);  // 2次失败打开, 2次成功恢复, 1秒超时

    // 触发打开
    cb.recordFailure();
    cb.recordFailure();
    assert(cb.state() == CircuitBreaker::Open);

    // 等待超时进入 Half-Open
    this_thread::sleep_for(chrono::milliseconds(1100));
    assert(cb.state() == CircuitBreaker::HalfOpen);
    assert(cb.allow());  // Half-Open 允许一个探测请求

    // 成功 2 次恢复到 Closed
    cb.recordSuccess();
    cb.recordSuccess();
    assert(cb.state() == CircuitBreaker::Closed);

    cout << "Half-open recovery test passed!" << endl;
}

void testHalfOpenFallback() {
    cout << "=== Testing Half-Open Fallback ===" << endl;

    CircuitBreaker cb(2, 2, 1);

    // 触发打开
    cb.recordFailure();
    cb.recordFailure();
    assert(cb.state() == CircuitBreaker::Open);

    // 等待进入 Half-Open
    this_thread::sleep_for(chrono::milliseconds(1100));
    assert(cb.allow());

    // Half-Open 下再次失败 → 回到 Open
    cb.recordFailure();
    assert(cb.state() == CircuitBreaker::Open);
    assert(!cb.allow());

    cout << "Half-open fallback test passed!" << endl;
}

void testExecuteHelper() {
    cout << "=== Testing Execute Helper ===" << endl;

    CircuitBreaker cb(3, 1, 1);
    int callCount = 0;

    // 成功调用
    auto result = cb.execute([&]() -> bool {
        callCount++;
        return true;
    });
    assert(result == true);
    assert(callCount == 1);
    assert(cb.state() == CircuitBreaker::Closed);

    // 失败调用
    for (int i = 0; i < 3; ++i) {
        cb.execute([&]() -> bool {
            callCount++;
            throw std::runtime_error("fail");
            return true;
        });
    }
    assert(cb.state() == CircuitBreaker::Open);

    // 熔断后不执行函数
    bool executed = false;
    cb.execute([&]() -> bool {
        executed = true;
        return true;
    });
    assert(!executed);

    cout << "Execute helper test passed!" << endl;
}

int main() {
    cout << "Starting CircuitBreaker Tests..." << endl << endl;

    testNormalOperation();
    testOpenOnFailure();
    testHalfOpenRecovery();
    testHalfOpenFallback();
    testExecuteHelper();

    cout << endl << "All CircuitBreaker tests passed!" << endl;
    return 0;
}
```

### Step 2: 实现 CircuitBreaker

创建 `src/util/CircuitBreaker.h`:

```cpp
/**
 * @file CircuitBreaker.h
 * @brief 熔断器（三态）
 *
 * 保护下游服务的熔断器，防止故障蔓延。
 *
 * 三态:
 * - Closed: 正常放行，统计失败次数
 * - Open: 直接拒绝所有请求
 * - HalfOpen: 超时后允许探测请求，成功则恢复
 *
 * @example
 * @code
 * CircuitBreaker cb(5, 2, 10);  // 5次失败打开, 2次成功恢复, 10秒超时
 *
 * if (cb.allow()) {
 *     try {
 *         auto result = rpcClient.call("method", params);
 *         cb.recordSuccess();
 *     } catch (...) {
 *         cb.recordFailure();
 *     }
 * } else {
 *     // 熔断中，返回降级响应
 * }
 *
 * // 或使用 execute 辅助函数
 * auto result = cb.execute([&]() {
 *     return rpcClient.call("method", params);
 * });
 * @endcode
 */

#pragma once

#include <mutex>
#include <chrono>
#include <functional>
#include <stdexcept>

class CircuitBreaker {
public:
    enum State { Closed, Open, HalfOpen };

    /**
     * @brief 构造熔断器
     * @param failureThreshold 连续失败多少次打开熔断
     * @param successThreshold Half-Open 状态下成功多少次恢复
     * @param timeoutSec Open 状态持续多久进入 Half-Open
     */
    CircuitBreaker(int failureThreshold, int successThreshold, int timeoutSec)
        : failureThreshold_(failureThreshold)
        , successThreshold_(successThreshold)
        , timeoutMs_(timeoutSec * 1000)
        , state_(Closed)
        , failureCount_(0)
        , successCount_(0)
        , lastFailureTime_(0)
    {}

    /// 获取当前状态（自动检测超时转换）
    State state() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == Open && nowMs() - lastFailureTime_ >= timeoutMs_) {
            state_ = HalfOpen;
            successCount_ = 0;
        }
        return state_;
    }

    /// 是否允许请求通过
    bool allow() {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (state_) {
            case Closed:
                return true;
            case Open:
                // 检查是否超时
                if (nowMs() - lastFailureTime_ >= timeoutMs_) {
                    state_ = HalfOpen;
                    successCount_ = 0;
                    return true;  // 允许一个探测请求
                }
                return false;
            case HalfOpen:
                return true;  // 允许探测请求
        }
        return false;
    }

    /// 记录成功
    void recordSuccess() {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (state_) {
            case Closed:
                failureCount_ = 0;
                break;
            case HalfOpen:
                successCount_++;
                if (successCount_ >= successThreshold_) {
                    state_ = Closed;
                    failureCount_ = 0;
                    successCount_ = 0;
                }
                break;
            case Open:
                break;
        }
    }

    /// 记录失败
    void recordFailure() {
        std::lock_guard<std::mutex> lock(mutex_);
        lastFailureTime_ = nowMs();
        switch (state_) {
            case Closed:
                failureCount_++;
                if (failureCount_ >= failureThreshold_) {
                    state_ = Open;
                }
                break;
            case HalfOpen:
                // Half-Open 下再次失败，回到 Open
                state_ = Open;
                break;
            case Open:
                break;
        }
    }

    /**
     * @brief 执行函数（自动记录成功/失败）
     * @param func 要执行的函数
     * @return 函数返回值，熔断时返回默认值
     */
    template<typename Func>
    auto execute(Func&& func) -> decltype(func()) {
        using ReturnType = decltype(func());
        if (!allow()) {
            return ReturnType{};
        }
        try {
            auto result = func();
            recordSuccess();
            return result;
        } catch (...) {
            recordFailure();
            return ReturnType{};
        }
    }

private:
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    int failureThreshold_;
    int successThreshold_;
    int64_t timeoutMs_;

    State state_;
    int failureCount_;
    int successCount_;
    int64_t lastFailureTime_;

    std::mutex mutex_;
};
```

### Step 3: 编译运行测试

```bash
cd build && cmake .. && make test_circuit_breaker -j$(nproc) && ./test_circuit_breaker
```

### Step 4: Commit

```bash
git add src/util/CircuitBreaker.h tests/test_circuit_breaker.cpp
git commit -m "feat: add CircuitBreaker with three-state protection

Closed → Open (on consecutive failures) → HalfOpen (after timeout)
→ Closed (on successful probes). Thread-safe with execute() helper
for automatic success/failure recording."
```

---

## Task 5: 全量编译验证 + 更新文档

### Step 1: 全量编译

```bash
cd build && cmake .. && make -j$(nproc) 2>&1
```

### Step 2: 运行所有测试

```bash
for test in test_gzip test_chunked test_object_pool test_circuit_breaker test_websocket_server test_websocket_frame test_buffer test_config test_timer test_load_balancer test_rate_limiter test_mysql_pool test_redis_pool; do
    echo "--- $test ---"
    ./$test 2>&1 | tail -2
done
```

### Step 3: 更新 CLAUDE.md

将阶段 2 的 `[ ]` 改为 `[x]`:

```markdown
### 阶段 2: mymuduo-http 第二梯队改进 ✓
- [x] Gzip 压缩中间件
- [x] Chunked Transfer Encoding
- [x] 内存池 / 对象池
- [x] 熔断器 Circuit Breaker
```

更新 "下次对话" 为阶段 3。

### Step 4: Commit

```bash
git add CLAUDE.md
git commit -m "docs: mark Phase 2 improvements as completed"
```

---

## 总结

| Task | 文件 | 类型 |
|------|------|------|
| 1. Gzip 压缩 | `src/http/GzipMiddleware.h` + `HttpServer.h` | 新建+修改 |
| 2. Chunked Transfer | `src/http/HttpResponse.h` | 修改 |
| 3. ObjectPool | `src/util/ObjectPool.h` | 新建 |
| 4. CircuitBreaker | `src/util/CircuitBreaker.h` | 新建 |
| 5. 验证 + 文档 | CLAUDE.md, CMakeLists.txt | 修改 |

**新增文件:** 4 个 header + 4 个 test
**修改文件:** HttpServer.h, HttpResponse.h, CMakeLists.txt, CLAUDE.md
**依赖:** zlib (zlib1g-dev, 通常已预装)
