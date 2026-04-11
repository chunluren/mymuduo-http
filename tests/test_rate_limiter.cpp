#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include "src/util/RateLimiter.h"

using namespace std;

void testTokenBucket() {
    cout << "=== Testing Token Bucket ===" << endl;
    TokenBucketLimiter limiter(10, 10);
    int allowed = 0;
    for (int i = 0; i < 15; ++i) {
        if (limiter.allow("client1")) allowed++;
    }
    assert(allowed == 10);
    this_thread::sleep_for(chrono::seconds(1));
    assert(limiter.allow("client1"));
    cout << "Token bucket test passed!" << endl;
}

void testTokenBucketMultiClient() {
    cout << "=== Testing Token Bucket Multi-Client ===" << endl;
    TokenBucketLimiter limiter(5, 5);
    for (int i = 0; i < 5; ++i) {
        assert(limiter.allow("client_a"));
        assert(limiter.allow("client_b"));
    }
    assert(!limiter.allow("client_a"));
    assert(!limiter.allow("client_b"));
    cout << "Multi-client test passed!" << endl;
}

void testSlidingWindow() {
    cout << "=== Testing Sliding Window ===" << endl;
    SlidingWindowLimiter limiter(5, 1);
    for (int i = 0; i < 5; ++i) {
        assert(limiter.allow("client1"));
    }
    assert(!limiter.allow("client1"));
    this_thread::sleep_for(chrono::milliseconds(1100));
    assert(limiter.allow("client1"));
    cout << "Sliding window test passed!" << endl;
}

void testGlobalLimiter() {
    cout << "=== Testing Global Limiter ===" << endl;
    TokenBucketLimiter limiter(3, 3);
    assert(limiter.allow(""));
    assert(limiter.allow(""));
    assert(limiter.allow(""));
    assert(!limiter.allow(""));
    cout << "Global limiter test passed!" << endl;
}

int main() {
    cout << "Starting RateLimiter Tests..." << endl << endl;
    testTokenBucket();
    testTokenBucketMultiClient();
    testSlidingWindow();
    testGlobalLimiter();
    cout << endl << "All RateLimiter tests passed!" << endl;
    return 0;
}
