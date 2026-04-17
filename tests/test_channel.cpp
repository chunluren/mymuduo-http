// test_channel.cpp - Channel class unit tests
//
// Channel depends on EventLoop + a real fd. We use eventfd(2) to create a
// lightweight fd, register it with a Channel, and exercise enable/disable
// + read-callback invocation via the loop.

#include <cassert>
#include <iostream>
#include <atomic>
#include <sys/eventfd.h>
#include <unistd.h>

#include "net/EventLoop.h"
#include "net/Channel.h"

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    test_##name(); \
    std::cout << "PASSED" << std::endl; \
} while(0)

// Test 1: enableReading()/disableAll() flip event state
TEST(enable_disable)
{
    EventLoop loop;
    int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(efd > 0);

    Channel channel(&loop, efd);

    // Freshly constructed: no events registered
    assert(channel.isNoneEvent());
    assert(!channel.isReading());
    assert(!channel.isWriting());

    // Enable read
    channel.enableReading();
    assert(!channel.isNoneEvent());
    assert(channel.isReading());
    assert(!channel.isWriting());

    // Disable all
    channel.disableAll();
    assert(channel.isNoneEvent());
    assert(!channel.isReading());
    assert(!channel.isWriting());

    channel.remove();
    ::close(efd);
}

// Test 2: Channel getter accessors (fd, events, ownerLoop, index)
TEST(accessors)
{
    EventLoop loop;
    int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(efd > 0);

    Channel channel(&loop, efd);

    assert(channel.fd() == efd);
    assert(channel.ownerLoop() == &loop);
    // events() mirrors the registered mask; initially 0 (kNoneEvent)
    assert(channel.events() == 0);

    // set_index / index round-trip
    channel.set_index(42);
    assert(channel.index() == 42);

    ::close(efd);
}

// Test 3: Read callback fires when fd becomes readable
// NOTE: EventLoop's timer wheel has ~1s tick granularity, so we avoid
// scheduling the write via runAfter(0.1,...). Instead we write to the
// eventfd before entering the loop - epoll will surface EPOLLIN on the
// first poll and the channel dispatches our readCallback.
TEST(read_callback_invocation)
{
    EventLoop loop;
    int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(efd > 0);

    std::atomic<bool> triggered{false};
    Channel channel(&loop, efd);
    channel.setReadCallback([&triggered, efd](Timestamp) {
        triggered.store(true);
        // Drain the eventfd so the channel doesn't fire repeatedly
        uint64_t val = 0;
        ssize_t n = ::read(efd, &val, sizeof(val));
        (void)n;
    });
    channel.enableReading();

    // Pre-arm: eventfd already has a value when the loop starts polling.
    uint64_t v = 1;
    ssize_t n = ::write(efd, &v, sizeof(v));
    assert(n == static_cast<ssize_t>(sizeof(v)));

    // Tear the loop down after giving the callback time to run. Use 1.5s
    // so we safely clear the ~1s timer wheel tick.
    loop.runAfter(1.5, [&loop]() { loop.quit(); });

    loop.loop();

    assert(triggered.load());

    channel.disableAll();
    channel.remove();
    ::close(efd);
}

// Test 4: enableWriting()/disableWriting() flip write-interest
TEST(enable_disable_writing)
{
    EventLoop loop;
    int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(efd > 0);

    Channel channel(&loop, efd);

    assert(!channel.isWriting());
    channel.enableWriting();
    assert(channel.isWriting());

    channel.disableWriting();
    assert(!channel.isWriting());
    assert(channel.isNoneEvent());

    channel.remove();
    ::close(efd);
}

int main()
{
    std::cout << "=== Channel Unit Tests ===" << std::endl;

    RUN_TEST(enable_disable);
    RUN_TEST(accessors);
    RUN_TEST(read_callback_invocation);
    RUN_TEST(enable_disable_writing);

    std::cout << std::endl;
    std::cout << "All Channel tests passed!" << std::endl;
    return 0;
}
