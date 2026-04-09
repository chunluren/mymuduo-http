// test_buffer.cpp - Buffer class unit tests
#include <iostream>
#include <cassert>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "net/Buffer.h"

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    test_##name(); \
    std::cout << "PASSED" << std::endl; \
} while(0)

// Test 1: Initial state of a fresh Buffer
TEST(initial_state)
{
    Buffer buf;

    assert(buf.readableBytes() == 0);
    assert(buf.writableBytes() == Buffer::kInitialSize);
    assert(buf.prependableBytes() == Buffer::kCheapPrepend);

    // Verify constants
    assert(Buffer::kCheapPrepend == 8);
    assert(Buffer::kInitialSize == 1024);
}

// Test 2: append() and basic read accessors
TEST(append_and_readable)
{
    Buffer buf;

    const char* msg = "hello world";
    size_t len = strlen(msg);
    buf.append(msg, len);

    assert(buf.readableBytes() == len);
    assert(buf.writableBytes() == Buffer::kInitialSize - len);
    assert(buf.prependableBytes() == Buffer::kCheapPrepend);

    // peek should return the data without consuming it
    assert(std::string(buf.peek(), buf.readableBytes()) == "hello world");
    // readableBytes should remain unchanged after peek
    assert(buf.readableBytes() == len);
}

// Test 3: retrieve() partial and retrieveAll()
TEST(retrieve)
{
    Buffer buf;
    buf.append("abcdefghij", 10);

    // Partial retrieve
    buf.retrieve(4);  // consume "abcd"
    assert(buf.readableBytes() == 6);
    assert(std::string(buf.peek(), buf.readableBytes()) == "efghij");

    // retrieveAll resets the buffer
    buf.retrieveAll();
    assert(buf.readableBytes() == 0);
    assert(buf.writableBytes() == Buffer::kInitialSize);
    assert(buf.prependableBytes() == Buffer::kCheapPrepend);
}

// Test 4: retrieveAllAsString() and retrieveAsString()
TEST(retrieve_as_string)
{
    Buffer buf;

    buf.append("hello", 5);
    buf.append(" ", 1);
    buf.append("world", 5);

    std::string result = buf.retrieveAllAsString();
    assert(result == "hello world");
    assert(buf.readableBytes() == 0);

    // Test retrieveAsString with partial length
    buf.append("foobar", 6);
    std::string partial = buf.retrieveAsString(3);
    assert(partial == "foo");
    assert(buf.readableBytes() == 3);

    std::string rest = buf.retrieveAllAsString();
    assert(rest == "bar");
}

// Test 5: ensureWritableBytes() triggers expansion
TEST(ensure_writable_expansion)
{
    Buffer buf;

    // Fill the buffer completely
    std::string largeData(Buffer::kInitialSize, 'X');
    buf.append(largeData.data(), largeData.size());
    assert(buf.readableBytes() == Buffer::kInitialSize);
    assert(buf.writableBytes() == 0);

    // Request more space - should trigger resize
    buf.ensureWritableBytes(512);
    assert(buf.writableBytes() >= 512);

    // Data should still be intact
    assert(buf.readableBytes() == Buffer::kInitialSize);
    assert(std::string(buf.peek(), Buffer::kInitialSize) == largeData);
}

// Test 6: makeSpace() internal compaction
TEST(make_space_compaction)
{
    Buffer buf;

    // Write some data
    std::string data(800, 'A');
    buf.append(data.data(), data.size());
    assert(buf.readableBytes() == 800);

    // Read most of it, creating free space at the front
    buf.retrieve(700);
    assert(buf.readableBytes() == 100);
    assert(buf.prependableBytes() == Buffer::kCheapPrepend + 700);

    // The writable space is kInitialSize - 800 = 224
    size_t writableBefore = buf.writableBytes();
    assert(writableBefore == Buffer::kInitialSize - 800);

    // Now write more data that fits in total free space but not in writable alone.
    // Total free = prependable - kCheapPrepend + writable = 700 + 224 = 924
    // We need something > 224 but <= 924 to trigger compaction (not resize).
    std::string moreData(500, 'B');
    buf.append(moreData.data(), moreData.size());

    // After compaction, readable data should be 100 A's + 500 B's
    assert(buf.readableBytes() == 600);
    assert(buf.prependableBytes() == Buffer::kCheapPrepend);

    std::string result = buf.retrieveAllAsString();
    assert(result == std::string(100, 'A') + std::string(500, 'B'));
}

// Test 7: peek() does not consume data
TEST(peek_no_consume)
{
    Buffer buf;
    buf.append("peek_test", 9);

    const char* p1 = buf.peek();
    size_t r1 = buf.readableBytes();

    // Peek again - same pointer, same readable count
    const char* p2 = buf.peek();
    size_t r2 = buf.readableBytes();

    assert(p1 == p2);
    assert(r1 == r2);
    assert(r1 == 9);
    assert(std::string(p1, 9) == "peek_test");
}

// Test 8: readFd() and writeFd() using socketpair
TEST(read_write_fd)
{
    int sv[2];
    int ret = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(ret == 0);

    // Write data to one end
    const char* msg = "hello from socketpair";
    size_t msgLen = strlen(msg);
    ssize_t written = ::write(sv[0], msg, msgLen);
    assert(written == static_cast<ssize_t>(msgLen));

    // Read it into a Buffer using readFd
    Buffer readBuf;
    int savedErrno = 0;
    ssize_t nread = readBuf.readFd(sv[1], &savedErrno);
    assert(nread == static_cast<ssize_t>(msgLen));
    assert(readBuf.readableBytes() == msgLen);
    assert(readBuf.retrieveAllAsString() == "hello from socketpair");

    // Now use writeFd: put data into a Buffer, then write to fd
    Buffer writeBuf;
    const char* reply = "reply via writeFd";
    size_t replyLen = strlen(reply);
    writeBuf.append(reply, replyLen);

    savedErrno = 0;
    ssize_t nwritten = writeBuf.writeFd(sv[1], &savedErrno);
    assert(nwritten == static_cast<ssize_t>(replyLen));

    // Read it back from the other end
    char recvBuf[256] = {};
    ssize_t recvLen = ::read(sv[0], recvBuf, sizeof(recvBuf));
    assert(recvLen == static_cast<ssize_t>(replyLen));
    assert(std::string(recvBuf, recvLen) == "reply via writeFd");

    ::close(sv[0]);
    ::close(sv[1]);
}

int main()
{
    std::cout << "=== Buffer Unit Tests ===" << std::endl;

    RUN_TEST(initial_state);
    RUN_TEST(append_and_readable);
    RUN_TEST(retrieve);
    RUN_TEST(retrieve_as_string);
    RUN_TEST(ensure_writable_expansion);
    RUN_TEST(make_space_compaction);
    RUN_TEST(peek_no_consume);
    RUN_TEST(read_write_fd);

    std::cout << std::endl;
    std::cout << "=== All 8 Buffer tests PASSED ===" << std::endl;
    return 0;
}
