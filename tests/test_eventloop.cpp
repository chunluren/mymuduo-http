// test_eventloop.cpp - EventLoop unit tests
#include <iostream>
#include <cassert>
#include <thread>
#include <future>
#include <chrono>
#include <atomic>
#include <vector>

#include "net/EventLoop.h"

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    test_##name(); \
    std::cout << "PASSED" << std::endl; \
} while(0)

// Helper: run an EventLoop in a background thread, return when loop is running
struct LoopThread {
    EventLoop* loop = nullptr;
    std::thread thread;

    void start()
    {
        std::promise<EventLoop*> promise;
        auto future = promise.get_future();

        thread = std::thread([&promise]() {
            EventLoop localLoop;
            promise.set_value(&localLoop);
            localLoop.loop();
        });

        loop = future.get();
        // Give the loop a moment to enter poll()
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    void stop()
    {
        if (loop) {
            loop->quit();
        }
        if (thread.joinable()) {
            thread.join();
        }
        loop = nullptr;
    }
};

// Test 1: EventLoop creation - one loop per thread
TEST(creation)
{
    // Create an EventLoop in a separate thread and verify it exists
    std::promise<bool> promise;
    auto future = promise.get_future();

    std::thread t([&promise]() {
        EventLoop loop;
        // isInLoopThread should be true when called from the creating thread
        promise.set_value(loop.isInLoopThread());
    });

    bool inLoopThread = future.get();
    assert(inLoopThread == true);
    t.join();
}

// Test 2: runInLoop() - direct execution when called from loop thread
TEST(run_in_loop_same_thread)
{
    std::promise<bool> promise;
    auto future = promise.get_future();

    std::thread t([&promise]() {
        EventLoop loop;
        bool executed = false;

        // runInLoop from the same thread should execute the callback directly
        loop.runInLoop([&executed]() {
            executed = true;
        });

        promise.set_value(executed);
        // Don't call loop.loop() - we just test same-thread direct execution
    });

    bool result = future.get();
    assert(result == true);
    t.join();
}

// Test 3: queueInLoop() - cross-thread task delivery
TEST(queue_in_loop_cross_thread)
{
    LoopThread lt;
    lt.start();

    std::promise<int> promise;
    auto future = promise.get_future();

    lt.loop->queueInLoop([&promise]() {
        promise.set_value(42);
    });

    auto status = future.wait_for(std::chrono::milliseconds(500));
    assert(status == std::future_status::ready);
    assert(future.get() == 42);

    lt.stop();
}

// Test 4: runAfter() - timer fires approximately on time
TEST(run_after)
{
    LoopThread lt;
    lt.start();

    std::promise<void> promise;
    auto future = promise.get_future();

    auto start = std::chrono::steady_clock::now();

    // The timer tick is 1000ms in this implementation, so use a delay
    // that aligns well. Use 1.0s and allow generous margin.
    lt.loop->runInLoop([&lt, &promise]() {
        lt.loop->runAfter(1.0, [&promise]() {
            promise.set_value();
        });
    });

    auto status = future.wait_for(std::chrono::milliseconds(3000));
    assert(status == std::future_status::ready);

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    // The timer wheel tick is 1000ms, so the actual delay could be up to ~2 ticks.
    // We just verify it fired and took at least some time.
    assert(elapsedMs >= 500);   // Should take at least ~1 tick
    assert(elapsedMs < 3000);   // Should not take forever

    lt.stop();
}

// Test 5: runEvery() - periodic timer fires multiple times
TEST(run_every)
{
    LoopThread lt;
    lt.start();

    std::atomic<int> count{0};
    std::promise<void> promise;
    auto future = promise.get_future();

    lt.loop->runInLoop([&lt, &count, &promise]() {
        lt.loop->runEvery(1.0, [&count, &promise]() {
            int c = count.fetch_add(1) + 1;
            if (c >= 3) {
                // Signal after 3 ticks
                try {
                    promise.set_value();
                } catch (...) {
                    // Already set
                }
            }
        });
    });

    // Wait up to 5 seconds for 3 ticks
    auto status = future.wait_for(std::chrono::milliseconds(5000));
    assert(status == std::future_status::ready);
    assert(count.load() >= 3);

    lt.stop();
}

// Test 6: cancel() - timer cancellation
TEST(cancel_timer)
{
    LoopThread lt;
    lt.start();

    std::atomic<int> count{0};
    std::promise<TimerId> timerIdPromise;
    auto timerIdFuture = timerIdPromise.get_future();

    // Schedule a periodic timer, then cancel it
    lt.loop->runInLoop([&lt, &count, &timerIdPromise]() {
        TimerId tid = lt.loop->runEvery(1.0, [&count]() {
            count.fetch_add(1);
        });
        timerIdPromise.set_value(tid);
    });

    TimerId tid = timerIdFuture.get();

    // Wait for at most 1 tick, then cancel
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    int countBefore = count.load();

    lt.loop->runInLoop([&lt, tid]() {
        lt.loop->cancel(tid);
    });

    // Wait a bit more - count should not increase after cancellation
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    int countAfter = count.load();

    // Allow at most 1 extra tick due to race between cancel and timer fire
    assert(countAfter <= countBefore + 1);

    lt.stop();
}

// Test 7: quit() - loop exits cleanly from another thread
TEST(quit)
{
    std::promise<EventLoop*> ptrPromise;
    auto ptrFuture = ptrPromise.get_future();
    std::atomic<bool> loopEnded{false};

    std::thread t([&ptrPromise, &loopEnded]() {
        EventLoop loop;
        ptrPromise.set_value(&loop);
        loop.loop();  // Blocks until quit
        loopEnded.store(true);
    });

    EventLoop* loopPtr = ptrFuture.get();
    // Give the loop time to enter poll()
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(loopEnded.load() == false);

    // Quit from this (different) thread - should wake the loop and exit
    loopPtr->quit();
    t.join();
    assert(loopEnded.load() == true);
}

int main()
{
    std::cout << "=== EventLoop Unit Tests ===" << std::endl;

    RUN_TEST(creation);
    RUN_TEST(run_in_loop_same_thread);
    RUN_TEST(queue_in_loop_cross_thread);
    RUN_TEST(run_after);
    RUN_TEST(run_every);
    RUN_TEST(cancel_timer);
    RUN_TEST(quit);

    std::cout << std::endl;
    std::cout << "=== All 7 EventLoop tests PASSED ===" << std::endl;
    return 0;
}
