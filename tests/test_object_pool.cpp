#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include "src/util/ObjectPool.h"

using namespace std;

struct TestObject {
    int value = 0;
};

void testBasicAcquireRelease() {
    cout << "=== Testing Basic Acquire/Release ===" << endl;
    ObjectPool<TestObject> pool(5);
    assert(pool.available() == 5);

    auto obj = pool.acquire();
    assert(obj != nullptr);
    assert(pool.available() == 4);

    obj->value = 42;
    pool.release(std::move(obj));
    assert(pool.available() == 5);

    auto obj2 = pool.acquire();
    assert(obj2->value == 42);
    pool.release(std::move(obj2));

    cout << "Basic acquire/release test passed!" << endl;
}

void testPoolExhaustion() {
    cout << "=== Testing Pool Exhaustion ===" << endl;
    ObjectPool<TestObject> pool(3, 5);

    vector<ObjectPool<TestObject>::Ptr> objects;
    for (int i = 0; i < 5; ++i) {
        auto obj = pool.acquire();
        assert(obj != nullptr);
        objects.push_back(std::move(obj));
    }
    assert(pool.available() == 0);

    auto overflow = pool.acquire();
    assert(overflow == nullptr);

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
    pool.setResetFunc([](TestObject& obj) { obj.value = 0; });

    auto obj = pool.acquire();
    obj->value = 99;
    pool.release(std::move(obj));

    auto obj2 = pool.acquire();
    assert(obj2->value == 0);
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
