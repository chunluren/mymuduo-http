/**
 * @file Buffer.h
 * @brief 自动增长的输入输出缓冲区
 *
 * 本文件定义了 Buffer 类，实现了非阻塞 I/O 所需的缓冲区功能。
 * Buffer 设计借鉴了 Netty 的 ByteBuffer 和 muduo 的缓冲区设计。
 *
 * 缓冲区结构:
 * @code
 * +-------------------+------------------+------------------+
 * | prependable bytes |  readable bytes  | writable bytes   |
 * |    (已读区域)      |    (可读区域)     |    (可写区域)    |
 * +-------------------+------------------+------------------+
 * |                   |                  |                  |
 * 0      <=      readerIndex   <=   writerIndex    <=     size
 * @endcode
 *
 * 关键特性:
 * - 自动扩容: 写入时自动扩展空间
 * - 高效读取: 支持从文件描述符直接读取
 * - 预留头部: 支持在数据前添加头部信息
 *
 * @example 基本使用
 * @code
 * Buffer buf;
 * buf.append("hello", 5);
 * std::string msg = buf.retrieveAllAsString();  // msg = "hello"
 * @endcode
 */

#pragma once

#include <vector>
#include <cstddef>
#include <string>

/**
 * @class Buffer
 * @brief 自动增长的输入输出缓冲区
 *
 * Buffer 内部使用 std::vector<char> 存储数据，支持:
 * - 自动扩容
 * - 高效的读写操作
 * - 预留头部空间
 *
 * 线程安全: Buffer 本身不是线程安全的，需要外部同步。
 * 在 muduo 中，每个 TcpConnection 都有独立的 inputBuffer_ 和 outputBuffer_，
 * 它们只在对应的 I/O 线程中访问，因此不需要锁。
 */
class Buffer
{
private:
    /**
     * @brief 获取缓冲区起始地址
     * @return 缓冲区起始地址 (可读写)
     */
    char* begin()
    {
        return &*buffer_.begin();
    }

    /**
     * @brief 获取缓冲区起始地址 (const 版本)
     * @return 缓冲区起始地址 (只读)
     */
    const char* begin() const
    {
        return &*buffer_.begin();
    }

    /**
     * @brief 确保有足够的可写空间
     * @param len 需要的空间大小
     *
     * 空间分配策略:
     * 1. 如果可写空间 + 已读空间 >= 需要的空间:
     *    - 将可读数据移动到缓冲区前端
     *    - 复用已读空间
     * 2. 否则:
     *    - 扩展缓冲区大小
     */
    void makeSpace(size_t len)
    {
        // 如果可写空间 + 已读空间不足，需要扩容
        if (writableBytes() + prependableBytes() < len + kCheapPrepend)
        {
            buffer_.resize(writerIndex_ + len);
        }
        else
        {
            // 可写空间不足，但总空间够用
            // 将可读数据移动到前端，腾出空间
            size_t readable = readableBytes();
            std::copy(begin() + readerIndex_,
                      begin() + writerIndex_,
                      begin() + kCheapPrepend);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        }
    }

    /// 底层存储
    std::vector<char> buffer_;
    /// 读指针位置 (指向可读数据起始位置)
    size_t readerIndex_;
    /// 写指针位置 (指向可写数据起始位置)
    size_t writerIndex_;

public:
    /// 预留头部大小 (8 字节)，用于添加协议头
    static const size_t kCheapPrepend = 8;
    /// 初始缓冲区大小 (1024 字节)
    static const size_t kInitialSize = 1024;

    /**
     * @brief 构造函数
     *
     * @param initialSize 可写区初始尺寸（不含 prepend）；默认 kInitialSize=1024。
     *
     * 调用方按场景预设：
     *   - HTTP 接入连接的 input/output buffer 建议 4KB（典型 header + JSON 一次到位，
     *     避免首次响应触发 vector::resize 拷贝）
     *   - 内部协议短消息可保留默认 1KB
     */
    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(initialSize + kCheapPrepend)
        , readerIndex_(kCheapPrepend)
        , writerIndex_(kCheapPrepend)
    {}

    /**
     * @brief 获取可读字节数
     * @return 可读字节数
     *
     * 可读字节 = writerIndex_ - readerIndex_
     */
    size_t readableBytes() const
    {
        return writerIndex_ - readerIndex_;
    }

