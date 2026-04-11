#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include "src/util/CircuitBreaker.h"

using namespace std;

void testNormalOperation() {
    cout << "=== Testing Normal Operation ===" << endl;
    CircuitBreaker cb(5, 2, 1);
    assert(cb.state() == CircuitBreaker::Closed);
    assert(cb.allow());
    cb.recordSuccess();
    assert(cb.state() == CircuitBreaker::Closed);
    cout << "Normal operation test passed!" << endl;
}

void testOpenOnFailure() {
    cout << "=== Testing Open on Failure ===" << endl;
    CircuitBreaker cb(3, 1, 1);
    for (int i = 0; i < 3; ++i) {
        assert(cb.allow());
        cb.recordFailure();
    }
    assert(cb.state() == CircuitBreaker::Open);
    assert(!cb.allow());
    cout << "Open on failure test passed!" << endl;
}

void testHalfOpenRecovery() {
    cout << "=== Testing Half-Open Recovery ===" << endl;
    CircuitBreaker cb(2, 2, 1);
    cb.recordFailure();
    cb.recordFailure();
    assert(cb.state() == CircuitBreaker::Open);
    this_thread::sleep_for(chrono::milliseconds(1100));
    assert(cb.state() == CircuitBreaker::HalfOpen);
    assert(cb.allow());
    cb.recordSuccess();
    cb.recordSuccess();
    assert(cb.state() == CircuitBreaker::Closed);
    cout << "Half-open recovery test passed!" << endl;
}

void testHalfOpenFallback() {
    cout << "=== Testing Half-Open Fallback ===" << endl;
    CircuitBreaker cb(2, 2, 1);
    cb.recordFailure();
    cb.recordFailure();
    assert(cb.state() == CircuitBreaker::Open);
    this_thread::sleep_for(chrono::milliseconds(1100));
    assert(cb.allow());
    cb.recordFailure();
    assert(cb.state() == CircuitBreaker::Open);
    assert(!cb.allow());
    cout << "Half-open fallback test passed!" << endl;
}

void testExecuteHelper() {
    cout << "=== Testing Execute Helper ===" << endl;
    CircuitBreaker cb(3, 1, 1);
    int callCount = 0;
    auto result = cb.execute([&]() -> bool { callCount++; return true; });
    assert(result == true);
    assert(callCount == 1);

    for (int i = 0; i < 3; ++i) {
        cb.execute([&]() -> bool { throw std::runtime_error("fail"); return true; });
    }
    assert(cb.state() == CircuitBreaker::Open);

    bool executed = false;
    cb.execute([&]() -> bool { executed = true; return true; });
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
