#pragma once

#include<vector>
#include <cstddef>
#include<string>


class Buffer
{
private:
    char* begin()
    {
        return &*buffer_.begin();
    }

    const char* begin() const
    {
        return &*buffer_.begin();
    }

    void makeSpace(size_t len)
    {
        if(writableBytes() + prependableBytes() < len + kCheapPrepend)
        {
            buffer_.resize(writerIndex_ + len);
        }
        else
        {
            size_t readable = readableBytes();
            std::copy(begin() + readerIndex_,
                      begin() + writerIndex_,
                      begin() + kCheapPrepend);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        }
    }


    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
public:
    static const size_t kCheapPrepend = 8;
    static const size_t kInitialSize = 1024;
    
    explicit Buffer()
    : buffer_(kInitialSize + kCheapPrepend),
      readerIndex_(kCheapPrepend),
      writerIndex_(kCheapPrepend)
    {}

    size_t readableBytes() const
    {
        return writerIndex_ - readerIndex_;
    }

    size_t writableBytes() const
    {
        return buffer_.size() - writerIndex_;
    }

    size_t prependableBytes() const
    {
        return readerIndex_;
    }

    // 缓冲区中可读数据的起始地址
    const char* peek()
    {
        return begin() + readerIndex_;
    }

    void retrieve(size_t len)
    {
        if(len < readableBytes())
        {
            readerIndex_ += len;
        }
        else
        {
            retrieveAll();
        }
    }

    void retrieveAll()
    {
        readerIndex_ = writerIndex_ = kCheapPrepend;
    }
    
    // 返回缓冲区中可读数据的字符串
    std::string retrieveAllAsString()
    {
        return retrieveAsString(readableBytes());
    }

    // 获取缓冲区中可读数据的字符串
    std::string retrieveAsString(size_t len)
    {
        std::string str(peek(), len);
        retrieve(len);  //已经读取了可读数据，对可读缓冲区进行复位操作
        return str;
    }

    void ensureWritableBytes(size_t len)
    {
        if(writableBytes() < len)
        {
            makeSpace(len); //扩容
        }
    }

    char* beginWrite()
    {
        return begin() + writerIndex_;
    }
    const char* beginWrite() const
    {
        return begin() + writerIndex_;

    }
    
    void append(const char* data,  size_t len)
    {
        ensureWritableBytes(len);
        std::copy(data, data + len, beginWrite());
        writerIndex_ += len;
    }

    ssize_t readFd(int fd, int* saveErrno);
    ssize_t writeFd(int fd, int* saveErrno);
};