    /**
     * @brief 获取可写字节数
     * @return 可写字节数
     *
     * 可写字节 = buffer_.size() - writerIndex_
     */
    size_t writableBytes() const
    {
        return buffer_.size() - writerIndex_;
    }

    /**
     * @brief 获取预留头部字节数
     * @return 预留头部字节数
     *
     * 预留头部 = readerIndex_
     * 这是已经被读取并可以复用的空间
     */
    size_t prependableBytes() const
    {
        return readerIndex_;
    }

    /**
     * @brief 获取可读数据的起始地址
     * @return 可读数据的起始地址
     *
     * 返回 readerIndex_ 位置的地址
     */
    const char* peek()
    {
        return begin() + readerIndex_;
    }

    /**
     * @brief 读取指定长度的数据
     * @param len 要读取的长度
     *
     * 更新 readerIndex_:
     * - 如果 len < 可读字节数，只移动 readerIndex_
     * - 否则，重置整个缓冲区
     */
    void retrieve(size_t len)
    {
        if (len < readableBytes())
        {
            // 只读取部分数据，移动读指针
            readerIndex_ += len;
        }
        else
        {
            // 读取全部数据，重置缓冲区
            retrieveAll();
        }
    }

    /**
     * @brief 重置缓冲区
     *
     * 将 readerIndex_ 和 writerIndex_ 都设为 kCheapPrepend
     * 表示缓冲区为空
     */
    void retrieveAll()
    {
        readerIndex_ = writerIndex_ = kCheapPrepend;
    }

    /**
     * @brief 返回缓冲区中所有可读数据的字符串
     * @return 可读数据的字符串
     *
     * 等价于 retrieveAsString(readableBytes())
     */
    std::string retrieveAllAsString()
    {
        return retrieveAsString(readableBytes());
    }

    /**
     * @brief 获取缓冲区中指定长度的可读数据字符串
     * @param len 要读取的长度
     * @return 可读数据的字符串
     *
     * 读取数据后自动移动读指针
     */
    std::string retrieveAsString(size_t len)
    {
        std::string str(peek(), len);
        retrieve(len);  // 已经读取了可读数据，对可读缓冲区进行复位操作
        return str;
    }

    /**
     * @brief 确保有足够的可写空间
     * @param len 需要的空间大小
     *
     * 如果可写空间不足，调用 makeSpace() 扩容
     */
    void ensureWritableBytes(size_t len)
    {
        if (writableBytes() < len)
        {
            makeSpace(len);  // 扩容
        }
    }

    /**
     * @brief 获取可写区域的起始地址
     * @return 可写区域的起始地址
     */
    char* beginWrite()
    {
        return begin() + writerIndex_;
    }

    /**
     * @brief 获取可写区域的起始地址 (const 版本)
     * @return 可写区域的起始地址
     */
    const char* beginWrite() const
    {
        return begin() + writerIndex_;
    }

    /**
     * @brief 追加数据到缓冲区
     * @param data 数据指针
     * @param len 数据长度
     *
     * 步骤:
     * 1. 确保有足够的可写空间
     * 2. 拷贝数据到缓冲区
     * 3. 更新 writerIndex_
     */
    void append(const char* data, size_t len)
    {
        ensureWritableBytes(len);
        std::copy(data, data + len, beginWrite());
        writerIndex_ += len;
    }

    /**
     * @brief 从文件描述符读取数据
     * @param fd 文件描述符
     * @param saveErrno 错误码输出参数
     * @return 读取的字节数，-1 表示错误
     *
     * 使用 readv 系统调用实现高效读取:
     * - 第一个 iovec 指向缓冲区的可写区域
     * - 第二个 iovec 指向栈上的 extrabuf (64KB)
     *
     * 这样可以:
     * - 避免频繁扩容
     * - 处理大数据量的读取
     *
     * @note Poller 工作在 LT 模式
     */
    ssize_t readFd(int fd, int* saveErrno);

    /**
     * @brief 将数据写入文件描述符
     * @param fd 文件描述符
     * @param saveErrno 错误码输出参数
     * @return 写入的字节数，-1 表示错误
     */
    ssize_t writeFd(int fd, int* saveErrno);
};