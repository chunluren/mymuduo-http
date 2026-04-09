# C++ 面试深度指南

> 覆盖 C++11/14/17/20 核心特性、STL 内幕、内存管理、模板元编程，面向中高级 C++ 岗位面试。

---

## 目录

- [一、C++11 语言特性深度解析](#一c11-语言特性深度解析)
- [二、C++11 标准库深度](#二c11-标准库深度)
- [三、C++14 特性](#三c14-特性)
- [四、C++17 特性深度](#四c17-特性深度)
- [五、C++20 特性深度](#五c20-特性深度)
- [六、STL 容器与算法内幕](#六stl-容器与算法内幕)
- [七、内存管理专题](#七内存管理专题)
- [八、模板与泛型编程](#八模板与泛型编程)
- [九、面试高频对比总结](#九面试高频对比总结)

---

# 一、C++11 语言特性深度解析

---

## 1.1 auto / decltype / 尾置返回类型

### auto 推导规则

```cpp
int x = 10;
const int cx = 20;
const int& rx = x;

auto a = x;       // int          — 忽略顶层 const 和引用
auto b = cx;      // int          — 忽略顶层 const
auto c = rx;      // int          — 忽略引用和 const
auto& d = cx;     // const int&   — 引用保留 const
const auto& e = x;// const int&
auto&& f = x;     // int&         — 万能引用 + 左值 → 折叠为 int&
auto&& g = 42;    // int&&        — 万能引用 + 右值 → int&&
```

**auto 的陷阱**：

```cpp
// 陷阱 1: auto 推导 initializer_list
auto x1 = {1, 2, 3};   // std::initializer_list<int>，不是 vector！
auto x2{42};            // C++11: initializer_list<int>; C++17: int

// 陷阱 2: auto 和代理类
std::vector<bool> v = {true, false};
auto b = v[0];          // std::vector<bool>::reference，不是 bool！
bool b2 = v[0];         // bool，正确

// 陷阱 3: auto 不能用于函数参数（C++11/14）
// void func(auto x);   // C++11/14 错误；C++20 OK（缩写函数模板）
```

### decltype 详细规则

```cpp
int x = 10;
const int& rx = x;

// 规则 1: 变量名 → 声明类型
decltype(x)   a;    // int
decltype(rx)  b = x; // const int&

// 规则 2: 表达式 → 根据值类别决定
decltype((x))  c = x; // int& — (x) 是左值表达式
decltype(x+0)  d;     // int  — x+0 是纯右值
decltype(std::move(x)) e = std::move(x); // int&& — xvalue

// 规则总结:
// 表达式是 lvalue  → decltype 得 T&
// 表达式是 xvalue  → decltype 得 T&&
// 表达式是 prvalue → decltype 得 T
```

### decltype(auto) — C++14

```cpp
// decltype(auto) 完整保留表达式的类型（包括引用和 const）
int x = 10;
decltype(auto) a = x;      // int
decltype(auto) b = (x);    // int& — 注意！加了括号变引用
decltype(auto) c = std::move(x); // int&&

// 用于返回类型推导
template<typename Container, typename Index>
decltype(auto) authAndAccess(Container& c, Index i) {
    return c[i];  // 返回引用而不是值
}
```

### 尾置返回类型

```cpp
// C++11: 返回类型依赖参数类型
template<typename T, typename U>
auto add(T a, U b) -> decltype(a + b) {
    return a + b;
}

// C++14: 可以省略尾置返回类型
template<typename T, typename U>
auto add(T a, U b) {
    return a + b;
}

// 复杂场景仍需尾置返回
auto getSize() -> decltype(std::declval<Container>().size()) {
    return container.size();
}
```

---

## 1.2 lambda 表达式深度

### 闭包的本质

每个 lambda 都会生成一个唯一的匿名类（闭包类型）：

```cpp
int x = 10, y = 20;
auto f = [x, &y](int a) -> int { return x + y + a; };

// 编译器生成的等价代码:
class __lambda_at_line_2 {
    int x;       // 值捕获 → 成员变量
    int& y;      // 引用捕获 → 引用成员
public:
    __lambda_at_line_2(int x, int& y) : x(x), y(y) {}

    int operator()(int a) const {  // 默认是 const！
        return x + y + a;
    }
};
auto f = __lambda_at_line_2(x, y);
```

### 捕获方式详解

```cpp
int a = 1, b = 2, c = 3;

[a]          // 值捕获 a
[&a]         // 引用捕获 a
[=]          // 值捕获所有（隐式）
[&]          // 引用捕获所有（隐式）
[=, &a]      // 默认值捕获，但 a 引用捕获
[&, a]       // 默认引用捕获，但 a 值捕获
[this]       // 捕获 this 指针（成员函数中）
[*this]      // C++17: 拷贝 *this 对象（值捕获整个对象）
[x = std::move(a)]  // C++14: 初始化捕获（移动捕获）
```

### mutable lambda

```cpp
int x = 10;
auto f = [x]() mutable {
    x++;         // 可以修改值捕获的变量（修改的是副本）
    return x;
};
f();             // 返回 11
f();             // 返回 12（闭包内的 x 持续存在）
std::cout << x;  // 10（原始 x 不变）

// 不加 mutable:
auto g = [x]() {
    // x++;       // 编译错误！operator() 是 const 的
    return x;
};
```

### 捕获的陷阱

```cpp
// 陷阱 1: 悬空引用
std::function<int()> createLambda() {
    int local = 42;
    return [&local]() { return local; };  // 危险！local 已销毁
}

// 陷阱 2: this 捕获的生命周期
class Widget {
    int value_ = 42;
public:
    auto getCallback() {
        return [this]() { return value_; };  // this 可能被删除
    }
    // 更安全的做法:
    auto getSafeCallback() {
        return [self = shared_from_this()]() { return self->value_; };
    }
    // C++17: 拷贝整个对象
    auto getCopyCallback() {
        return [*this]() { return value_; };
    }
};

// 陷阱 3: [=] 不会捕获静态变量和全局变量
static int s = 10;
auto f = [=]() { return s; };  // s 不被捕获，直接引用
s = 20;
f();  // 返回 20，不是 10！

// 陷阱 4: [=] 在成员函数中捕获的是 this，不是成员变量
class Foo {
    int x_ = 42;
    auto getLambda() {
        return [=]() { return x_; };
        // 等价于 [this]() { return this->x_; }
        // 不是值捕获 x_！
    }
};
```

### C++14 初始化捕获（广义捕获）

```cpp
// 移动捕获
auto ptr = std::make_unique<int>(42);
auto f = [p = std::move(ptr)]() { return *p; };
// ptr 现在为空，p 拥有对象

// 重命名捕获
int longVariableName = 42;
auto g = [x = longVariableName]() { return x; };

// 表达式捕获
auto h = [x = a + b]() { return x; };
```

### lambda 与 std::function 的性能

```cpp
// lambda 直接调用 — 编译器可以内联，零开销
auto f = [](int x) { return x * 2; };
f(42);  // 可能直接变成 84

// std::function 包装 — 类型擦除，有间接调用开销
std::function<int(int)> g = [](int x) { return x * 2; };
g(42);  // 虚函数表查找 + 间接调用

// 性能对比:
// lambda 直接调用: ~0ns（内联）
// std::function: ~10-20ns（堆分配 + 虚调用）
// 小 lambda（< 16-32 bytes）: std::function 可能用 SBO（小缓冲优化），无堆分配
```

---

## 1.3 右值引用 & move 语义深度

### 值类别体系

C++11 把表达式分为五种值类别（实际常用三种）：

```
          expression
          /       \
       glvalue   rvalue
       /    \    /    \
    lvalue  xvalue  prvalue
```

```cpp
int x = 10;

// lvalue (左值): 有身份，不可移动
x;                // 变量名
*ptr;             // 解引用
arr[0];           // 下标
"hello";          // 字符串字面量（const char[6] 类型，有地址）

// prvalue (纯右值): 无身份，可移动
42;               // 字面量
x + 1;            // 临时值
int();            // 默认构造临时对象
[](int a) {};     // lambda 表达式

// xvalue (将亡值): 有身份，可移动
std::move(x);     // 显式移动
static_cast<int&&>(x);
func_returning_rref();  // 返回 && 的函数
```

### 移动语义的本质

```cpp
class String {
    char* data_;
    size_t size_;
public:
    // 拷贝构造: 深拷贝，分配新内存
    String(const String& other)
        : data_(new char[other.size_])
        , size_(other.size_)
    {
        memcpy(data_, other.data_, size_);  // O(n) 拷贝
    }

    // 移动构造: 偷资源，不分配新内存
    String(String&& other) noexcept
        : data_(other.data_)     // 偷走指针
        , size_(other.size_)
    {
        other.data_ = nullptr;   // 让原对象放手
        other.size_ = 0;
    }

    // 移动赋值
    String& operator=(String&& other) noexcept {
        if (this != &other) {
            delete[] data_;          // 释放自己的资源
            data_ = other.data_;     // 偷走
            size_ = other.size_;
            other.data_ = nullptr;   // 原对象放手
            other.size_ = 0;
        }
        return *this;
    }
};
```

### 五法则（Rule of Five）

> 如果你定义了析构函数、拷贝构造、拷贝赋值、移动构造、移动赋值中的任何一个，
> 你通常应该定义所有五个。

```cpp
class Resource {
public:
    ~Resource();                                   // 析构
    Resource(const Resource&);                     // 拷贝构造
    Resource& operator=(const Resource&);          // 拷贝赋值
    Resource(Resource&&) noexcept;                 // 移动构造
    Resource& operator=(Resource&&) noexcept;      // 移动赋值
};

// 零法则（Rule of Zero）: 如果用智能指针管理资源，五个都不需要
class Better {
    std::unique_ptr<char[]> data_;  // 自动管理
    // 不需要写任何特殊成员函数！
};
```

### RVO / NRVO — 返回值优化

```cpp
String createString() {
    String s("hello");
    return s;  // NRVO: 直接在调用者的内存上构造，无拷贝无移动
}

String s = createString();  // 可能零次拷贝/移动

// 编译器优化级别:
// 1. NRVO (Named RVO): 有名字的局部变量直接返回
// 2. RVO: 返回临时对象 return String("hello");
// 3. 移动: RVO 失败时回退到移动构造
// 4. 拷贝: 移动不可用时回退到拷贝构造

// C++17: 保证的 copy elision (强制 RVO)
String s = String("hello");  // C++17 保证不会调用任何构造函数（除了 String(const char*)）
```

**什么情况下 NRVO 会失败？**

```cpp
String create(bool flag) {
    String a("hello");
    String b("world");
    if (flag) return a;  // NRVO 失败！有多个可能的返回对象
    else return b;       // 编译器不知道优化哪个
    // 回退到移动构造
}

String create2() {
    String s("hello");
    return std::move(s);  // 别这样！会阻止 NRVO！
    // std::move 让 s 变成右值引用，编译器不再做 NRVO
    // 正确做法: return s;
}
```

### 引用折叠规则

```cpp
// T&&: 万能引用（仅在模板推导上下文中）
template<typename T>
void func(T&& arg);

// 引用折叠规则:
// T = int     → T&& = int&&    (右值引用)
// T = int&    → T&& = int&     (左值引用)  — & + && = &
// T = int&&   → T&& = int&&    (右值引用)  — && + && = &&

// 总结: 只要有一个 & 就折叠为 &

func(42);    // T = int,    arg 类型: int&&
int x = 10;
func(x);     // T = int&,   arg 类型: int& (折叠)
func(std::move(x)); // T = int, arg 类型: int&&
```

### 完美转发

```cpp
template<typename T>
void wrapper(T&& arg) {
    // 不用 forward: arg 总是左值（有名字就是左值）
    target(arg);  // 总是调用 target(lvalue)

    // 用 forward: 保持原始值类别
    target(std::forward<T>(arg));
    // 如果 T = int&  → forward 返回 int&  (左值)
    // 如果 T = int   → forward 返回 int&& (右值)
}

// forward 的实现原理
template<typename T>
T&& forward(std::remove_reference_t<T>& arg) noexcept {
    return static_cast<T&&>(arg);
    // T = int   → static_cast<int&&>(arg)   → 右值
    // T = int&  → static_cast<int& &&>(arg) → 折叠为 int& → 左值
}

// 实际用途: emplace 系列函数
template<typename... Args>
void emplace_back(Args&&... args) {
    // 完美转发所有参数给构造函数
    new (ptr) T(std::forward<Args>(args)...);
}
```

### noexcept 与移动

```cpp
// 移动构造和移动赋值应该标记 noexcept
// 原因: STL 容器（如 vector 扩容）只在 noexcept 时才用移动

class Foo {
public:
    Foo(Foo&& other) noexcept { ... }  // ✓ vector 扩容用移动
    // Foo(Foo&& other) { ... }        // ✗ vector 扩容用拷贝
};

// vector 扩容策略:
// 1. 如果元素的移动构造是 noexcept → 移动元素（高效）
// 2. 否则 → 拷贝元素（安全但慢）
// 原因: 移动一半时如果抛异常，已移动的元素无法恢复
//       拷贝则可以丢弃新空间，保持原数据完整
```

---

## 1.4 智能指针深度

### shared_ptr 的控制块

```
shared_ptr<T> 内部结构:
┌──────────────┐
│ T* ptr_      │──────────────▶ [实际对象 T]
│ CtrlBlock*   │──┐
└──────────────┘  │
                  ▼
         ┌───────────────────┐
         │    Control Block   │
         │ ┌───────────────┐ │
         │ │ use_count (强) │ │  ← shared_ptr 数量
         │ │ weak_count (弱)│ │  ← weak_ptr 数量 + (use_count > 0 ? 1 : 0)
         │ │ deleter        │ │  ← 自定义删除器
         │ │ allocator      │ │  ← 分配器
         │ └───────────────┘ │
         └───────────────────┘
```

### make_shared vs new

```cpp
// 方式 1: new
auto sp = std::shared_ptr<Foo>(new Foo(args...));
// 两次内存分配:
// 1. new Foo()        → 分配对象
// 2. shared_ptr 内部  → 分配控制块

// 方式 2: make_shared（推荐）
auto sp = std::make_shared<Foo>(args...);
// 一次内存分配:
// ┌────────────┬──────────┐
// │ Control Block │  Foo 对象 │  ← 一块连续内存
// └────────────┴──────────┘

// make_shared 的优势:
// 1. 性能: 一次分配 vs 两次分配
// 2. 缓存友好: 对象和控制块在一起
// 3. 异常安全: 不会出现内存泄漏

// make_shared 的劣势:
// 1. 不支持自定义删除器
// 2. 对象和控制块一起分配，weak_ptr 存在时整块内存无法释放
//    → 如果对象很大且有长期存活的 weak_ptr，可能浪费内存
// 3. 不能用 {} 初始化（没有 initializer_list 版本）
```

### enable_shared_from_this 原理

```cpp
class Widget : public std::enable_shared_from_this<Widget> {
public:
    std::shared_ptr<Widget> getPtr() {
        return shared_from_this();  // 安全获取自身的 shared_ptr
    }
};

// 为什么不能直接 shared_ptr<Widget>(this)?
Widget* raw = new Widget();
auto sp1 = std::shared_ptr<Widget>(raw);
auto sp2 = std::shared_ptr<Widget>(raw);  // 两个独立控制块！
// sp1 析构 → delete raw
// sp2 析构 → delete raw 又一次！ → double free！

// enable_shared_from_this 的原理:
// 内部有一个 weak_ptr<Widget> 成员
// 第一次创建 shared_ptr 时自动初始化这个 weak_ptr
// shared_from_this() 就是 weak_ptr.lock()

// 使用注意:
// 1. 对象必须由 shared_ptr 管理（不能在栈上）
// 2. 不能在构造函数中调用 shared_from_this（此时 weak_ptr 还没初始化）
```

### unique_ptr 高级用法

```cpp
// 自定义删除器
auto fileDeleter = [](FILE* fp) {
    if (fp) fclose(fp);
};
std::unique_ptr<FILE, decltype(fileDeleter)> file(
    fopen("test.txt", "r"), fileDeleter);

// unique_ptr 管理 C 资源
struct MallocDeleter {
    void operator()(void* ptr) { free(ptr); }
};
std::unique_ptr<int, MallocDeleter> p(static_cast<int*>(malloc(sizeof(int))));

// unique_ptr 管理数组
auto arr = std::make_unique<int[]>(10);  // C++14
arr[0] = 42;

// unique_ptr 和多态
class Base { public: virtual ~Base() = default; };
class Derived : public Base {};
std::unique_ptr<Base> p = std::make_unique<Derived>();  // OK

// unique_ptr 转 shared_ptr（但不能反过来！）
std::unique_ptr<Foo> up = std::make_unique<Foo>();
std::shared_ptr<Foo> sp = std::move(up);  // OK，up 变空
```

### weak_ptr 使用场景

```cpp
// 场景 1: 打破循环引用
struct Node {
    std::shared_ptr<Node> next;
    std::weak_ptr<Node> prev;  // 弱引用打破环
};

// 场景 2: 观察者模式 / 缓存
class Cache {
    std::unordered_map<int, std::weak_ptr<Data>> cache_;
public:
    std::shared_ptr<Data> get(int key) {
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            if (auto sp = it->second.lock()) {
                return sp;  // 缓存命中，对象还活着
            }
            cache_.erase(it);  // 对象已销毁，清理缓存
        }
        auto data = std::make_shared<Data>(key);
        cache_[key] = data;
        return data;
    }
};

// 场景 3: 多线程中安全地引用可能被销毁的对象
// (本项目 Channel::tie_ 就是这个用法)
```

---

## 1.5 类型萃取 & SFINAE

### type_traits 常用工具

```cpp
#include <type_traits>

// 类型判断
std::is_integral_v<int>          // true
std::is_floating_point_v<double> // true
std::is_pointer_v<int*>          // true
std::is_reference_v<int&>        // true
std::is_const_v<const int>       // true
std::is_same_v<int, int32_t>     // true (通常)
std::is_base_of_v<Base, Derived> // true
std::is_trivially_copyable_v<T>  // memcpy 安全？

// 类型变换
std::remove_const_t<const int>        // int
std::remove_reference_t<int&>         // int
std::remove_pointer_t<int*>           // int
std::add_const_t<int>                 // const int
std::add_lvalue_reference_t<int>      // int&
std::decay_t<const int&>             // int — 模拟传值衰减
std::conditional_t<true, int, double> // int — 编译期三目运算
std::common_type_t<int, double>       // double — 公共类型

// 类型能力
std::is_default_constructible_v<T>
std::is_copy_constructible_v<T>
std::is_move_constructible_v<T>
std::is_nothrow_move_constructible_v<T>  // vector 扩容用这个判断
```

### SFINAE — Substitution Failure Is Not An Error

```cpp
// SFINAE: 模板参数替换失败不是错误，只是排除该重载

// 方式 1: enable_if
template<typename T>
typename std::enable_if<std::is_integral_v<T>, T>::type
double_it(T val) {
    return val * 2;
}

template<typename T>
typename std::enable_if<std::is_floating_point_v<T>, T>::type
double_it(T val) {
    return val * 2.0;
}

// 方式 2: enable_if 放在模板参数中（更简洁）
template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
T double_it(T val) {
    return val * 2;
}

// 方式 3: C++17 if constexpr（最好的替代）
template<typename T>
T double_it(T val) {
    if constexpr (std::is_integral_v<T>) {
        return val * 2;
    } else {
        return val * 2.0;
    }
}

// 方式 4: C++20 concepts（最终方案）
template<std::integral T>
T double_it(T val) { return val * 2; }
```

### 检测成员是否存在

```cpp
// C++11/14: SFINAE 检测
template<typename T, typename = void>
struct has_size : std::false_type {};

template<typename T>
struct has_size<T, std::void_t<decltype(std::declval<T>().size())>>
    : std::true_type {};

// 使用
static_assert(has_size<std::vector<int>>::value);  // true
static_assert(!has_size<int>::value);               // true

// C++20: requires 表达式（简单太多了）
template<typename T>
concept HasSize = requires(T t) {
    { t.size() } -> std::convertible_to<std::size_t>;
};
```

---

## 1.6 可变参数模板深度

### 递归展开

```cpp
// 终止条件
void print() {
    std::cout << std::endl;
}

// 递归展开
template<typename T, typename... Args>
void print(T first, Args... rest) {
    std::cout << first << " ";
    print(rest...);  // 参数包缩减
}

print(1, "hello", 3.14);
// 展开过程:
// print(1, "hello", 3.14) → cout<<1; print("hello", 3.14)
// print("hello", 3.14)    → cout<<"hello"; print(3.14)
// print(3.14)             → cout<<3.14; print()
// print()                 → cout<<endl;  ← 终止
```

### sizeof... 和包展开

```cpp
template<typename... Args>
void info(Args... args) {
    std::cout << sizeof...(Args) << std::endl;  // 类型参数数量
    std::cout << sizeof...(args) << std::endl;  // 值参数数量
}

// 包展开模式
template<typename... Args>
auto sum(Args... args) {
    return (args + ...);  // C++17 折叠表达式
}

// 展开到函数调用
template<typename... Args>
void callAll(Args&&... args) {
    // 每个参数都调用 process()
    (process(std::forward<Args>(args)), ...);  // C++17 逗号折叠
}

// C++11 展开技巧（没有折叠表达式时）
template<typename... Args>
void callAll11(Args&&... args) {
    int dummy[] = {0, (process(std::forward<Args>(args)), 0)...};
    (void)dummy;
}
```

### std::tuple 与参数包

```cpp
// tuple 存储参数包
template<typename... Args>
auto capture(Args&&... args) {
    return std::make_tuple(std::forward<Args>(args)...);
}

// std::apply: 把 tuple 展开为函数参数
auto t = std::make_tuple(1, 2.0, "hello");
std::apply([](auto&&... args) {
    ((std::cout << args << " "), ...);
}, t);
// 输出: 1 2 hello

// 实现 make_from_tuple
template<typename T, typename Tuple, size_t... I>
T make_from_tuple_impl(Tuple&& t, std::index_sequence<I...>) {
    return T(std::get<I>(std::forward<Tuple>(t))...);
}
```

---

## 1.7 constexpr 深度

### 编译期 vs 运行期

```cpp
constexpr int factorial(int n) {
    return n <= 1 ? 1 : n * factorial(n - 1);
}

// 编译期
constexpr int a = factorial(5);   // 编译期求值，等同于 constexpr int a = 120;
static_assert(factorial(5) == 120); // 编译期断言

// 运行期
int n;
std::cin >> n;
int b = factorial(n);  // 运行期求值，和普通函数一样
```

### constexpr 的演进

```cpp
// C++11: 只能有一条 return 语句
constexpr int square11(int x) {
    return x * x;
}

// C++14: 放松限制，可以有循环、局部变量、条件语句
constexpr int factorial14(int n) {
    int result = 1;
    for (int i = 2; i <= n; ++i) {
        result *= i;
    }
    return result;
}

// C++17: if constexpr — 编译期分支
template<typename T>
constexpr auto process(T val) {
    if constexpr (std::is_integral_v<T>) {
        return val * 2;
    } else {
        return val;
    }
}

// C++20: consteval — 必须编译期求值
consteval int must_compile_time(int n) {
    return n * n;
}
constexpr int a = must_compile_time(5);  // OK
// int x = 5; must_compile_time(x);      // 错误！必须编译期

// C++20: constinit — 编译期初始化，运行期可修改
constinit int global = factorial14(5);   // 编译期初始化 = 120
// global = 200;  // 运行期可修改（和 constexpr 不同）
```

---

## 1.8 其他 C++11 重要特性

### enum class

```cpp
// 传统 enum 的问题
enum Color { RED, GREEN };      // RED, GREEN 污染命名空间
enum Fruit { APPLE, ORANGE };
// int x = RED;                 // 隐式转换为 int
// if (RED == APPLE) {}         // 居然能比较！语义上不合理

// enum class 解决
enum class Color : uint8_t { RED, GREEN, BLUE };  // 可指定底层类型
enum class Fruit { APPLE, ORANGE };
// int x = Color::RED;          // 错误！不能隐式转换
int x = static_cast<int>(Color::RED);  // 必须显式
// Color::RED == Fruit::APPLE;  // 错误！不同类型不能比较
```

### override / final

```cpp
class Base {
public:
    virtual void foo(int) {}
    virtual void bar() const {}
    virtual void baz() {}
};

class Derived : public Base {
public:
    void foo(int) override {}        // OK
    // void foo(double) override {}   // 编译错误！签名不匹配
    // void bar() override {}         // 编译错误！缺少 const
    void baz() override final {}     // OK，且不允许再被重写
};

class Final final { };               // 整个类禁止继承
// class X : public Final {};        // 编译错误！
```

### 委托构造 & 继承构造

```cpp
// 委托构造
class Server {
    int port_;
    int threads_;
    bool ssl_;
public:
    Server(int port, int threads, bool ssl)
        : port_(port), threads_(threads), ssl_(ssl)
    {
        // 公共初始化逻辑
    }
    Server() : Server(8080, 4, false) {}          // 委托
    Server(int port) : Server(port, 4, false) {}  // 委托
};

// 继承构造 (C++11)
class Base {
public:
    Base(int x) {}
    Base(int x, double y) {}
};

class Derived : public Base {
public:
    using Base::Base;  // 继承 Base 的所有构造函数
};

Derived d1(42);        // 调用 Base(int)
Derived d2(42, 3.14);  // 调用 Base(int, double)
```

### 用户自定义字面量

```cpp
// 自定义后缀运算符
constexpr long double operator"" _km(long double val) {
    return val * 1000.0;  // 转为米
}

constexpr long double operator"" _m(long double val) {
    return val;
}

auto distance = 5.0_km;  // 5000.0
auto height = 1.8_m;     // 1.8

// 标准库的字面量
using namespace std::chrono_literals;
auto timeout = 100ms;   // std::chrono::milliseconds(100)
auto delay = 2s;         // std::chrono::seconds(2)

using namespace std::string_literals;
auto s = "hello"s;       // std::string，不是 const char*
```

---

# 二、C++11 标准库深度

---

## 2.1 std::function — 类型擦除原理

```cpp
// std::function 可以包装任何可调用对象
std::function<int(int, int)> f;

f = [](int a, int b) { return a + b; };  // lambda
f = std::plus<int>();                      // 仿函数
f = &add;                                  // 函数指针

// 内部原理: 类型擦除 (type erasure)
// 简化版实现:
template<typename R, typename... Args>
class Function<R(Args...)> {
    struct Concept {                       // 抽象基类
        virtual R call(Args...) = 0;
        virtual ~Concept() = default;
    };

    template<typename F>
    struct Model : Concept {               // 具体包装
        F func_;
        Model(F f) : func_(std::move(f)) {}
        R call(Args... args) override {
            return func_(args...);
        }
    };

    std::unique_ptr<Concept> impl_;       // 多态指针

public:
    template<typename F>
    Function(F f) : impl_(std::make_unique<Model<F>>(std::move(f))) {}

    R operator()(Args... args) {
        return impl_->call(args...);       // 虚函数调用
    }
};
```

**SBO（Small Buffer Optimization）**：

```
// 小对象直接存在 function 内部，避免堆分配
// 通常 16-32 bytes（各编译器不同）

std::function 内部:
┌─────────────────────────────────────┐
│ [vtable ptr]  [SBO buffer (16-32B)] │  ← 小 lambda 放这里
│       或                             │
│ [vtable ptr]  [heap ptr ──▶ 堆上对象]│  ← 大 lambda 堆分配
└─────────────────────────────────────┘
```

---

## 2.2 std::atomic & 内存序

### 六种内存序

```cpp
#include <atomic>

std::atomic<int> x{0}, y{0};

// 1. memory_order_relaxed — 只保证原子性，不保证顺序
x.store(1, std::memory_order_relaxed);
y.load(std::memory_order_relaxed);
// 用途: 计数器、统计数据

// 2. memory_order_acquire — 读屏障
// 保证: 此操作之后的读写不会被重排到此操作之前
y.load(std::memory_order_acquire);
// 常见搭配: 和 release 配对

// 3. memory_order_release — 写屏障
// 保证: 此操作之前的读写不会被重排到此操作之后
x.store(1, std::memory_order_release);
// 常见搭配: 和 acquire 配对

// 4. memory_order_acq_rel — 读写屏障
// 同时具备 acquire 和 release 语义
x.fetch_add(1, std::memory_order_acq_rel);

// 5. memory_order_seq_cst — 顺序一致性（默认）
// 最强保证: 所有线程看到相同的操作顺序
x.store(1);  // 默认就是 seq_cst

// 6. memory_order_consume — 数据依赖序（几乎不用）
```

### acquire-release 实战

```cpp
// 经典模式: 生产者-消费者
std::atomic<bool> ready{false};
int data = 0;

// 生产者
void producer() {
    data = 42;                                    // 普通写
    ready.store(true, std::memory_order_release);  // release 写
    // 保证: data = 42 在 ready = true 之前对其他线程可见
}

// 消费者
void consumer() {
    while (!ready.load(std::memory_order_acquire)) {}  // acquire 读
    // 保证: 看到 ready == true 时，一定能看到 data == 42
    assert(data == 42);  // 一定成功
}
```

### CAS（Compare And Swap）

```cpp
std::atomic<int> counter{0};

// 无锁自增
void increment() {
    int old = counter.load();
    while (!counter.compare_exchange_weak(old, old + 1)) {
        // CAS 失败: old 被更新为当前值，重试
    }
}

// compare_exchange_weak vs strong
// weak: 可能虚假失败（在某些架构上），适合循环中使用
// strong: 不会虚假失败，但可能更慢
// 规则: 循环中用 weak，单次判断用 strong
```

---

## 2.3 线程同步全家族

```cpp
// mutex 家族
std::mutex                  // 基本互斥锁
std::recursive_mutex        // 可重入锁（同一线程可多次加锁）
std::timed_mutex           // 带超时的锁
std::shared_mutex          // C++17 读写锁

// RAII 锁管理
std::lock_guard<std::mutex>   // 最简单，构造加锁，析构解锁
std::unique_lock<std::mutex>  // 灵活，支持延迟加锁、手动解锁、配合 cv
std::shared_lock<std::shared_mutex> // C++17 读锁
std::scoped_lock              // C++17 同时锁多个 mutex，防死锁

// 读写锁示例
std::shared_mutex rw_mutex;

void reader() {
    std::shared_lock lock(rw_mutex);  // 多个 reader 可并发
    // 读操作...
}

void writer() {
    std::unique_lock lock(rw_mutex);  // writer 独占
    // 写操作...
}

// scoped_lock 防死锁
std::mutex m1, m2;
void safe() {
    std::scoped_lock lock(m1, m2);  // 自动避免死锁（内部用 std::lock）
}
```

### condition_variable 细节

```cpp
std::mutex mtx;
std::condition_variable cv;
std::queue<int> queue;

// 生产者
void produce(int val) {
    {
        std::lock_guard lock(mtx);
        queue.push(val);
    }
    cv.notify_one();  // 在锁外 notify（性能更好）
}

// 消费者
void consume() {
    std::unique_lock lock(mtx);
    cv.wait(lock, [&]{ return !queue.empty(); });
    // wait 等价于:
    // while (!predicate()) {
    //     cv.wait(lock);  // 释放锁 + 阻塞
    //     // 被唤醒后重新获取锁，检查谓词
    // }

    int val = queue.front();
    queue.pop();
}

// 为什么必须用谓词？
// 1. 虚假唤醒: OS 可能在没有 notify 时唤醒线程
// 2. Lost wakeup: notify 在 wait 之前到达
// 3. 多个消费者竞争: 被唤醒但数据已被其他消费者取走
```

---

## 2.4 STL 容器内部实现

### vector

```
内存布局:
┌─────────────────────────────────────────────┐
│  [elem0][elem1][elem2][elem3][ ... 未使用 ...] │
│  ↑                     ↑                    ↑  │
│  begin              size               capacity│
└─────────────────────────────────────────────┘

扩容策略:
- GCC: capacity × 2
- MSVC: capacity × 1.5
- 时间复杂度: 均摊 O(1)

// push_back 扩容过程:
// 1. 分配 2 倍大小的新内存
// 2. 移动（或拷贝）旧元素到新内存
// 3. 在末尾构造新元素
// 4. 释放旧内存
// 5. 更新指针
```

```cpp
// 预分配避免扩容
std::vector<int> v;
v.reserve(10000);  // 预分配，size 不变
v.resize(10000);   // 预分配 + 改变 size

// shrink_to_fit: 释放多余内存
v.shrink_to_fit();  // capacity → size（非强制）

// emplace_back vs push_back
v.push_back(MyObj(1, 2));    // 构造临时对象 + 移动
v.emplace_back(1, 2);        // 直接在 vector 内构造（零临时对象）
```

### deque

```
内存布局: 分段数组 + 中控器
                    map (指针数组)
                   ┌────────┐
                   │ buf[0] ─┼───▶ [elem][elem][elem][elem]
                   │ buf[1] ─┼───▶ [elem][elem][elem][elem]
                   │ buf[2] ─┼───▶ [elem][elem][elem][elem]
                   │ buf[3] ─┼───▶ [    ][    ][elem][elem]  ← 当前头
                   │ buf[4] ─┼───▶ [elem][elem][    ][    ]  ← 当前尾
                   │ buf[5] ─┼───▶ (未分配)
                   └────────┘

特点:
- 两端 O(1) 插入/删除
- 随机访问 O(1)（两次间接寻址）
- 不保证连续内存
```

### map / set — 红黑树

```
红黑树规则:
1. 节点是红色或黑色
2. 根节点是黑色
3. 叶子节点（NIL）是黑色
4. 红色节点的子节点必须是黑色（不能连续红）
5. 从任意节点到叶子的路径上黑色节点数相同

       7(B)
      /    \
    3(R)    11(R)
   / \     / \
  1(B) 5(B) 9(B) 13(B)

查找/插入/删除: O(log n)
有序遍历: 中序遍历红黑树 → 升序
```

### unordered_map — 哈希表

```
哈希表结构（拉链法）:
┌────┐
│ 0  │ → [key1, val1] → [key5, val5] → null
│ 1  │ → null
│ 2  │ → [key2, val2] → null
│ 3  │ → [key3, val3] → [key7, val7] → [key9, val9] → null
│ 4  │ → null
│ ... │
│ n-1│ → [key4, val4] → null
└────┘
  桶(bucket)    链表/红黑树(C++无要求，通常是链表)

关键参数:
- load_factor = size / bucket_count
- max_load_factor = 1.0 (默认)
- 当 load_factor > max_load_factor 时触发 rehash
- rehash: 重新分配桶，重新哈希所有元素 → O(n)
```

```cpp
// 自定义哈希函数
struct MyHash {
    size_t operator()(const MyKey& key) const {
        return std::hash<int>()(key.id) ^ (std::hash<std::string>()(key.name) << 1);
    }
};

struct MyEqual {
    bool operator()(const MyKey& a, const MyKey& b) const {
        return a.id == b.id && a.name == b.name;
    }
};

std::unordered_map<MyKey, int, MyHash, MyEqual> m;
```

### 迭代器失效规则

| 容器 | 插入 | 删除 |
|------|------|------|
| **vector** | 扩容: 全部失效；不扩容: 插入点之后失效 | 删除点之后全部失效 |
| **deque** | 头尾插入: 迭代器失效但引用有效；中间插入: 全部失效 | 头尾删除: 仅首/末失效；中间删除: 全部失效 |
| **list** | 不失效 | 仅被删元素失效 |
| **map/set** | 不失效 | 仅被删元素失效 |
| **unordered_map** | rehash 时全部失效 | 仅被删元素失效 |

---

# 三、C++14 特性

---

## 3.1 泛型 lambda & 返回类型推导

```cpp
// 泛型 lambda — auto 参数
auto add = [](auto a, auto b) { return a + b; };
add(1, 2);           // int
add(1.5, 2.5);       // double
add("hello"s, " world"s); // string

// 等价的模板:
struct AddLambda {
    template<typename T, typename U>
    auto operator()(T a, U b) const { return a + b; }
};

// 返回类型自动推导
auto divide(int a, int b) {
    if (b == 0) return 0;  // 推导为 int
    return a / b;           // 也是 int，OK
}
// 多个 return 必须推导出相同类型
```

## 3.2 std::make_unique & 变量模板

```cpp
// make_unique 补全了 C++11 的缺失
auto p = std::make_unique<Foo>(1, "hello");
auto arr = std::make_unique<int[]>(10);

// 变量模板 (variable template)
template<typename T>
constexpr T pi = T(3.14159265358979323846L);

double d = pi<double>;  // 3.14159265358979...
float f = pi<float>;    // 3.14159...

// 标准库大量使用:
// C++14:  std::is_integral<T>::value
// C++17:  std::is_integral_v<T>  ← 就是变量模板
template<typename T>
inline constexpr bool is_integral_v = is_integral<T>::value;
```

## 3.3 std::shared_timed_mutex & [[deprecated]]

```cpp
// 读写锁 (C++14)
#include <shared_mutex>
std::shared_timed_mutex rw;

// 读操作
{
    std::shared_lock lock(rw);
    // 多个线程可同时读
}

// 写操作
{
    std::unique_lock lock(rw);
    // 独占写
}

// [[deprecated]] 标记
[[deprecated("Use newFunc() instead")]]
void oldFunc() { }
// 调用 oldFunc() 会产生编译警告
```

---

# 四、C++17 特性深度

---

## 4.1 结构化绑定深度

```cpp
// 绑定 pair/tuple
auto [x, y] = std::make_pair(1, "hello");  // x=1, y="hello"

// 绑定数组
int arr[] = {1, 2, 3};
auto [a, b, c] = arr;

// 绑定结构体（要求所有成员 public）
struct Point { double x, y; };
Point p{1.0, 2.0};
auto [px, py] = p;

// 绑定 map 元素
std::map<std::string, int> m = {{"a", 1}, {"b", 2}};
for (const auto& [key, value] : m) {
    // key: const std::string&
    // value: const int&
}

// 绑定引用（修改原数据）
auto& [rx, ry] = p;
rx = 3.0;  // 修改 p.x

// 底层原理: 编译器生成隐藏变量
// auto [a, b] = expr;
// 等价于:
// auto __hidden = expr;
// auto& a = get<0>(__hidden);
// auto& b = get<1>(__hidden);

// 自定义类型支持结构化绑定:
// 需要特化 std::tuple_size, std::tuple_element, 提供 get 函数
```

## 4.2 if constexpr 替代 SFINAE

```cpp
// C++11 SFINAE 写法（复杂难读）
template<typename T>
typename std::enable_if<std::is_integral_v<T>, std::string>::type
toString(T val) {
    return std::to_string(val);
}

template<typename T>
typename std::enable_if<std::is_same_v<T, std::string>, std::string>::type
toString(T val) {
    return val;
}

// C++17 if constexpr（简单清晰）
template<typename T>
std::string toString(T val) {
    if constexpr (std::is_integral_v<T>) {
        return std::to_string(val);
    } else if constexpr (std::is_floating_point_v<T>) {
        return std::to_string(val);
    } else if constexpr (std::is_same_v<T, std::string>) {
        return val;
    } else if constexpr (std::is_same_v<T, const char*>) {
        return std::string(val);
    } else {
        static_assert(always_false<T>, "Unsupported type");
    }
}

// 注意: if constexpr 中未选中的分支不会被实例化
// 这意味着可以在分支中使用该类型不支持的操作
template<typename T>
void process(T val) {
    if constexpr (std::is_pointer_v<T>) {
        *val = 42;  // 只有 T 是指针时才编译
    } else {
        val.doSomething();  // 只有 T 不是指针时才编译
    }
}
```

## 4.3 std::optional / variant / any

### std::optional

```cpp
#include <optional>

// 表示"可能没有值"
std::optional<int> findUser(const std::string& name) {
    if (name == "admin") return 42;
    return std::nullopt;
}

auto result = findUser("admin");

// 访问方式
if (result.has_value()) { ... }
if (result) { ... }              // 隐式 bool 转换
int id = result.value();          // 有值返回值，无值抛 bad_optional_access
int id = result.value_or(-1);     // 有值返回值，无值返回默认值
int id = *result;                 // 有值返回值，无值 UB！

// optional 不用堆分配
// 内部: aligned_storage + bool flag
// sizeof(optional<int>) = sizeof(int) + padding + bool ≈ 8 bytes
```

### std::variant

```cpp
#include <variant>

// 类型安全的 union
std::variant<int, double, std::string> v;

v = 42;          // 存 int
v = 3.14;        // 存 double (int 被销毁)
v = "hello"s;    // 存 string (double 被销毁)

// 访问
std::get<int>(v);          // 类型不匹配 → 抛 bad_variant_access
std::get<2>(v);            // 按索引访问
auto* p = std::get_if<std::string>(&v);  // 返回指针，不匹配返回 nullptr

// visitor 模式（最推荐的访问方式）
std::visit([](auto&& arg) {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, int>) {
        std::cout << "int: " << arg;
    } else if constexpr (std::is_same_v<T, double>) {
        std::cout << "double: " << arg;
    } else if constexpr (std::is_same_v<T, std::string>) {
        std::cout << "string: " << arg;
    }
}, v);

// overloaded 技巧（C++17 常见模式）
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

std::visit(overloaded {
    [](int arg)         { std::cout << "int: " << arg; },
    [](double arg)      { std::cout << "double: " << arg; },
    [](const std::string& arg) { std::cout << "string: " << arg; },
}, v);

// variant vs union:
// variant: 类型安全、自动管理析构、知道当前存的是什么类型
// union: 不安全、不调用析构、不知道当前类型
```

### std::any

```cpp
#include <any>

std::any a = 42;
a = std::string("hello");
a = 3.14;

// 访问
auto val = std::any_cast<double>(a);       // OK: 3.14
// std::any_cast<int>(a);                  // 抛 bad_any_cast
auto* p = std::any_cast<double>(&a);       // 返回指针

// any vs variant:
// any: 任意类型，堆分配（大对象），运行时类型检查
// variant: 固定类型集合，栈上分配，编译时类型检查
// 优先用 variant！
```

## 4.4 std::string_view 与陷阱

```cpp
#include <string_view>

void process(std::string_view sv) {
    sv.substr(0, 5);      // 返回新的 string_view，不拷贝
    sv.remove_prefix(2);   // 移动起始位置，不拷贝
    sv.find("hello");
}

std::string s = "hello world";
process(s);            // 从 string，零拷贝
process("literal");    // 从字面量，零拷贝
process(s.c_str());    // 从 const char*，零拷贝

// 陷阱 1: 悬空引用
std::string_view dangerous() {
    std::string s = "temporary";
    return s;  // 危险！s 销毁后 string_view 指向无效内存
}

// 陷阱 2: string_view 不保证 null 结尾
std::string_view sv = "hello world";
sv.remove_prefix(6);  // sv = "world"
// printf("%s", sv.data());  // 可能打印 "world" 后面的垃圾
// 正确: printf("%.*s", (int)sv.size(), sv.data());

// 陷阱 3: 隐式转换
std::string s = some_string_view;  // OK，但会拷贝（string_view → string）
```

## 4.5 std::filesystem

```cpp
#include <filesystem>
namespace fs = std::filesystem;

// 路径操作
fs::path p = "/home/user/doc/report.pdf";
p.filename();      // "report.pdf"
p.stem();          // "report"
p.extension();     // ".pdf"
p.parent_path();   // "/home/user/doc"
p.root_path();     // "/"
p / "subdir";      // "/home/user/doc/report.pdf/subdir" — operator/

// 文件操作
fs::exists(p);
fs::file_size(p);
fs::is_regular_file(p);
fs::is_directory(p);
fs::create_directories("/tmp/a/b/c");
fs::copy_file("src", "dst", fs::copy_options::overwrite_existing);
fs::remove_all("/tmp/a");
fs::rename("old.txt", "new.txt");

// 遍历目录
for (auto& entry : fs::directory_iterator("/home/user")) {
    if (entry.is_regular_file()) {
        std::cout << entry.path() << ": " << entry.file_size() << "\n";
    }
}

// 递归遍历
for (auto& entry : fs::recursive_directory_iterator("/home/user")) {
    std::cout << entry.path() << "\n";
}

// 空间信息
auto info = fs::space("/home");
info.capacity;   // 总容量
info.free;       // 可用空间
info.available;  // 非 root 用户可用
```

## 4.6 其他 C++17 特性

```cpp
// if/switch 带初始化
if (auto it = m.find(key); it != m.end()) {
    // it 只在 if 作用域内
}

// CTAD (类模板参数推导)
std::pair p(1, 2.0);          // pair<int, double>
std::vector v = {1, 2, 3};    // vector<int>
std::lock_guard lock(mtx);    // lock_guard<std::mutex>

// 折叠表达式
template<typename... Args>
auto sum(Args... args) { return (args + ...); }         // 右折叠
template<typename... Args>
auto sum(Args... args) { return (... + args); }         // 左折叠
template<typename... Args>
auto sum(Args... args) { return (0 + ... + args); }     // 带初值
template<typename... Args>
void printAll(Args... args) { ((std::cout << args << " "), ...); }

// std::invoke — 统一调用
int add(int a, int b) { return a + b; }
struct Foo { int val; void show() {} };

std::invoke(add, 1, 2);        // 普通函数
std::invoke(&Foo::show, foo);   // 成员函数
std::invoke(&Foo::val, foo);    // 成员变量

// std::apply — tuple 展开为参数
auto t = std::make_tuple(1, 2);
std::apply(add, t);  // add(1, 2) = 3

// 并行算法
#include <execution>
std::sort(std::execution::par, v.begin(), v.end());
std::reduce(std::execution::par, v.begin(), v.end());
std::transform_reduce(std::execution::par, v1.begin(), v1.end(),
                      v2.begin(), 0, std::plus{}, std::multiplies{});
```

---

# 五、C++20 特性深度

---

## 5.1 Concepts

```cpp
#include <concepts>

// 定义 concept
template<typename T>
concept Printable = requires(T t) {
    { std::cout << t } -> std::same_as<std::ostream&>;
};

template<typename T>
concept Numeric = std::integral<T> || std::floating_point<T>;

template<typename T>
concept Container = requires(T c) {
    typename T::value_type;
    typename T::iterator;
    { c.begin() } -> std::same_as<typename T::iterator>;
    { c.end() } -> std::same_as<typename T::iterator>;
    { c.size() } -> std::convertible_to<std::size_t>;
};

// 四种使用方式
// 1. requires 子句
template<typename T> requires Numeric<T>
T add(T a, T b) { return a + b; }

// 2. 模板参数约束
template<Numeric T>
T multiply(T a, T b) { return a * b; }

// 3. 尾置 requires
template<typename T>
T divide(T a, T b) requires Numeric<T> { return a / b; }

// 4. 缩写函数模板
auto square(Numeric auto val) { return val * val; }

// 标准库 concepts
std::same_as<T, U>
std::derived_from<Derived, Base>
std::convertible_to<From, To>
std::integral<T>
std::floating_point<T>
std::invocable<F, Args...>
std::predicate<F, Args...>
std::ranges::range<R>
std::sortable<I, Comp>
```

### concepts vs SFINAE vs if constexpr

```cpp
// 同一个需求的三种实现:

// SFINAE (C++11) — 复杂难读
template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
T double_it(T val) { return val * 2; }

// if constexpr (C++17) — 简单但无重载
template<typename T>
auto double_it(T val) {
    if constexpr (std::is_integral_v<T>) return val * 2;
    else return val * 2.0;
}

// Concepts (C++20) — 最优雅
template<std::integral T>
T double_it(T val) { return val * 2; }

template<std::floating_point T>
T double_it(T val) { return val * 2.0; }

// 错误信息对比:
// SFINAE: "no matching function... candidate template ignored..."
// Concepts: "constraints not satisfied: T does not model std::integral"
```

---

## 5.2 Ranges

```cpp
#include <ranges>

std::vector<int> v = {5, 3, 1, 4, 2, 8, 6, 7};

// 链式操作（惰性求值）
auto result = v
    | std::views::filter([](int x) { return x % 2 == 0; })
    | std::views::transform([](int x) { return x * x; })
    | std::views::take(3);

for (int x : result) {
    std::cout << x << " ";  // 16 4 64
}

// views 不拷贝数据，惰性求值
// 每次迭代才执行 filter → transform → take

// 常用 views
std::views::filter(pred)        // 过滤
std::views::transform(func)     // 变换
std::views::take(n)             // 取前 n 个
std::views::drop(n)             // 跳过前 n 个
std::views::reverse              // 反转
std::views::split(delim)        // 分割
std::views::join                 // 展平
std::views::iota(start)         // 无限序列 start, start+1, ...
std::views::zip(r1, r2)         // C++23

// ranges 算法（直接传容器）
std::ranges::sort(v);
std::ranges::find(v, 42);
std::ranges::count_if(v, [](int x) { return x > 3; });
auto [min, max] = std::ranges::minmax(v);

// 投影（projection）
struct Person { std::string name; int age; };
std::vector<Person> people = {{"Alice", 30}, {"Bob", 25}};
std::ranges::sort(people, {}, &Person::age);  // 按 age 排序
```

---

## 5.3 Coroutines

```cpp
#include <coroutine>

// 协程返回类型
struct Generator {
    struct promise_type {
        int current_value;

        Generator get_return_object() {
            return Generator{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(int val) {
            current_value = val;
            return {};
        }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;

    bool next() {
        handle.resume();
        return !handle.done();
    }
    int value() { return handle.promise().current_value; }
    ~Generator() { if (handle) handle.destroy(); }
};

// 使用协程
Generator fibonacci() {
    int a = 0, b = 1;
    while (true) {
        co_yield a;          // 挂起，返回 a
        auto temp = a + b;
        a = b;
        b = temp;
    }
}

auto gen = fibonacci();
for (int i = 0; i < 10 && gen.next(); ++i) {
    std::cout << gen.value() << " ";
}
// 输出: 0 1 1 2 3 5 8 13 21 34

// 三个关键字:
// co_await expr  — 挂起，等待 awaitable 完成
// co_yield expr  — 挂起，返回一个值（生成器）
// co_return expr — 完成协程
```

---

## 5.4 其他 C++20 特性

```cpp
// Modules
export module math;
export int add(int a, int b) { return a + b; }
// import math;  ← 使用方

// 三路比较 <=>
struct Point {
    int x, y;
    auto operator<=>(const Point&) const = default;  // 自动生成 6 个比较运算符
};

// std::format
auto s = std::format("Hello, {}! You are {} years old.", name, age);
auto s2 = std::format("{:.2f}", 3.14159);  // "3.14"
auto s3 = std::format("{:>10}", "hi");     // "        hi"

// std::span
void process(std::span<int> data) { /* 接受任何连续容器 */ }
int arr[] = {1, 2, 3};
std::vector<int> vec = {4, 5, 6};
process(arr);  process(vec);  // 都 OK

// std::jthread — 自动 join 的线程
std::jthread t([](std::stop_token st) {
    while (!st.stop_requested()) {
        // 工作...
    }
});
// t 析构时自动 request_stop() + join()

// 同步原语
std::latch latch(3);         // 一次性屏障
std::barrier barrier(3);      // 可重用屏障
std::counting_semaphore<5> sem(5); // 信号量

// std::source_location (替代 __FILE__, __LINE__)
void log(std::source_location loc = std::source_location::current()) {
    std::cout << loc.file_name() << ":" << loc.line()
              << " " << loc.function_name();
}

// consteval (必须编译期)
consteval int sqr(int n) { return n * n; }

// constinit (编译期初始化，运行期可变)
constinit int global = sqr(5);  // 编译期初始化
```

---

# 六、STL 容器与算法内幕

---

## 6.1 容器选择指南

```
需要随机访问？
├── 是 → 尾部插入为主？
│       ├── 是 → vector
│       └── 否 → deque
└── 否 → 需要排序？
        ├── 是 → 需要重复 key？
        │       ├── 是 → multimap/multiset
        │       └── 否 → map/set
        └── 否 → 查找为主？
                ├── 是 → unordered_map/unordered_set
                └── 否 → 频繁中间插入？
                        ├── 是 → list
                        └── 否 → vector
```

## 6.2 常用算法复杂度

| 算法 | 平均复杂度 | 稳定性 | 说明 |
|------|----------|--------|------|
| `std::sort` | O(n log n) | 不稳定 | IntroSort (快排+堆排+插入排序) |
| `std::stable_sort` | O(n log n) | 稳定 | 归并排序 |
| `std::partial_sort` | O(n log k) | 不稳定 | 堆排序 |
| `std::nth_element` | O(n) | 不稳定 | IntroSelect |
| `std::lower_bound` | O(log n) | — | 二分查找 |
| `std::find` | O(n) | — | 线性查找 |
| `std::count` | O(n) | — | 线性计数 |
| `std::accumulate` | O(n) | — | 线性求和 |
| `std::unique` | O(n) | — | 去重（需先排序） |

## 6.3 priority_queue — 堆

```cpp
// 默认最大堆
std::priority_queue<int> maxHeap;
maxHeap.push(3); maxHeap.push(1); maxHeap.push(4);
maxHeap.top();  // 4

// 最小堆
std::priority_queue<int, std::vector<int>, std::greater<int>> minHeap;

// 自定义比较
auto cmp = [](const Task& a, const Task& b) {
    return a.priority < b.priority;  // 大的优先
};
std::priority_queue<Task, std::vector<Task>, decltype(cmp)> pq(cmp);
```

---

# 七、内存管理专题

---

## 7.1 new/delete vs malloc/free

| 特性 | new/delete | malloc/free |
|------|-----------|-------------|
| 类型 | 运算符 | 函数 |
| 构造/析构 | 调用 | 不调用 |
| 返回类型 | 正确类型指针 | void* |
| 失败处理 | 抛 bad_alloc | 返回 nullptr |
| 可重载 | 可以 | 不可以 |
| 大小 | 自动计算 | 手动指定 |

```cpp
// new 的三步:
// 1. 调用 operator new(size) 分配内存（可重载）
// 2. 在分配的内存上调用构造函数
// 3. 返回类型化指针

// delete 的两步:
// 1. 调用析构函数
// 2. 调用 operator delete(ptr) 释放内存
```

## 7.2 placement new

```cpp
#include <new>

// 在已有内存上构造对象
char buffer[sizeof(Foo)];
Foo* p = new (buffer) Foo(args...);  // 不分配内存，只调用构造函数

// 手动析构（不能用 delete）
p->~Foo();

// 用途:
// 1. 内存池: 预分配大块内存，按需构造对象
// 2. STL 容器: vector 内部用 placement new 在预分配空间上构造元素
// 3. 对齐内存: 在特定对齐的内存上构造对象
```

## 7.3 内存对齐

```cpp
// alignof: 查询对齐要求
alignof(int);     // 4
alignof(double);  // 8
alignof(char);    // 1

struct Foo {
    char a;    // 1 byte + 3 padding
    int b;     // 4 bytes
    char c;    // 1 byte + 3 padding
};
sizeof(Foo);   // 12 (不是 6！因为对齐)

// alignas: 指定对齐
struct alignas(64) CacheLine {  // 按缓存行对齐
    int data[16];
};

// C++17 aligned new
auto p = new (std::align_val_t(64)) CacheLine();
```

```
内存布局示例 (64-bit):
struct S {
    char a;     // offset 0, size 1
    // padding 7 bytes
    double b;   // offset 8, size 8 (对齐到 8)
    int c;      // offset 16, size 4
    // padding 4 bytes
};              // total: 24 bytes, alignment: 8

优化后:
struct S {
    double b;   // offset 0, size 8
    int c;      // offset 8, size 4
    char a;     // offset 12, size 1
    // padding 3 bytes
};              // total: 16 bytes (节省 8 bytes!)
```

## 7.4 RAII 与资源管理

```cpp
// RAII: Resource Acquisition Is Initialization
// 构造时获取资源，析构时释放资源

// 文件 RAII
class FileHandle {
    FILE* fp_;
public:
    FileHandle(const char* path, const char* mode)
        : fp_(fopen(path, mode))
    {
        if (!fp_) throw std::runtime_error("Cannot open file");
    }
    ~FileHandle() { if (fp_) fclose(fp_); }

    // 禁止拷贝
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    // 允许移动
    FileHandle(FileHandle&& other) noexcept : fp_(other.fp_) {
        other.fp_ = nullptr;
    }

    FILE* get() { return fp_; }
};

// 锁 RAII → lock_guard
// 内存 RAII → smart pointer
// 连接 RAII → 自定义 wrapper
```

## 7.5 常见内存问题

```cpp
// 1. 内存泄漏
void leak() {
    int* p = new int(42);
    // 忘记 delete → 泄漏
    // 修复: 用 unique_ptr
}

// 2. 野指针 (dangling pointer)
int* p = new int(42);
delete p;
*p = 10;  // UB! p 是野指针
// 修复: delete 后置 nullptr; 或用智能指针

// 3. Double free
int* p = new int(42);
delete p;
delete p;  // UB!
// 修复: 智能指针自动管理

// 4. Use after free
auto sp = std::make_shared<Foo>();
Foo* raw = sp.get();
sp.reset();   // Foo 被销毁
raw->method(); // UB!
// 修复: 不要混用裸指针和智能指针

// 5. 栈上缓冲区溢出
char buf[10];
strcpy(buf, "this is too long");  // 溢出！
// 修复: 用 std::string 或 strncpy

// 6. 未初始化内存
int x;
std::cout << x;  // UB! 值不确定
// 修复: 总是初始化变量
```

---

# 八、模板与泛型编程

---

## 8.1 模板基础

```cpp
// 函数模板
template<typename T>
T max(T a, T b) { return (a > b) ? a : b; }

// 类模板
template<typename T, int N>
class Array {
    T data_[N];
public:
    T& operator[](int i) { return data_[i]; }
    constexpr int size() const { return N; }
};

Array<int, 10> arr;
```

## 8.2 模板特化

```cpp
// 全特化
template<>
class Array<bool, 8> {
    uint8_t bits_;
public:
    // 位操作实现
};

// 偏特化（只有类模板可以）
template<typename T>
class Array<T, 0> {
    // 空数组特化
};

template<typename T>
class Array<T*, 10> {
    // 指针类型特化
};

// 函数模板不能偏特化，用重载代替
template<typename T>
void process(T val) { ... }       // 通用版本

template<typename T>
void process(T* ptr) { ... }      // 指针重载

void process(int val) { ... }     // 具体类型重载
```

## 8.3 CRTP — 奇异递归模板模式

```cpp
// CRTP: Curiously Recurring Template Pattern
// 静态多态，编译期解析，零开销

template<typename Derived>
class Base {
public:
    void interface() {
        // 编译期调用派生类方法
        static_cast<Derived*>(this)->implementation();
    }
};

class Concrete : public Base<Concrete> {
public:
    void implementation() {
        std::cout << "Concrete implementation\n";
    }
};

// 对比虚函数:
// 虚函数: 运行时多态，vtable 查找，~10ns 开销
// CRTP: 编译期多态，直接调用，零开销

// 实际应用: enable_shared_from_this 就是 CRTP
class Widget : public std::enable_shared_from_this<Widget> {};
```

## 8.4 编译期编程总结

```
C++11: constexpr (受限) + type_traits + SFINAE (enable_if)
          ↓
C++14: 放松 constexpr + 变量模板
          ↓
C++17: if constexpr + fold expressions + void_t
          ↓
C++20: concepts + requires + consteval + constinit

// 趋势: 从黑魔法 (SFINAE) 到优雅 (concepts)
```

---

# 九、面试高频对比总结

---

## 值类别

| 类别 | 有身份 | 可移动 | 示例 |
|------|-------|--------|------|
| lvalue | 是 | 否 | `x`, `*ptr`, `arr[0]` |
| xvalue | 是 | 是 | `std::move(x)`, `static_cast<T&&>(x)` |
| prvalue | 否 | 是 | `42`, `x+1`, `Foo()` |

## 智能指针

| | unique_ptr | shared_ptr | weak_ptr |
|--|-----------|-----------|----------|
| 所有权 | 独占 | 共享(引用计数) | 无 |
| 大小 | 1 指针 | 2 指针 | 2 指针 |
| 性能 | 约等于裸指针 | 原子引用计数 | 无额外开销 |
| 拷贝 | 禁止 | 允许 | 允许 |
| 循环引用 | 不可能 | 会泄漏 | 用来打破循环 |
| 线程安全 | 不安全 | 控制块安全，对象不安全 | 控制块安全 |

## 锁

| | lock_guard | unique_lock | shared_lock | scoped_lock |
|--|-----------|------------|-------------|-------------|
| 版本 | C++11 | C++11 | C++17 | C++17 |
| 手动解锁 | 否 | 是 | 是 | 否 |
| 配合 cv | 否 | 是 | 否 | 否 |
| 多锁 | 否 | 否 | 否 | 是 |
| 读写 | 互斥 | 互斥 | 共享读 | 互斥 |

## 容器时间复杂度

| 操作 | vector | deque | list | map | unordered_map |
|------|--------|-------|------|-----|---------------|
| 随机访问 | O(1) | O(1) | O(n) | O(log n) | O(1)~O(n) |
| 头部插入 | O(n) | O(1) | O(1) | — | — |
| 尾部插入 | O(1)* | O(1) | O(1) | — | — |
| 中间插入 | O(n) | O(n) | O(1) | O(log n) | O(1)~O(n) |
| 查找 | O(n) | O(n) | O(n) | O(log n) | O(1)~O(n) |

`*` 均摊

## 内存序

| 序 | 保证 | 性能 | 用途 |
|----|------|------|------|
| relaxed | 只保证原子性 | 最快 | 计数器 |
| acquire | 后续读写不重排到前面 | 快 | 消费者端 |
| release | 之前读写不重排到后面 | 快 | 生产者端 |
| acq_rel | acquire + release | 中 | RMW 操作 |
| seq_cst | 全局顺序一致 | 最慢 | 默认/简单场景 |

## C++ 各版本核心特性

| 版本 | 语言特性 | 标准库 |
|------|---------|--------|
| **C++11** | auto, lambda, move, constexpr, enum class, variadic template, override/final, nullptr | function, thread, atomic, shared_ptr, unique_ptr, unordered_map, chrono, tuple, regex |
| **C++14** | 泛型 lambda, decltype(auto), 放松 constexpr, 变量模板 | make_unique, shared_timed_mutex, integer_sequence |
| **C++17** | 结构化绑定, if constexpr, inline 变量, 折叠表达式, CTAD | optional, variant, any, string_view, filesystem, parallel algorithms, shared_mutex |
| **C++20** | concepts, requires, coroutines, modules, <=>, consteval/constinit | ranges, format, span, jthread, latch/barrier/semaphore, source_location |

## 编译期技术演进

| 时代 | 技术 | 复杂度 | 错误信息 |
|------|------|-------|---------|
| C++11 | SFINAE + enable_if | 高 | 难读 |
| C++14 | 变量模板 + 放松 constexpr | 中 | 一般 |
| C++17 | if constexpr + void_t | 中低 | 较好 |
| C++20 | concepts + requires | 低 | 清晰 |

## 面试必问"为什么"

| 问题 | 答案 |
|------|------|
| 为什么移动构造要 noexcept？ | vector 扩容只在 noexcept 时用移动，否则用拷贝（异常安全） |
| 为什么 make_shared 比 new 好？ | 一次分配、异常安全、缓存友好 |
| 为什么 weak_ptr 能打破循环引用？ | 不增加引用计数，不阻止对象析构 |
| 为什么 lambda 比 bind 好？ | 可读性好、编译器易内联、不需要 placeholder |
| 为什么 emplace_back 比 push_back 好？ | 直接原地构造，不产生临时对象 |
| 为什么用 enum class 不用 enum？ | 不污染命名空间、不隐式转 int、可指定底层类型 |
| 为什么优先用 string_view？ | 避免字符串拷贝，接受任意字符串源 |
| 为什么 variant 比 union 好？ | 类型安全、自动析构、知道当前类型 |
| 为什么用 concepts 不用 SFINAE？ | 更易读、更好的错误信息、约束可组合 |
| 为什么 jthread 比 thread 好？ | 自动 join、支持 stop_token 协作取消 |

---

> 本文档覆盖 C++11/14/17/20 核心特性、STL 内幕、内存管理、模板元编程，面向中高级 C++ 岗位面试。
> 最后更新：2026-04-09
