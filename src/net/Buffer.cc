/**
 * @file Buffer.cc
 * @brief 缓冲区实现
 *
 * 本文件实现了 Buffer 类的核心方法:
 * - readFd: 从文件描述符高效读取数据
 * - writeFd: 将数据写入文件描述符
 */

#include "Buffer.h"

#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

/**
 * @brief 从文件描述符读取数据
 * @param fd 文件描述符
 * @param saveErrno 错误码输出参数
 * @return 读取的字节数，-1 表示错误
 *
 * 使用 readv 系统调用实现高效读取。
 *
 * 设计思路:
 * Buffer 缓冲区是有大小的！但是从 fd 上读数据的时候，
 * 却不知道 TCP 数据最终的大小。如果数据量很大，需要多次读取。
 *
 * 解决方案:
 * 使用 readv + 栈上缓冲区:
 * - vec[0]: Buffer 的可写空间
 * - vec[1]: 栈上的 extrabuf (64KB)
 *
 * 好处:
 * 1. 如果数据量小，一次读取完成
 * 2. 如果数据量大，先读满 Buffer，再读到 extrabuf
 * 3. 然后将 extrabuf 中的数据追加到 Buffer
 * 4. 避免了预先分配过大的缓冲区
 *
 * @note Poller 工作在 LT (Level Trigger) 模式
 */
ssize_t Buffer::readFd(int fd, int* saveErrno)
{
    // 栈上的内存空间，64KB
    // 用于处理大数据量的读取
    char extrabuf[65536];  // No need to zero — readv fills it

    // 使用 iovec 结构体组织两个缓冲区
    struct iovec vec[2];

    // 获取 Buffer 底层缓冲区剩余的可写空间大小
    const size_t writable = writableBytes();

    // 第一个缓冲区: Buffer 的可写区域
    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len = writable;

    // 第二个缓冲区: 栈上的 extrabuf
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof extrabuf;

    // 如果 Buffer 可写空间足够大，只使用一个缓冲区
    // 否则使用两个缓冲区
    const int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;

    // 调用 readv 进行分散读
    const ssize_t n = ::readv(fd, vec, iovcnt);

    if (n < 0)
    {
        // 读取错误，保存错误码
        *saveErrno = errno;
    }
    else if (n <= static_cast<ssize_t>(writable))
    {
        // Buffer 的可写缓冲区已经够存储读出来的数据了
        // 只需要更新写指针
        writerIndex_ += n;
    }
    else
    {
        // Buffer 可写空间不够，extrabuf 里也写入了数据
        // 需要将 extrabuf 中的数据追加到 Buffer
        writerIndex_ = buffer_.size();  // 先将 writerIndex_ 移到末尾

        // 将 extrabuf 中多出的数据追加到 Buffer
        // append 会自动扩容
        append(extrabuf, n - writable);
    }

    return n;
}

/**
 * @brief 将数据写入文件描述符
 * @param fd 文件描述符
 * @param saveErrno 错误码输出参数
 * @return 写入的字节数，-1 表示错误
 *
 * 将 Buffer 中的可读数据写入文件描述符。
 */
ssize_t Buffer::writeFd(int fd, int* saveErrno)
{
    ssize_t n = ::write(fd, peek(), readableBytes());
    if (n < 0)
    {
        *saveErrno = errno;
    }
    return n;
}