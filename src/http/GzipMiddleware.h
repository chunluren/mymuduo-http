/**
 * @file GzipMiddleware.h
 * @brief Gzip 压缩中间件
 *
 * 提供 Gzip 压缩和解压功能，用于 HTTP 响应压缩。
 * 使用 zlib 库实现 gzip 格式（windowBits = 15 + 16）。
 *
 * @example
 * @code
 * // 压缩
 * std::string compressed = GzipCodec::compress(data);
 *
 * // 解压
 * std::string decompressed = GzipCodec::decompress(compressed);
 *
 * // 判断是否需要压缩
 * if (GzipCodec::shouldCompress("text/html")) { ... }
 * @endcode
 */

#pragma once

#include <string>
#include <zlib.h>
#include <cstring>

/**
 * @class GzipCodec
 * @brief Gzip 编解码器
 *
 * 提供静态方法进行 gzip 压缩和解压缩，以及内容类型判断。
 */
class GzipCodec {
public:
    /**
     * @brief Gzip 压缩
     * @param data 原始数据
     * @param level 压缩级别（Z_DEFAULT_COMPRESSION 为默认）
     * @return 压缩后的数据，失败返回空字符串
     */
    static std::string compress(const std::string& data, int level = Z_DEFAULT_COMPRESSION) {
        if (data.empty()) return "";

        z_stream strm;
        memset(&strm, 0, sizeof(strm));

        // windowBits = 15 + 16 表示 gzip 格式
        if (deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            return "";
        }

        strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        strm.avail_in = static_cast<uInt>(data.size());

        std::string result;
        char buffer[32768];  // 32KB buffer

        do {
            strm.next_out = reinterpret_cast<Bytef*>(buffer);
            strm.avail_out = sizeof(buffer);

            int ret = deflate(&strm, Z_FINISH);
            if (ret == Z_STREAM_ERROR) {
                deflateEnd(&strm);
                return "";
            }

            size_t have = sizeof(buffer) - strm.avail_out;
            result.append(buffer, have);
        } while (strm.avail_out == 0);

        deflateEnd(&strm);
        return result;
    }

    /**
     * @brief Gzip 解压缩
     * @param data 压缩数据
     * @return 解压后的数据，失败返回空字符串
     */
    static std::string decompress(const std::string& data) {
        if (data.empty()) return "";

        z_stream strm;
        memset(&strm, 0, sizeof(strm));

        // windowBits = 15 + 16 表示 gzip 格式
        if (inflateInit2(&strm, 15 + 16) != Z_OK) {
            return "";
        }

        strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        strm.avail_in = static_cast<uInt>(data.size());

        std::string result;
        char buffer[32768];  // 32KB buffer

        do {
            strm.next_out = reinterpret_cast<Bytef*>(buffer);
            strm.avail_out = sizeof(buffer);

            int ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                inflateEnd(&strm);
                return "";
            }

            size_t have = sizeof(buffer) - strm.avail_out;
            result.append(buffer, have);
        } while (strm.avail_out == 0);

        inflateEnd(&strm);
        return result;
    }

    /**
     * @brief 判断内容类型是否适合 Gzip 压缩
     * @param contentType Content-Type 头部值
     * @return 是否应该压缩
     *
     * 以下类型会被压缩:
     * - text/html, text/plain, text/css 等
     * - application/json
     * - application/javascript
     * - application/xml
     */
    static bool shouldCompress(const std::string& contentType) {
        if (contentType.empty()) return false;

        // text/* 类型都压缩
        if (contentType.find("text/") == 0) return true;

        // 特定 application 类型
        if (contentType.find("application/json") != std::string::npos) return true;
        if (contentType.find("application/javascript") != std::string::npos) return true;
        if (contentType.find("application/xml") != std::string::npos) return true;

        return false;
    }
};
