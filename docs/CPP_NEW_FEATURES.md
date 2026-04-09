# C++ 新特性完全指南

> 面向面试的 C++11/14/17/20 新特性详解，配合 mymuduo-http 项目源码示例。

---

## 目录

- [一、C++11 语言特性](#一c11-语言特性)
- [二、C++11 标准库](#二c11-标准库)
- [三、C++14 特性](#三c14-特性)
- [四、C++17 特性](#四c17-特性)
- [五、C++20 特性](#五c20-特性)
- [六、面试高频对比总结](#六面试高频对比总结)

---

# 一、C++11 语言特性

C++11 是现代 C++ 的分水岭，引入了大量核心特性。

---

## 1.1 auto 类型推导

**解决的问题**：冗长的类型声明，尤其是迭代器和模板类型。

```cpp
// C++03：类型冗长
std::map<std::string, std::vector<int>>::iterator it = m.begin();

// C++11：auto 自动推导
auto it = m.begin();  // 编译器自动推导类型
```

**推导规则**：

```cpp
int x = 10;
auto a = x;        // int        (值拷贝，忽略 const/引用)
auto& b = x;       // int&       (引用)
const auto& c = x; // const int& (const 引用)
auto&& d = x;      // int&       (万能引用 + 引用折叠)
auto&& e = 10;     // int&&      (万能引用绑定右值)
```

**注意事项**：
- `auto` 不能用于函数参数（C++11/14），C++20 的 abbreviated function template 才允许
- `auto` 不能用于类的非静态成员变量
- `auto` 会忽略顶层 `const` 和引用

---

## 1.2 decltype 类型推断

**解决的问题**：获取表达式的类型，而不实际执行表达式。

```cpp
int x = 10;
decltype(x) y = 20;      // int
decltype(x + 1.0) z;     // double（推断表达式结果类型）
decltype((x)) r = x;     // int&（加括号变左值引用）
```

**与 auto 的区别**：

| 特性 | auto | decltype |
|------|------|----------|
| 推导时机 | 初始化时 | 编译时 |
| 是否保留 const/& | 忽略顶层 const/& | 完整保留 |
| 用途 | 变量声明 | 类型推导、返回值 |

**实用场景：尾置返回类型**

```cpp
// 在模板中推导返回类型
template<typename T, typename U>
auto add(T a, U b) -> decltype(a + b) {
    return a + b;
}
```

---

## 1.3 范围 for 循环（Range-based for）

**解决的问题**：遍历容器的样板代码太多。

```cpp
// C++03
for (std::vector<int>::iterator it = v.begin(); it != v.end(); ++it) {
    std::cout << *it << std::endl;
}

// C++11
for (auto& elem : v) {
    std::cout << elem << std::endl;
}
```

**三种使用方式**：

```cpp
std::vector<int> v = {1, 2, 3, 4, 5};

for (auto elem : v)        { }  // 值拷贝（修改不影响原容器）
for (auto& elem : v)       { }  // 引用（可修改原容器）
for (const auto& elem : v) { }  // const 引用（只读，最高效）
```

**项目中的使用** — `EventLoop::doPendingFunctors()`：

```cpp
// EventLoop.cc — 遍历待执行的回调函数
std::vector<Functor> functors;
{
    std::unique_lock<std::mutex> lock(mutex_);
    functors.swap(pendingFunctors_);
}

for (const auto& functor : functors) {
    functor();  // 执行回调
}
```

---

## 1.4 lambda 表达式（重点）

**解决的问题**：需要定义简短的函数对象时，不再需要写完整的 class。

**基本语法**：

```
[捕获列表](参数列表) -> 返回类型 { 函数体 }
```

**捕获方式**：

```cpp
int x = 10, y = 20;

auto f1 = [x]()       { return x; };       // 值捕获 x
auto f2 = [&x]()      { x++; };            // 引用捕获 x
auto f3 = [=]()       { return x + y; };   // 值捕获所有外部变量
auto f4 = [&]()       { x++; y++; };       // 引用捕获所有外部变量
auto f5 = [=, &x]()   { x++; return y; };  // x 引用捕获，其余值捕获
auto f6 = [this]()     { return member_; }; // 捕获 this 指针
```

**项目中的大量使用**：

```cpp
// Callbacks.h — 类型别名配合 lambda
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;

// HttpServer.h — 路由注册使用 lambda
server.GET("/api/users", [](const HttpRequest& req, HttpResponse& resp) {
    resp.json(R"([{"id":1,"name":"Alice"}])");
});

// EventLoop — 跨线程任务调度
loop->runInLoop([conn]() {
    conn->connectEstablished();  // 在 IO 线程中执行
});

// TcpServer — Channel 回调
channel_->setReadCallback(
    std::bind(&TcpConnection::handleRead, this, std::placeholders::_1)
);
// 等价于更清晰的 lambda 写法：
channel_->setReadCallback(
    [this](Timestamp receiveTime) { handleRead(receiveTime); }
);
```

**lambda vs std::bind**：

| 特性 | lambda | std::bind |
|------|--------|-----------|
| 可读性 | 直观 | 需要 placeholders |
| 性能 | 编译器更容易内联 | 可能有额外间接调用 |
| 灵活性 | 可以写任意逻辑 | 只能绑定参数 |
| 建议 | 优先使用 | 仅在需要时使用 |

**面试追问：lambda 的本质是什么？**

> 编译器会为每个 lambda 生成一个匿名类，捕获的变量成为该类的成员变量，`operator()` 就是函数体。

```cpp
// 这个 lambda:
auto f = [x, &y](int a) { return x + y + a; };

// 编译器大致生成:
class __lambda_001 {
    int x;      // 值捕获
    int& y;     // 引用捕获
public:
    __lambda_001(int x, int& y) : x(x), y(y) {}
    int operator()(int a) const { return x + y + a; }
};
auto f = __lambda_001(x, y);
```

---

## 1.5 右值引用 & move 语义（重点）

**解决的问题**：减少不必要的深拷贝，提升性能。

### 左值与右值

```cpp
int a = 10;      // a 是左值（有名字、有地址）
                  // 10 是右值（临时的、没有地址）

int& ref = a;     // 左值引用
int&& rref = 10;  // 右值引用（C++11 新增）
// int&& rref2 = a;  // 错误！右值引用不能绑定左值
int&& rref3 = std::move(a);  // OK，std::move 把左值转为右值
```

### 移动构造 & 移动赋值

```cpp
class MyString {
    char* data_;
    size_t size_;
public:
    // 拷贝构造：深拷贝，O(n)
    MyString(const MyString& other)
        : data_(new char[other.size_])
        , size_(other.size_)
    {
        memcpy(data_, other.data_, size_);
    }

    // 移动构造：偷资源，O(1)
    MyString(MyString&& other) noexcept
        : data_(other.data_)
        , size_(other.size_)
    {
        other.data_ = nullptr;  // 让原对象放弃所有权
        other.size_ = 0;
    }

    // 移动赋值
    MyString& operator=(MyString&& other) noexcept {
        if (this != &other) {
            delete[] data_;
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }
};

MyString a("hello");
MyString b = std::move(a);  // 移动构造，a 变为空壳
```

### 项目中的使用

```cpp
// EventLoop.h — Functor 使用 std::function<void()>
// runInLoop 传入回调时会触发 move
void EventLoop::queueInLoop(Functor cb) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(std::move(cb));  // move 避免拷贝
    }
    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();
    }
}

// doPendingFunctors — swap 而不是拷贝
void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);  // O(1) 交换，不拷贝
    }
    for (const auto& functor : functors) {
        functor();
    }
}
```

### 完美转发（std::forward）

```cpp
template<typename T>
void wrapper(T&& arg) {
    // 如果 arg 是左值引用，转发为左值
    // 如果 arg 是右值引用，转发为右值
    target(std::forward<T>(arg));
}
```

**万能引用 vs 右值引用**：

```cpp
template<typename T>
void func(T&& arg);   // 万能引用（模板 + &&）

void func(int&& arg); // 右值引用（具体类型 + &&）
```

**引用折叠规则**：

| T 的类型 | T&& 的结果 |
|----------|-----------|
| int      | int&&     |
| int&     | int&      |
| int&&    | int&&     |

> 规则：只要有一个 `&` 就折叠为 `&`，两个 `&&` 才是 `&&`。

---

## 1.6 智能指针（重点）

**解决的问题**：手动 `new/delete` 容易导致内存泄漏和悬空指针。

### std::unique_ptr — 独占所有权

```cpp
// 创建
auto ptr = std::make_unique<int>(42);    // C++14
std::unique_ptr<int> ptr2(new int(42));  // C++11

// 不可拷贝，只能移动
// auto ptr3 = ptr;                      // 编译错误！
auto ptr3 = std::move(ptr);             // OK，所有权转移

// 自定义删除器
auto deleter = [](FILE* fp) { fclose(fp); };
std::unique_ptr<FILE, decltype(deleter)> file(fopen("a.txt", "r"), deleter);
```

**项目中的使用**：

```cpp
// EventLoop.h — Poller 和 wakeupChannel 用 unique_ptr 管理
std::unique_ptr<Poller> poller_;
std::unique_ptr<Channel> wakeupChannel_;

// TcpConnection.h — Socket 和 Channel 用 unique_ptr 管理
std::unique_ptr<Socket> socket_;
std::unique_ptr<Channel> channel_;
```

### std::shared_ptr — 共享所有权

```cpp
auto sp1 = std::make_shared<int>(42);
auto sp2 = sp1;  // 引用计数 +1，现在为 2

std::cout << sp1.use_count();  // 2
sp2.reset();                    // 引用计数 -1
std::cout << sp1.use_count();  // 1
// sp1 析构时，引用计数归 0，释放内存
```

**项目中的使用**：

```cpp
// Callbacks.h — TcpConnection 用 shared_ptr 管理
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

// Thread.h — std::thread 用 shared_ptr 管理
std::shared_ptr<std::thread> thread_;
```

### std::weak_ptr — 弱引用

```cpp
auto sp = std::make_shared<int>(42);
std::weak_ptr<int> wp = sp;  // 不增加引用计数

// 使用前必须 lock()
if (auto locked = wp.lock()) {
    // locked 是 shared_ptr，对象还活着
    std::cout << *locked << std::endl;
} else {
    // 对象已被销毁
}
```

**解决循环引用**：

```cpp
struct Node {
    std::shared_ptr<Node> next;   // 强引用
    std::weak_ptr<Node> prev;     // 弱引用，打破循环
};
```

**项目中的使用**：

```cpp
// Channel 通过 weak_ptr 引用 TcpConnection
// 防止 Channel 回调时 TcpConnection 已经被销毁
std::weak_ptr<void> tie_;

void Channel::handleEvent(Timestamp receiveTime) {
    if (tied_) {
        std::shared_ptr<void> guard = tie_.lock();
        if (guard) {
            handleEventWithGuard(receiveTime);
        }
        // guard 为空说明 TcpConnection 已销毁，不处理
    }
}
```

### 三种智能指针对比

| 特性 | unique_ptr | shared_ptr | weak_ptr |
|------|-----------|-----------|----------|
| 所有权 | 独占 | 共享 | 无 |
| 引用计数 | 无 | 有（原子操作） | 不增加 |
| 拷贝 | 不可 | 可以 | 可以 |
| 开销 | 接近裸指针 | 引用计数+控制块 | 很小 |
| 适用场景 | 明确独占 | 多处共享 | 观察者/打破循环 |

---

## 1.7 nullptr

**解决的问题**：C 语言的 `NULL` 是整数 0，在重载函数时有歧义。

```cpp
void func(int n)    { std::cout << "int" << std::endl; }
void func(int* ptr) { std::cout << "pointer" << std::endl; }

func(NULL);     // 调用 func(int)！因为 NULL 是整数 0
func(nullptr);  // 调用 func(int*)，正确
```

`nullptr` 的类型是 `std::nullptr_t`，可以隐式转换为任意指针类型，但不能转为整数。

---

## 1.8 统一初始化（Uniform Initialization）

**解决的问题**：C++ 初始化方式太多（`=`、`()`、`{}`），语义不一致。

```cpp
// C++11 统一使用 {} 初始化
int x{10};                        // 基本类型
std::vector<int> v{1, 2, 3, 4};  // 容器初始化列表
std::map<std::string, int> m{{"a", 1}, {"b", 2}};

// 防止窄化转换
int a = 3.14;   // OK，隐式截断为 3
// int b{3.14};  // 编译错误！{} 禁止窄化转换
```

**std::initializer_list**：

```cpp
void printAll(std::initializer_list<int> list) {
    for (auto elem : list) {
        std::cout << elem << " ";
    }
}
printAll({1, 2, 3, 4, 5});
```

---

## 1.9 constexpr — 编译期常量

**解决的问题**：让函数和变量在编译期求值，提升性能。

```cpp
// 编译期计算
constexpr int factorial(int n) {
    return n <= 1 ? 1 : n * factorial(n - 1);
}

constexpr int result = factorial(5);  // 编译期求值 = 120
// 相当于写了 constexpr int result = 120;

// 用于数组大小
constexpr int SIZE = 100;
int arr[SIZE];  // OK，编译期常量
```

**const vs constexpr**：

| 特性 | const | constexpr |
|------|-------|-----------|
| 语义 | 运行时不可修改 | 编译期可求值 |
| 初始化 | 可以运行时 | 必须编译期 |
| 函数 | 不能修饰 | 可以修饰 |

---

## 1.10 enum class — 强类型枚举

**解决的问题**：传统 enum 会污染命名空间，可以隐式转换为 int。

```cpp
// C++03 问题
enum Color { RED, GREEN };
enum TrafficLight { RED, YELLOW, GREEN };  // 错误！RED 重定义

// C++11 enum class
enum class Color { RED, GREEN, BLUE };
enum class TrafficLight { RED, YELLOW, GREEN };  // OK，不冲突

Color c = Color::RED;
// int n = c;  // 编译错误！不能隐式转为 int
int n = static_cast<int>(c);  // 必须显式转换
```

**项目中的使用**：

```cpp
// AsyncLogger.h — 日志级别
enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

// static_cast 显式转换
const char* levelStr[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
file << levelStr[static_cast<int>(entry.level)];
```

---

## 1.11 override 和 final

**解决的问题**：防止虚函数重写时拼写错误或签名不匹配导致的 bug。

```cpp
class Base {
public:
    virtual void func(int x) {}
    virtual void draw() {}
};

class Derived : public Base {
public:
    void func(int x) override {}     // OK，正确重写
    // void func(double x) override {}  // 编译错误！签名不匹配
    void draw() override final {}    // 重写，且禁止再被重写
};

class Final : public Derived {
    // void draw() override {}  // 编译错误！draw 已经 final
};
```

---

## 1.12 可变参数模板（Variadic Templates）

**解决的问题**：C 的 `va_list` 不类型安全；支持任意数量、任意类型的参数。

```cpp
// 递归终止
void print() {}

// 可变参数模板
template<typename T, typename... Args>
void print(T first, Args... rest) {
    std::cout << first << " ";
    print(rest...);  // 递归展开
}

print(1, "hello", 3.14, 'c');
// 输出: 1 hello 3.14 c
```

**sizeof...获取参数包大小**：

```cpp
template<typename... Args>
void func(Args... args) {
    std::cout << sizeof...(Args) << std::endl;  // 类型参数个数
    std::cout << sizeof...(args) << std::endl;  // 值参数个数
}
```

---

## 1.13 default 和 delete

**解决的问题**：显式控制编译器自动生成的特殊成员函数。

```cpp
class MyClass {
public:
    MyClass() = default;                         // 使用默认实现
    MyClass(const MyClass&) = delete;            // 禁止拷贝构造
    MyClass& operator=(const MyClass&) = delete; // 禁止拷贝赋值
    MyClass(MyClass&&) = default;                // 使用默认移动构造
};
```

**项目中的使用** — `noncopyable` 基类：

```cpp
// noncopyable.h
class noncopyable {
public:
    noncopyable(const noncopyable&) = delete;
    noncopyable& operator=(const noncopyable&) = delete;
protected:
    noncopyable() = default;
    ~noncopyable() = default;
};

// EventLoop 继承 noncopyable，自动禁止拷贝
class EventLoop : noncopyable { ... };
class TcpConnection : noncopyable, public std::enable_shared_from_this<TcpConnection> { ... };
```

---

## 1.14 委托构造（Delegating Constructor）

**解决的问题**：构造函数之间的代码重复。

```cpp
class Server {
    int port_;
    int threads_;
public:
    Server() : Server(8080, 4) {}            // 委托给第三个构造函数
    Server(int port) : Server(port, 4) {}    // 委托给第三个构造函数
    Server(int port, int threads)            // 主构造函数
        : port_(port), threads_(threads) {}
};
```

---

## 1.15 static_assert — 编译期断言

```cpp
static_assert(sizeof(int) == 4, "int must be 4 bytes");
static_assert(sizeof(void*) == 8, "Only 64-bit platforms supported");

template<typename T>
class Container {
    static_assert(std::is_default_constructible<T>::value,
                  "T must be default constructible");
};
```

---

## 1.16 using 类型别名

**解决的问题**：`typedef` 不支持模板别名。

```cpp
// typedef 方式
typedef std::vector<int> IntVec;
typedef void (*FuncPtr)(int, int);

// using 方式（更清晰）
using IntVec = std::vector<int>;
using FuncPtr = void(*)(int, int);

// using 支持模板别名（typedef 做不到！）
template<typename T>
using Vec = std::vector<T>;

Vec<int> v;     // std::vector<int>
Vec<double> vd; // std::vector<double>
```

**项目中大量使用**：

```cpp
// EventLoop.h
using Functor = std::function<void()>;
using ChannelList = std::vector<Channel*>;

// Callbacks.h
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*, Timestamp)>;

// Thread.h
using ThreadFunc = std::function<void()>;

// HttpServer.h
using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;
```

---

# 二、C++11 标准库

---

## 2.1 std::function — 通用可调用对象包装器

**解决的问题**：统一函数指针、lambda、仿函数、成员函数的调用方式。

```cpp
#include <functional>

// 普通函数
int add(int a, int b) { return a + b; }

// 仿函数
struct Multiply {
    int operator()(int a, int b) { return a * b; }
};

// 统一使用 std::function
std::function<int(int, int)> func;

func = add;                                   // 函数指针
func = Multiply();                            // 仿函数
func = [](int a, int b) { return a - b; };   // lambda

std::cout << func(3, 2);  // 1（最后赋值的 lambda）
```

**项目中的核心使用**：

```cpp
// Callbacks.h — 所有回调都用 std::function
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*, Timestamp)>;

// EventLoop.h — 任务队列
using Functor = std::function<void()>;
std::vector<Functor> pendingFunctors_;
```

---

## 2.2 std::bind — 函数绑定

```cpp
#include <functional>

void greet(const std::string& name, int age) {
    std::cout << name << " is " << age << std::endl;
}

// 绑定第一个参数
auto greetAlice = std::bind(greet, "Alice", std::placeholders::_1);
greetAlice(25);  // Alice is 25

// 绑定成员函数
class Foo {
public:
    void print(int x) { std::cout << x; }
};
Foo foo;
auto f = std::bind(&Foo::print, &foo, std::placeholders::_1);
f(42);  // 42
```

> **现代 C++ 建议**：优先用 lambda 代替 `std::bind`，更直观、更高效。

---

## 2.3 std::thread — 线程

```cpp
#include <thread>

void worker(int id) {
    std::cout << "Thread " << id << std::endl;
}

std::thread t1(worker, 1);
std::thread t2([]() {
    std::cout << "Lambda thread" << std::endl;
});

t1.join();   // 等待线程结束
t2.join();

// 注意：必须 join() 或 detach()，否则析构时 std::terminate
```

**项目中的使用**：

```cpp
// Thread.h — 封装 std::thread
class Thread : noncopyable {
    std::shared_ptr<std::thread> thread_;
    ThreadFunc func_;
public:
    void start() {
        thread_ = std::make_shared<std::thread>(func_);
    }
    void join() {
        if (thread_->joinable()) {
            thread_->join();
        }
    }
};

// AsyncLogger.h — 后台日志线程
std::thread writerThread_;
writerThread_ = std::thread(&AsyncLogger::writerLoop, this);
```

---

## 2.4 std::mutex & std::lock_guard & std::unique_lock

```cpp
#include <mutex>

std::mutex mtx;
int shared_data = 0;

// lock_guard：RAII，作用域结束自动解锁
void func1() {
    std::lock_guard<std::mutex> lock(mtx);
    shared_data++;
    // 自动解锁
}

// unique_lock：更灵活，支持手动 lock/unlock、defer_lock、try_lock
void func2() {
    std::unique_lock<std::mutex> lock(mtx);
    shared_data++;
    lock.unlock();       // 手动解锁
    // ... 做不需要锁的操作 ...
    lock.lock();         // 重新加锁
}
```

**lock_guard vs unique_lock**：

| 特性 | lock_guard | unique_lock |
|------|-----------|------------|
| 手动解锁 | 不支持 | 支持 |
| 移动语义 | 不支持 | 支持 |
| 配合 condition_variable | 不支持 | 支持 |
| 开销 | 更小 | 稍大 |
| 使用建议 | 简单加锁 | 需要灵活控制 |

**项目中的使用**：

```cpp
// AsyncLogger.h — lock_guard 保护日志写入
void log(LogLevel level, ...) {
    // ...
    {
        std::lock_guard<std::mutex> lock(mutex_);
        currentBuffer_->push_back(entry);
        if (currentBuffer_->size() >= kFlushThreshold) {
            cv_.notify_one();
        }
    }
}

// AsyncLogger.h — unique_lock 配合 condition_variable
void writerLoop() {
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !running_.load() || currentBuffer_->size() >= kFlushThreshold;
            });
            std::swap(currentBuffer_, flushBuffer_);
        }
        // ... 写文件 ...
    }
}

// EventLoop.cc — 保护 pendingFunctors_
{
    std::unique_lock<std::mutex> lock(mutex_);
    pendingFunctors_.emplace_back(std::move(cb));
}
```

---

## 2.5 std::condition_variable — 条件变量

```cpp
#include <condition_variable>

std::mutex mtx;
std::condition_variable cv;
bool ready = false;

// 生产者
void producer() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        ready = true;
    }
    cv.notify_one();  // 通知一个等待的线程
}

// 消费者
void consumer() {
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, []{ return ready; });
    // ready 为 true 才会继续执行
    // wait 内部：如果谓词为 false，释放锁并阻塞
    //           被唤醒后重新获取锁，检查谓词
}
```

**为什么 wait 要传谓词？** 防止虚假唤醒（spurious wakeup）。操作系统可能在没有 notify 时唤醒线程。

**wait_for — 带超时的等待**：

```cpp
// 项目中的使用 — AsyncLogger 超时刷盘
cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
    return !running_.load() || currentBuffer_->size() >= kFlushThreshold;
});
// 超时后无论谓词是否满足都返回
// 返回 true：谓词满足
// 返回 false：超时
```

---

## 2.6 std::atomic — 原子操作

```cpp
#include <atomic>

std::atomic<int> counter{0};

// 原子操作，无需加锁
counter++;                        // 等价于 counter.fetch_add(1)
counter.store(42);                // 原子写
int val = counter.load();         // 原子读
int old = counter.exchange(100);  // 原子交换

// CAS（Compare And Swap）
int expected = 42;
counter.compare_exchange_strong(expected, 100);
// 如果 counter == 42，设为 100，返回 true
// 否则，expected 被设为 counter 的当前值，返回 false
```

**项目中的大量使用**：

```cpp
// EventLoop.h
std::atomic_bool looping_;   // 是否在循环中
std::atomic_bool quit_;      // 退出标志
std::atomic_bool callingPendingFunctors_;

// TcpConnection.h
std::atomic_int state_;      // 连接状态

// Thread.h
static std::atomic_int numCreated_;  // 创建的线程数

// AsyncLogger.h
std::atomic<LogLevel> level_;   // 日志级别
std::atomic<bool> running_;     // 运行标志
```

**atomic vs mutex**：

| 特性 | atomic | mutex |
|------|--------|-------|
| 适用场景 | 单个变量的简单操作 | 保护代码段/多变量 |
| 性能 | CPU 指令级，极快 | 系统调用，较慢 |
| 使用复杂度 | 简单 | 需要 RAII |

---

## 2.7 std::chrono — 时间库

```cpp
#include <chrono>

// 时间点
auto start = std::chrono::steady_clock::now();

// ... 执行操作 ...

auto end = std::chrono::steady_clock::now();
auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
std::cout << "耗时: " << duration.count() << " ms" << std::endl;

// 时间段
using namespace std::chrono_literals;  // C++14
auto timeout = 100ms;   // std::chrono::milliseconds(100)
auto delay = 2s;        // std::chrono::seconds(2)
```

**项目中的使用**：

```cpp
// AsyncLogger.h — 超时等待
cv_.wait_for(lock, std::chrono::milliseconds(100), predicate);

// AsyncLogger.h — 获取毫秒级时间戳
auto now = std::chrono::system_clock::now();
auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    now.time_since_epoch()) % 1000;
```

---

## 2.8 std::unordered_map / std::unordered_set

**解决的问题**：`map/set` 基于红黑树（O(log n)），哈希表更快（平均 O(1)）。

```cpp
#include <unordered_map>

std::unordered_map<std::string, int> scores;
scores["Alice"] = 95;
scores["Bob"] = 87;

// 查找 O(1) 平均
if (scores.count("Alice")) {
    std::cout << scores["Alice"] << std::endl;
}
```

| 特性 | map | unordered_map |
|------|-----|---------------|
| 底层结构 | 红黑树 | 哈希表 |
| 查找 | O(log n) | O(1) 平均 |
| 有序性 | 按 key 排序 | 无序 |
| 迭代器失效 | 更稳定 | rehash 时全部失效 |

---

## 2.9 std::array & std::tuple

```cpp
// std::array — 固定大小数组，比 C 数组更安全
#include <array>
std::array<int, 5> arr = {1, 2, 3, 4, 5};
arr.at(0) = 10;  // 带边界检查
arr.size();       // 5，编译期已知

// std::tuple — 元组，可存储不同类型
#include <tuple>
auto t = std::make_tuple(1, "hello", 3.14);
std::cout << std::get<0>(t);  // 1
std::cout << std::get<1>(t);  // hello

// C++17 结构化绑定
auto [x, y, z] = t;  // x=1, y="hello", z=3.14
```

---

# 三、C++14 特性

C++14 是 C++11 的补充和完善。

---

## 3.1 泛型 lambda

```cpp
// C++11：lambda 参数必须指定类型
auto add11 = [](int a, int b) { return a + b; };

// C++14：auto 参数，泛型 lambda
auto add14 = [](auto a, auto b) { return a + b; };

add14(1, 2);       // int + int
add14(1.5, 2.5);   // double + double
add14(std::string("hello"), std::string(" world"));  // string
```

---

## 3.2 返回类型自动推导

```cpp
// C++11 需要尾置返回类型
auto add(int a, int b) -> int { return a + b; }

// C++14 自动推导返回类型
auto add(int a, int b) {
    return a + b;  // 编译器自动推导为 int
}

// 多个 return 必须类型一致
auto func(bool flag) {
    if (flag) return 1;
    return 2;           // OK，都是 int
    // return 1.0;       // 错误！类型不一致
}
```

---

## 3.3 std::make_unique

```cpp
// C++11 只有 make_shared
auto sp = std::make_shared<MyClass>(args...);
std::unique_ptr<MyClass> up(new MyClass(args...));  // 不对称

// C++14 补上了 make_unique
auto up = std::make_unique<MyClass>(args...);
```

**为什么 make_unique/make_shared 比 new 好？**

```cpp
// 潜在内存泄漏（异常安全问题）
func(std::unique_ptr<A>(new A()), std::unique_ptr<B>(new B()));
// 编译器可能先 new A、new B，再构造 unique_ptr
// 如果 new B 抛异常，A 就泄漏了

// make_unique 是异常安全的
func(std::make_unique<A>(), std::make_unique<B>());
```

---

## 3.4 二进制字面量 & 数字分隔符

```cpp
// 二进制字面量
int mask = 0b1111'0000;  // 240

// 数字分隔符（单引号）
int million = 1'000'000;
double pi = 3.141'592'653;
long long bignum = 0xFF'FF'FF'FF;
```

---

## 3.5 放松的 constexpr

```cpp
// C++11 constexpr 函数只能有一条 return 语句
constexpr int factorial11(int n) {
    return n <= 1 ? 1 : n * factorial11(n - 1);
}

// C++14 放松限制，可以有循环、条件语句、局部变量
constexpr int factorial14(int n) {
    int result = 1;
    for (int i = 2; i <= n; ++i) {
        result *= i;
    }
    return result;
}
```

---

# 四、C++17 特性

---

## 4.1 结构化绑定（Structured Bindings）

**解决的问题**：从 pair/tuple/struct 中解包变量太啰嗦。

```cpp
// C++11
std::map<std::string, int> m = {{"a", 1}, {"b", 2}};
for (const auto& pair : m) {
    std::cout << pair.first << ": " << pair.second;
}

// C++17
for (const auto& [key, value] : m) {
    std::cout << key << ": " << value;
}
```

**项目中的大量使用**：

```cpp
// ServiceCatalog.h — 遍历服务目录
for (const auto& [key, instances] : catalog_) { ... }

// HttpServer.h — 遍历静态目录映射
for (const auto& [prefix, dir] : staticDirs_) { ... }

// TimerQueue.h — 遍历过期定时器
for (auto& [timer, repeat] : expiredTimers) { ... }

// WebSocketServer.h — 遍历 WebSocket 会话
for (const auto& [id, session] : sessions_) { ... }

// HttpResponse.h — 遍历响应头
for (const auto& [key, value] : headers) { ... }
```

---

## 4.2 if constexpr — 编译期条件分支

**解决的问题**：模板中的条件编译，避免不必要的实例化。

```cpp
template<typename T>
std::string to_string(T value) {
    if constexpr (std::is_integral_v<T>) {
        return std::to_string(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        return std::to_string(value);
    } else if constexpr (std::is_same_v<T, std::string>) {
        return value;
    } else {
        static_assert(false, "Unsupported type");
    }
}
```

**与普通 if 的区别**：

```cpp
template<typename T>
void process(T value) {
    // 普通 if：两个分支都会编译，可能报错
    if (std::is_integral_v<T>) {
        value++;  // 如果 T 是 string，编译失败！
    }

    // if constexpr：编译期选择，未选中的分支不编译
    if constexpr (std::is_integral_v<T>) {
        value++;  // T 不是 integral 时这段代码直接丢弃
    }
}
```

---

## 4.3 inline 变量

**解决的问题**：头文件中定义静态变量会导致链接错误（重复定义）。

```cpp
// C++11：必须在 .cpp 中定义
// Timer.h
class Timer {
    static std::atomic<int64_t> nextId_;
};
// Timer.cpp（必须单独定义）
std::atomic<int64_t> Timer::nextId_{0};

// C++17：直接在头文件中定义
// Timer.h
inline std::atomic<int64_t> Timer::nextId_{0};  // 项目实际使用
```

**优势**：
- header-only 库更方便
- 消除了忘记在 .cpp 定义的 bug
- 链接器保证全局只有一份

---

## 4.4 折叠表达式（Fold Expressions）

**解决的问题**：可变参数模板中，不再需要递归展开。

```cpp
// C++11 递归展开
void print() {}  // 终止条件
template<typename T, typename... Args>
void print(T first, Args... rest) {
    std::cout << first << " ";
    print(rest...);
}

// C++17 折叠表达式，一行搞定
template<typename... Args>
void print(Args... args) {
    ((std::cout << args << " "), ...);  // 逗号折叠
}

// 四种折叠形式
template<typename... Args>
auto sum(Args... args) {
    return (args + ...);    // 右折叠: a + (b + (c + d))
    return (... + args);    // 左折叠: ((a + b) + c) + d
    return (args + ... + 0);  // 右折叠带初值
    return (0 + ... + args);  // 左折叠带初值
}
```

---

## 4.5 std::optional — 可选值

**解决的问题**：表示"可能没有值"，替代 `nullptr`、`-1`、异常等 hack。

```cpp
#include <optional>

// 查找函数，可能找不到
std::optional<int> findIndex(const std::vector<int>& v, int target) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == target) return i;
    }
    return std::nullopt;  // 没找到
}

// 使用
auto result = findIndex({1, 2, 3}, 2);
if (result) {                       // 或 result.has_value()
    std::cout << *result;            // 2（解引用获取值）
}

std::cout << result.value_or(-1);   // 有值返回值，无值返回默认值
```

---

## 4.6 std::variant — 类型安全的 union

**解决的问题**：C 的 `union` 不类型安全，不知道当前存的是什么类型。

```cpp
#include <variant>

std::variant<int, double, std::string> v;

v = 42;
std::cout << std::get<int>(v);  // 42

v = "hello";
// std::get<int>(v);  // 抛出 std::bad_variant_access

// 安全访问
if (auto* p = std::get_if<std::string>(&v)) {
    std::cout << *p;  // hello
}

// visitor 模式
std::visit([](auto&& arg) {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, int>) {
        std::cout << "int: " << arg;
    } else if constexpr (std::is_same_v<T, std::string>) {
        std::cout << "string: " << arg;
    }
}, v);
```

---

## 4.7 std::string_view — 字符串视图

**解决的问题**：传递字符串时避免不必要的拷贝。

```cpp
#include <string_view>

// 不拷贝字符串，只是"看一眼"
void process(std::string_view sv) {
    std::cout << sv.substr(0, 5);  // 子串也不拷贝
    std::cout << sv.size();
}

std::string s = "hello world";
process(s);           // OK，从 string
process("literal");   // OK，从字面量，零拷贝
process(s.c_str());   // OK，从 C 字符串

// 注意：string_view 不拥有数据！
// 原字符串销毁后，string_view 变成悬空引用
```

**string_view vs const string&**：

| 特性 | const string& | string_view |
|------|--------------|-------------|
| 接受字面量 | 会创建临时 string | 零拷贝 |
| 接受 string | 零拷贝 | 零拷贝 |
| 接受 char* | 会创建临时 string | 零拷贝 |
| 拥有数据 | 引用原 string | 不拥有 |

---

## 4.8 std::filesystem — 文件系统库

```cpp
#include <filesystem>
namespace fs = std::filesystem;

// 路径操作
fs::path p = "/home/user/test.txt";
std::cout << p.filename();   // test.txt
std::cout << p.extension();  // .txt
std::cout << p.parent_path(); // /home/user

// 文件操作
if (fs::exists(p)) {
    std::cout << fs::file_size(p) << " bytes";
}

// 遍历目录
for (const auto& entry : fs::directory_iterator("/home/user")) {
    std::cout << entry.path() << std::endl;
}

// 递归遍历
for (const auto& entry : fs::recursive_directory_iterator("/home/user")) {
    if (entry.is_regular_file()) {
        std::cout << entry.path() << ": " << entry.file_size() << std::endl;
    }
}

// 创建/删除
fs::create_directories("/tmp/a/b/c");
fs::remove_all("/tmp/a");
fs::copy_file("src.txt", "dst.txt");
```

---

## 4.9 CTAD — 类模板参数推导

**解决的问题**：使用类模板时必须写模板参数。

```cpp
// C++14
std::pair<int, double> p(1, 2.0);
std::vector<int> v = {1, 2, 3};
std::lock_guard<std::mutex> lock(mtx);

// C++17 CTAD：自动推导
std::pair p(1, 2.0);           // pair<int, double>
std::vector v = {1, 2, 3};    // vector<int>
std::lock_guard lock(mtx);    // lock_guard<std::mutex>
std::tuple t(1, "hi", 3.14);  // tuple<int, const char*, double>
```

---

## 4.10 if/switch 带初始化

```cpp
// C++17: if 语句中声明变量
if (auto it = m.find("key"); it != m.end()) {
    // it 只在 if 作用域内有效
    std::cout << it->second;
}
// it 在这里不可访问

// 等价于 C++11
{
    auto it = m.find("key");
    if (it != m.end()) {
        std::cout << it->second;
    }
}
```

---

## 4.11 并行算法

```cpp
#include <algorithm>
#include <execution>

std::vector<int> v(1000000);

// 顺序执行
std::sort(v.begin(), v.end());

// 并行执行（C++17）
std::sort(std::execution::par, v.begin(), v.end());

// 并行 + 向量化
std::sort(std::execution::par_unseq, v.begin(), v.end());

// 其他并行算法
std::for_each(std::execution::par, v.begin(), v.end(), [](int& x) { x *= 2; });
std::reduce(std::execution::par, v.begin(), v.end());  // 并行求和
```

---

# 五、C++20 特性

C++20 是继 C++11 之后的又一次大版本更新。

---

## 5.1 Concepts — 概念（模板约束）

**解决的问题**：模板错误信息难以理解，无法约束模板参数。

```cpp
#include <concepts>

// 定义概念
template<typename T>
concept Addable = requires(T a, T b) {
    { a + b } -> std::convertible_to<T>;
};

// 使用概念约束模板
template<Addable T>
T add(T a, T b) {
    return a + b;
}

add(1, 2);          // OK
add("hello", "w");  // 编译错误，const char* 不满足 Addable

// 标准库提供的概念
template<std::integral T>       // T 必须是整数类型
void func1(T val) {}

template<std::floating_point T> // T 必须是浮点类型
void func2(T val) {}

// requires 子句
template<typename T>
    requires std::is_default_constructible_v<T>
void func3(T val) {}
```

---

## 5.2 Ranges — 范围库

**解决的问题**：STL 算法需要传 begin/end 迭代器对，不够简洁；无法链式调用。

```cpp
#include <ranges>

std::vector<int> v = {5, 3, 1, 4, 2, 8, 6, 7};

// C++17
std::vector<int> result;
std::copy_if(v.begin(), v.end(), std::back_inserter(result),
             [](int x) { return x % 2 == 0; });
std::sort(result.begin(), result.end());
std::transform(result.begin(), result.end(), result.begin(),
               [](int x) { return x * x; });

// C++20 Ranges：链式调用，惰性求值
auto result = v
    | std::views::filter([](int x) { return x % 2 == 0; })  // 过滤偶数
    | std::views::transform([](int x) { return x * x; })    // 平方
    | std::views::take(3);                                    // 取前3个

for (int x : result) {
    std::cout << x << " ";  // 4 16 64
}

// 直接传容器，不再需要 begin/end
std::ranges::sort(v);
auto it = std::ranges::find(v, 42);
```

---

## 5.3 Coroutines — 协程

**解决的问题**：异步编程中回调地狱，让异步代码看起来像同步。

```cpp
#include <coroutine>

// 协程生成器
generator<int> fibonacci() {
    int a = 0, b = 1;
    while (true) {
        co_yield a;          // 挂起，返回 a
        auto temp = a + b;
        a = b;
        b = temp;
    }
}

// 使用
for (int x : fibonacci() | std::views::take(10)) {
    std::cout << x << " ";  // 0 1 1 2 3 5 8 13 21 34
}

// 异步任务
Task<std::string> fetchData(std::string url) {
    auto response = co_await httpGet(url);  // 挂起等待
    co_return response.body();              // 返回结果
}
```

**三个关键字**：
- `co_await`：挂起协程，等待异步操作完成
- `co_yield`：挂起协程，返回一个值（生成器）
- `co_return`：完成协程，返回最终结果

---

## 5.4 Modules — 模块

**解决的问题**：`#include` 是文本替换，编译慢，宏污染。

```cpp
// math.cppm（模块接口文件）
export module math;

export int add(int a, int b) {
    return a + b;
}

export class Calculator {
public:
    int multiply(int a, int b) { return a * b; }
};

// main.cpp
import math;  // 导入模块，不是文本替换

int main() {
    add(1, 2);
    Calculator calc;
    calc.multiply(3, 4);
}
```

**Modules vs #include**：

| 特性 | #include | import |
|------|---------|--------|
| 机制 | 文本替换 | 语义导入 |
| 编译速度 | 慢（重复解析） | 快（预编译） |
| 宏泄漏 | 有 | 无 |
| 顺序依赖 | 有 | 无 |

---

## 5.5 三路比较运算符 <=>（spaceship operator）

**解决的问题**：为了支持排序，需要写 6 个比较运算符。

```cpp
#include <compare>

struct Point {
    int x, y;

    // 一个运算符 = 六个运算符 (==, !=, <, >, <=, >=)
    auto operator<=>(const Point&) const = default;
};

Point a{1, 2}, b{3, 4};
if (a < b)  { }  // true
if (a == b) { }  // false
if (a != b) { }  // true

// 自定义比较逻辑
struct Person {
    std::string name;
    int age;

    auto operator<=>(const Person& other) const {
        if (auto cmp = name <=> other.name; cmp != 0)
            return cmp;
        return age <=> other.age;
    }
    bool operator==(const Person&) const = default;
};
```

---

## 5.6 std::format — 格式化字符串

**解决的问题**：printf 不类型安全，stringstream 太啰嗦。

```cpp
#include <format>

// 类型安全的格式化
std::string s = std::format("Hello, {}! You are {} years old.", "Alice", 25);
// "Hello, Alice! You are 25 years old."

// 位置参数
std::string s2 = std::format("{1} comes before {0}", "world", "hello");
// "hello comes before world"

// 格式控制
std::format("{:.2f}", 3.14159);    // "3.14"
std::format("{:08x}", 255);        // "000000ff"
std::format("{:>10}", "hi");       // "        hi"
std::format("{:*^10}", "hi");      // "****hi****"
```

---

## 5.7 std::span — 连续内存视图

**解决的问题**：统一 C 数组、`std::array`、`std::vector` 的函数接口。

```cpp
#include <span>

// 一个函数接受所有连续内存容器
void process(std::span<int> data) {
    for (int x : data) {
        std::cout << x << " ";
    }
}

int c_arr[] = {1, 2, 3};
std::vector<int> vec = {4, 5, 6};
std::array<int, 3> arr = {7, 8, 9};

process(c_arr);  // OK
process(vec);    // OK
process(arr);    // OK

// 子视图
process(std::span(vec).subspan(1, 2));  // 5 6
```

---

# 六、面试高频对比总结

## 各版本核心特性一览

| 版本 | 核心特性 | 一句话总结 |
|------|---------|-----------|
| **C++11** | auto, lambda, move, 智能指针, 线程库 | 现代 C++ 的起点 |
| **C++14** | 泛型 lambda, make_unique, 放松 constexpr | C++11 的补丁 |
| **C++17** | 结构化绑定, optional, string_view, filesystem | 实用工具集 |
| **C++20** | Concepts, Ranges, Coroutines, Modules | 又一次革命 |

## 面试必问对比

### smart pointer 对比

| | unique_ptr | shared_ptr | weak_ptr |
|--|-----------|-----------|----------|
| 所有权 | 独占 | 共享（引用计数） | 无（观察者） |
| 拷贝 | 禁止 | 允许 | 允许 |
| 性能开销 | 约等于裸指针 | 原子引用计数 | 很小 |
| 典型场景 | 工厂模式、PIMPL | 多处共享资源 | 缓存、打破循环引用 |

### 值类别对比

| | 左值 (lvalue) | 右值 (rvalue) |
|--|--------------|--------------|
| 定义 | 有名字、有地址 | 临时对象、无名 |
| 例子 | 变量 `a`、`*ptr` | `42`、`a+b`、`std::move(a)` |
| 引用 | `T&` | `T&&` |
| 用途 | 正常使用 | 移动语义优化 |

### 锁对比

| | lock_guard | unique_lock | shared_lock (C++17) |
|--|-----------|------------|----------|
| 灵活性 | 最低 | 高 | 高 |
| 手动解锁 | 不支持 | 支持 | 支持 |
| 配合 cv | 不支持 | 支持 | — |
| 读写 | 互斥 | 互斥 | 共享读 |

### 容器对比

| | map | unordered_map |
|--|-----|---------------|
| 底层 | 红黑树 | 哈希表 |
| 查找 | O(log n) | O(1) 平均 |
| 有序 | 是 | 否 |
| 迭代器 | 稳定 | rehash 失效 |

### 编译期 vs 运行时

| | const | constexpr | consteval (C++20) | constinit (C++20) |
|--|-------|-----------|-----------|-----------|
| 语义 | 不可修改 | 编译期可求值 | 必须编译期 | 编译期初始化 |
| 函数 | 不能修饰 | 可以 | 可以 | 不能 |
| 运行时调用 | — | 允许 | 不允许 | — |

---

## 项目中 C++ 新特性使用清单

| 特性 | 使用位置 | 用途 |
|------|---------|------|
| `std::function` | Callbacks.h, EventLoop.h | 统一回调类型 |
| `std::shared_ptr` | TcpConnection, Thread | 共享生命周期管理 |
| `std::unique_ptr` | EventLoop (Poller, Channel) | 独占资源管理 |
| `std::weak_ptr` | Channel::tie_ | 防止回调时对象已销毁 |
| `lambda` | HttpServer 路由, EventLoop 任务 | 简洁的回调定义 |
| `std::move` | EventLoop::queueInLoop | 避免回调拷贝 |
| `std::atomic` | EventLoop, TcpConnection, Thread | 无锁状态标志 |
| `std::mutex` | EventLoop, AsyncLogger | 保护共享数据 |
| `std::condition_variable` | AsyncLogger | 缓冲区满/超时通知 |
| `std::thread` | Thread, AsyncLogger | 多线程 |
| `std::chrono` | AsyncLogger | 超时等待、时间戳 |
| `enum class` | LogLevel, HttpStatusCode | 强类型枚举 |
| `using` 别名 | Callbacks.h, EventLoop.h | 简洁类型别名 |
| `override` | EPollPoller | 安全虚函数重写 |
| `= default/delete` | noncopyable | 控制特殊成员函数 |
| `enable_shared_from_this` | TcpConnection | 安全获取自身 shared_ptr |
| 结构化绑定 (C++17) | HttpServer, ServiceCatalog | 简洁的 map 遍历 |
| inline 变量 (C++17) | Timer::nextId_ | 头文件静态成员定义 |

---

> 本文档基于 mymuduo-http 项目实际源码，配合标准示例，覆盖 C++11/14/17/20 核心特性。
> 最后更新：2026-04-09
