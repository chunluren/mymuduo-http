# C++17 特性文档

---

## 一、本项目用到的 C++17 特性

### 1. 结构化绑定（Structured Binding）

**语法**：`auto& [a, b] = pair;`

**本项目使用位置**：

```cpp
// src/registry/ServiceCatalog.h:163
for (const auto& [key, instances] : catalog_) {
    // ...
}

// src/http/HttpServer.h:451
for (const auto& [prefix, dir] : staticDirs_) {
    // ...
}

// src/timer/TimerQueue.h:215
for (auto& [timer, repeat] : expiredTimers) {
    // ...
}

// src/websocket/WebSocketServer.h:170
for (const auto& [id, session] : sessions_) {
    // ...
}

// src/registry/RegistryServer.h:210
for (const auto& [key, instances] : allServices) {
    // ...
}

// src/http/HttpResponse.h:205
for (const auto& [key, value] : headers) {
    // ...
}
```

**对比 C++11**：

```cpp
// C++11 写法
for (const auto& pair : catalog_) {
    const auto& key = pair.first;
    const auto& instances = pair.second;
    // ...
}

// C++17 写法（更简洁）
for (const auto& [key, instances] : catalog_) {
    // ...
}
```

### 2. inline 静态变量

**语法**：`inline static Type var = value;`

**本项目使用位置**：

```cpp
// src/timer/Timer.h:164-165
/// 静态成员定义 (C++17 inline static)
inline std::atomic<int64_t> Timer::nextId_{0};
```

**对比 C++11**：

```cpp
// C++11 写法：需要在 .cpp 文件中定义
// Timer.h
class Timer {
    static std::atomic<int64_t> nextId_;
};

// Timer.cpp
std::atomic<int64_t> Timer::nextId_{0};

// C++17 写法：直接在头文件中定义
// Timer.h
inline std::atomic<int64_t> Timer::nextId_{0};
```

**优势**：
- 无需单独的 .cpp 文件定义静态成员
- 避免链接时的"重复定义"问题
- header-only 库更方便

---

## 二、C++17 相对 C++11 的新特性

### 语言特性

| 特性 | 说明 | 实用程度 |
|------|------|----------|
| **结构化绑定** | `auto [a, b] = pair;` | ⭐⭐⭐⭐⭐ |
| **if constexpr** | 编译期条件判断 | ⭐⭐⭐⭐ |
| **inline 变量** | 头文件中定义静态变量 | ⭐⭐⭐⭐ |
| **折叠表达式** | `(... op args)` 可变参数模板 | ⭐⭐⭐ |
| **auto 占位符扩展** | `auto x{1};` 推导为 int | ⭐⭐⭐ |
| **嵌套命名空间** | `namespace A::B::C {}` | ⭐⭐⭐ |
| **十六进制浮点字面量** | `0x1.2p3` | ⭐⭐ |
| **UTF-8 字面量** | `u8"中文"` | ⭐⭐ |
| **__has_include** | 检查头文件是否存在 | ⭐⭐ |

### 标准库新增

| 特性 | 说明 | 实用程度 |
|------|------|----------|
| **std::optional** | 可选值，可能无值 | ⭐⭐⭐⭐⭐ |
| **std::variant** | 类型安全的 union | ⭐⭐⭐⭐ |
| **std::any** | 任意类型容器 | ⭐⭐⭐ |
| **std::string_view** | 字符串视图，避免拷贝 | ⭐⭐⭐⭐⭐ |
| **std::invoke** | 统一调用方式 | ⭐⭐⭐ |
| **std::apply** | tuple 展开为参数 | ⭐⭐⭐ |
| **文件系统库** | `<filesystem>` | ⭐⭐⭐⭐ |
| **并行算法** | `std::sort(std::execution::par, ...)` | ⭐⭐⭐⭐ |

### 详细说明

#### 1. std::optional

```cpp
#include <optional>

// 可能失败的函数
std::optional<int> divide(int a, int b) {
    if (b == 0) return std::nullopt;
    return a / b;
}

// 使用
auto result = divide(10, 2);
if (result) {
    std::cout << *result << std::endl;  // 5
}

// 或
if (auto r = divide(10, 0)) {
    std::cout << *r << std::endl;
} else {
    std::cout << "除零错误" << std::endl;
}
```

#### 2. std::string_view

```cpp
#include <string_view>

// 避免字符串拷贝
void process(std::string_view sv) {
    // 不拷贝字符串，只是指向原数据
}

std::string s = "hello";
process(s);       // OK
process("world"); // OK，无需构造临时 string
process(s.substr(0, 3));  // OK
```

#### 3. if constexpr

```cpp
// 编译期条件判断
template<typename T>
auto get_value(T t) {
    if constexpr (std::is_pointer_v<T>) {
        return *t;  // 只有 T 是指针时才编译
    } else {
        return t;
    }
}
```

#### 4. 折叠表达式

```cpp
// C++11 可变参数模板
template<typename... Args>
auto sum(Args... args) {
    return (args + ...);  // C++17 折叠表达式
}

sum(1, 2, 3, 4);  // 10
```

#### 5. 嵌套命名空间

```cpp
// C++11
namespace A {
    namespace B {
        namespace C {
            void func();
        }
    }
}

// C++17
namespace A::B::C {
    void func();
}
```

#### 6. std::variant

```cpp
#include <variant>

// 类型安全的 union
std::variant<int, double, std::string> v;

v = 42;              // 存储 int
v = 3.14;            // 存储 double
v = "hello";         // 存储 string

// 访问
std::visit([](auto&& arg) {
    std::cout << arg << std::endl;
}, v);
```

---

## 三、C++ 版本选择建议

| 场景 | 推荐版本 |
|------|----------|
| 需要广泛兼容 | C++11 |
| 现代项目，现代编译器 | C++17 ⭐ |
| 需要最新特性 | C++20 |

**本项目选择 C++17 的理由**：

1. **结构化绑定**：遍历 map 更简洁
2. **inline 静态变量**：header-only 设计更方便
3. **编译器支持**：GCC 7+、Clang 5+、MSVC 2017+ 都支持
4. **向后兼容**：C++11 代码无需修改即可编译

---

## 四、C++17 编译器支持

| 编译器 | 最低版本 |
|--------|----------|
| GCC | 7.0+ |
| Clang | 5.0+ |
| MSVC | 2017 15.0+ |

**编译选项**：`-std=c++17` 或 `-std=c++1z`