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
     * @param level 压缩级别（Z_DEFAULT_COMPRESSION 为默认，通常等价于 6）
     * @return 压缩后的数据，失败返回空字符串
     *
     * 使用 zlib 的 deflateInit2 进行 gzip 格式压缩:
     * - windowBits = 15 + 16: 其中 15 表示 32KB 滑动窗口（zlib 最大值），
     *   +16 告诉 zlib 生成 gzip 格式（带 gzip 文件头和 CRC32 校验尾）
     *   而非 raw deflate 或 zlib 格式
     * - memLevel = 8: 内部压缩状态使用的内存量（1-9，8 为默认平衡值）
     * - strategy = Z_DEFAULT_STRATEGY: 通用压缩策略
     *
     * 压缩循环: 使用 32KB 输出缓冲区分块读取压缩结果，
     * 当 avail_out == 0 时表示缓冲区已满，需要继续读取剩余数据。
     */
    static std::string compress(const std::string& data, int level = Z_DEFAULT_COMPRESSION) {
        if (data.empty()) return "";

        z_stream strm;
        memset(&strm, 0, sizeof(strm));

        /// windowBits = 15（最大窗口）+ 16（启用 gzip 格式头尾）
        if (deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            return "";
        }

        strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        strm.avail_in = static_cast<uInt>(data.size());

        std::string result;
        char buffer[32768];  ///< 32KB 输出缓冲区，分块收集压缩数据

        do {
            strm.next_out = reinterpret_cast<Bytef*>(buffer);
            strm.avail_out = sizeof(buffer);

            /// Z_FINISH 表示输入数据已全部提供，要求 deflate 完成所有压缩
            int ret = deflate(&strm, Z_FINISH);
            if (ret == Z_STREAM_ERROR) {
                deflateEnd(&strm);
                return "";
            }

            /// have = 本次实际产生的压缩字节数
            size_t have = sizeof(buffer) - strm.avail_out;
            result.append(buffer, have);
        } while (strm.avail_out == 0);  ///< 输出缓冲区满说明可能还有数据待读取

        deflateEnd(&strm);
        return result;
    }

    /**
     * @brief Gzip 解压缩
     * @param data 压缩数据（gzip 格式）
     * @return 解压后的原始数据，失败返回空字符串
     *
     * 使用 zlib 的 inflateInit2 进行 gzip 格式解压:
     * - windowBits = 15 + 16: 与压缩对应，+16 告诉 zlib 自动识别并解析
     *   gzip 文件头（magic number、CRC32 校验等）
     *
     * 解压循环说明:
     * - 由于原始数据大小未知，使用 32KB 缓冲区分块解压
     * - 每次 inflate 后检查 avail_out: 若为 0 表示缓冲区已满，
     *   可能还有更多数据需要解压，继续循环
     * - 遇到 Z_STREAM_ERROR（状态错误）、Z_DATA_ERROR（数据损坏）、
     *   Z_MEM_ERROR（内存不足）时立即终止并返回空字符串
     */
    static std::string decompress(const std::string& data) {
        if (data.empty()) return "";

        z_stream strm;
        memset(&strm, 0, sizeof(strm));

        /// windowBits = 15 + 16: 启用 gzip 格式自动检测与解析
        if (inflateInit2(&strm, 15 + 16) != Z_OK) {
            return "";
        }

        strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        strm.avail_in = static_cast<uInt>(data.size());

        std::string result;
        char buffer[32768];  ///< 32KB 输出缓冲区，分块收集解压数据

        do {
            strm.next_out = reinterpret_cast<Bytef*>(buffer);
            strm.avail_out = sizeof(buffer);

            /// Z_NO_FLUSH: 让 zlib 自行决定刷新时机，逐步解压
            int ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                inflateEnd(&strm);
                return "";  ///< 解压失败（数据损坏或内存不足）
            }

            /// have = 本次实际解压出的字节数
            size_t have = sizeof(buffer) - strm.avail_out;
            result.append(buffer, have);
        } while (strm.avail_out == 0);  ///< 缓冲区满说明可能还有数据需解压

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
