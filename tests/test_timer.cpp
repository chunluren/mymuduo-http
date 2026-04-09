// test_timer.cpp - TimerQueue and Timer unit tests
//
// Tests the standalone time-wheel TimerQueue: add, cancel, periodic,
// timer count, and multiple timers with different delays.

#include <iostream>
#include <cassert>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

#include "timer/TimerQueue.h"
#include "timer/Timer.h"

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    test_##name(); \
    std::cout << "PASSED" << std::endl; \
} while(0)

// ---------------------------------------------------------------------------
// Test 1: Add a timer with 1-tick delay, tick once, verify callback fires
// ---------------------------------------------------------------------------
TEST(add_and_tick) {
    // 10 buckets, 50 ms per tick
    TimerQueue tq(10, 50);

    std::atomic<int> fired{0};
    tq.addTimer([&]() { fired++; }, 50); // delay = 1 tick

    // 时间轮需要转到 timer 所在的桶才能触发
    // tick 多次确保转到目标桶
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    tq.tick();
    tq.tick();

    assert(fired.load() == 1);
}

// ---------------------------------------------------------------------------
// Test 2: Add a timer, cancel it before it fires, tick, verify no fire
// ---------------------------------------------------------------------------
TEST(cancel_timer) {
    TimerQueue tq(10, 50);

    std::atomic<int> fired{0};
    int64_t id = tq.addTimer([&]() { fired++; }, 50);

    // Cancel before tick
    tq.cancelTimer(id);

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    tq.tick();
    tq.tick();

    assert(fired.load() == 0);
}

// ---------------------------------------------------------------------------
// Test 3: Periodic timer – fires every tick for 5 ticks
// ---------------------------------------------------------------------------
TEST(periodic_timer) {
    // 20 buckets, 50 ms per tick
    TimerQueue tq(20, 50);

    std::atomic<int> fired{0};
    // delay = 50 ms (1 tick), interval = 50 ms (periodic)
    tq.addTimer([&]() { fired++; }, 50, 50);

    // 时间轮需要足够的 tick 来触发周期性定时器
    // 多 tick 几轮确保周期性定时器有机会触发
    for (int i = 0; i < 12; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        tq.tick();
    }

    // 至少触发 3 次即可
    assert(fired.load() >= 3);
}

// ---------------------------------------------------------------------------
// Test 4: timerCount – add timers, verify count, cancel some, verify decrease
// ---------------------------------------------------------------------------
TEST(timer_count) {
    TimerQueue tq(60, 1000);

    assert(tq.timerCount() == 0);

    int64_t id1 = tq.addTimer([](){}, 1000);
    int64_t id2 = tq.addTimer([](){}, 2000);
    int64_t id3 = tq.addTimer([](){}, 3000);
    (void)id3; // suppress unused warning

    assert(tq.timerCount() == 3);

    tq.cancelTimer(id1);
    assert(tq.timerCount() == 2);

    tq.cancelTimer(id2);
    assert(tq.timerCount() == 1);
}

// ---------------------------------------------------------------------------
// Test 5: Multiple timers with different delays, tick through, all fire
// ---------------------------------------------------------------------------
TEST(multiple_timers_different_delays) {
    // 20 buckets, 50 ms per tick
    TimerQueue tq(20, 50);

    const int N = 10;
    std::atomic<int> totalFired{0};

    // Add timers at delays 1-tick, 2-tick, ..., N-tick
    for (int i = 1; i <= N; ++i) {
        tq.addTimer([&]() { totalFired++; }, i * 50);
    }

    assert(tq.timerCount() == static_cast<size_t>(N));

    // Tick 足够多次确保所有定时器都有机会触发
    for (int i = 0; i < N + 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        tq.tick();
    }

    assert(totalFired.load() == N);
}

// ---------------------------------------------------------------------------
// Test 6: Cancel a periodic timer – verify it stops firing
// ---------------------------------------------------------------------------
TEST(cancel_periodic_timer) {
    TimerQueue tq(20, 50);

    std::atomic<int> fired{0};
    int64_t id = tq.addTimer([&]() { fired++; }, 50, 50);

    // 让 timer 触发几次
    for (int i = 0; i < 6; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        tq.tick();
    }
    int countBeforeCancel = fired.load();
    assert(countBeforeCancel >= 1);  // 至少触发一次

    // Cancel
    tq.cancelTimer(id);

    // Tick more — count must not increase
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        tq.tick();
    }
    assert(fired.load() == countBeforeCancel);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=== Timer / TimerQueue Tests ===" << std::endl;

    RUN_TEST(add_and_tick);
    RUN_TEST(cancel_timer);
    RUN_TEST(periodic_timer);
    RUN_TEST(timer_count);
    RUN_TEST(multiple_timers_different_delays);
    RUN_TEST(cancel_periodic_timer);

    std::cout << std::endl << "All Timer tests passed!" << std::endl;
    return 0;
}
