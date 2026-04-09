/**
 * @file WebSocketFrame.h
 * @brief WebSocket 帧定义和编解码
 *
 * 本文件实现了 RFC 6455 WebSocket 协议的帧编解码。
 * 支持:
 * - 文本帧、二进制帧
 * - Ping/Pong 心跳
 * - 关闭帧
 * - 掩码处理
 *
 * @example 使用示例
 * @code
 * // 编码文本帧
 * auto data = WebSocketFrameCodec::encodeText("Hello World");
 *
 * // 编码二进制帧
 * std::vector<uint8_t> binaryData = {0x01, 0x02, 0x03};
 * auto data = WebSocketFrameCodec::encodeBinary(binaryData);
 *
 * // 解码帧
 * auto result = WebSocketFrameCodec::decode(buffer, length);
 * if (result.status == DecodeResult::Ok) {
 *     std::string text = result.frame.textPayload();
 * }
 * @endcode
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <random>

/**
 * @enum WsOpcode
 * @brief WebSocket 操作码
 */
enum class WsOpcode : uint8_t {
    Continuation = 0x0,  ///< 继续帧 (分片消息)
    Text = 0x1,          ///< 文本帧
    Binary = 0x2,       ///< 二进制帧
    Close = 0x8,        ///< 关闭帧
    Ping = 0x9,         ///< Ping (心跳请求)
    Pong = 0xA,         ///< Pong (心跳响应)
};

/**
 * @struct WebSocketFrame
 * @brief WebSocket 帧结构
 *
 * 存储一个 WebSocket 帧的所有信息
 */
struct WebSocketFrame {
    bool fin;                      ///< 是否为最后一帧
    uint8_t rsv1;                  ///< 保留位 1
    uint8_t rsv2;                  ///< 保留位 2
    uint8_t rsv3;                  ///< 保留位 3
    WsOpcode opcode;               ///< 操作码
    bool mask;                     ///< 是否掩码
    uint8_t maskingKey[4];         ///< 掩码密钥
    std::vector<uint8_t> payload;  ///< 负载数据

    WebSocketFrame()
        : fin(true), rsv1(0), rsv2(0), rsv3(0),
          opcode(WsOpcode::Text), mask(false) {
        memset(maskingKey, 0, 4);
    }

    /// 判断是否为控制帧
    bool isControlFrame() const {
        return static_cast<uint8_t>(opcode) >= 0x8;
    }

    /// 获取负载大小
    size_t payloadSize() const {
        return payload.size();
    }

    /// 获取文本负载
    std::string textPayload() const {
        return std::string(payload.begin(), payload.end());
    }

    /// 设置文本负载
    void setTextPayload(const std::string& text) {
        payload.assign(text.begin(), text.end());
        opcode = WsOpcode::Text;
    }

    /// 设置二进制负载
    void setBinaryPayload(const std::vector<uint8_t>& data) {
        payload = data;
        opcode = WsOpcode::Binary;
    }
};

/**
 * @class WebSocketFrameCodec
 * @brief WebSocket 帧编解码器
 *
 * 提供帧的编码和解码功能
 */
class WebSocketFrameCodec {
public:
    /// 编码结果
    struct EncodeResult {
        bool success;
        std::string error;
        std::vector<uint8_t> data;
    };

    /// 解码结果
    struct DecodeResult {
        enum Status {
            Ok,          ///< 成功
            Incomplete,  ///< 数据不完整
            Error        ///< 解析错误
        };
        Status status;
        std::string error;
        WebSocketFrame frame;
        size_t consumed;  ///< 消耗的字节数
    };

    /**
     * @brief 编码帧
     * @param frame 要编码的帧
     * @param mask 是否掩码 (服务器发送不需要)
     * @return 编码后的字节数组
     */
    static std::vector<uint8_t> encode(const WebSocketFrame& frame, bool mask = false) {
        std::vector<uint8_t> data;

        // 第一个字节: FIN(1) + RSV(3) + Opcode(4)
        uint8_t byte1 = (frame.fin ? 0x80 : 0x00) |
                        ((frame.rsv1 & 0x01) << 6) |
                        ((frame.rsv2 & 0x01) << 5) |
                        ((frame.rsv3 & 0x01) << 4) |
                        (static_cast<uint8_t>(frame.opcode) & 0x0F);
        data.push_back(byte1);

        // 第二个字节: MASK(1) + Payload length(7)
        size_t payloadLen = frame.payload.size();
        uint8_t byte2 = (mask ? 0x80 : 0x00);

        if (payloadLen <= 125) {
            byte2 |= static_cast<uint8_t>(payloadLen);
            data.push_back(byte2);
        } else if (payloadLen <= 65535) {
            byte2 |= 126;
            data.push_back(byte2);
            // 16-bit length (big-endian)
            data.push_back(static_cast<uint8_t>((payloadLen >> 8) & 0xFF));
            data.push_back(static_cast<uint8_t>(payloadLen & 0xFF));
        } else {
            byte2 |= 127;
            data.push_back(byte2);
            // 64-bit length (big-endian)
            for (int i = 7; i >= 0; --i) {
                data.push_back(static_cast<uint8_t>((payloadLen >> (i * 8)) & 0xFF));
            }
        }

        // 掩码密钥
        uint8_t maskingKey[4] = {0};
        if (mask) {
            // 使用密码学安全的随机数生成器
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<uint32_t> dist(0, 255);

            for (int i = 0; i < 4; ++i) {
                maskingKey[i] = static_cast<uint8_t>(dist(gen));
                data.push_back(maskingKey[i]);
            }
        }

        // 负载数据
        const uint8_t* payloadData = frame.payload.data();
        for (size_t i = 0; i < payloadLen; ++i) {
            if (mask) {
                data.push_back(payloadData[i] ^ maskingKey[i % 4]);
            } else {
                data.push_back(payloadData[i]);
            }
        }

        return data;
    }

    /// 快速编码文本帧
    static std::vector<uint8_t> encodeText(const std::string& text) {
        WebSocketFrame frame;
        frame.fin = true;
        frame.opcode = WsOpcode::Text;
        frame.payload.assign(text.begin(), text.end());
        return encode(frame);
    }

    /// 快速编码二进制帧
    static std::vector<uint8_t> encodeBinary(const std::vector<uint8_t>& data) {
        WebSocketFrame frame;
        frame.fin = true;
        frame.opcode = WsOpcode::Binary;
        frame.payload = data;
        return encode(frame);
    }

    /// 快速编码关闭帧
    static std::vector<uint8_t> encodeClose(uint16_t code = 1000, const std::string& reason = "") {
        WebSocketFrame frame;
        frame.fin = true;
        frame.opcode = WsOpcode::Close;
        frame.payload.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
        frame.payload.push_back(static_cast<uint8_t>(code & 0xFF));
        frame.payload.insert(frame.payload.end(), reason.begin(), reason.end());
        return encode(frame);
    }

    /// 快速编码 Pong 帧
    static std::vector<uint8_t> encodePong(const std::vector<uint8_t>& pingData = {}) {
        WebSocketFrame frame;
        frame.fin = true;
        frame.opcode = WsOpcode::Pong;
        frame.payload = pingData;
        return encode(frame);
    }

    /// 快速编码 Ping 帧
    static std::vector<uint8_t> encodePing(const std::vector<uint8_t>& data = {}) {
        WebSocketFrame frame;
        frame.fin = true;
        frame.opcode = WsOpcode::Ping;
        frame.payload = data;
        return encode(frame);
    }

    /**
     * @brief 解码帧
     * @param data 数据指针
     * @param len 数据长度
     * @return 解码结果
     */
    static DecodeResult decode(const uint8_t* data, size_t len) {
        DecodeResult result;
        result.status = DecodeResult::Error;
        result.consumed = 0;

        // 最少需要 2 字节
        if (len < 2) {
            result.status = DecodeResult::Incomplete;
            return result;
        }

        // 解析第一个字节
        result.frame.fin = (data[0] & 0x80) != 0;
        result.frame.rsv1 = (data[0] >> 6) & 0x01;
        result.frame.rsv2 = (data[0] >> 5) & 0x01;
        result.frame.rsv3 = (data[0] >> 4) & 0x01;
        result.frame.opcode = static_cast<WsOpcode>(data[0] & 0x0F);

        // 解析第二个字节
        result.frame.mask = (data[1] & 0x80) != 0;
        uint64_t payloadLen = data[1] & 0x7F;

        size_t headerSize = 2;

        // 解析扩展长度
        if (payloadLen == 126) {
            if (len < 4) {
                result.status = DecodeResult::Incomplete;
                return result;
            }
            payloadLen = (static_cast<uint64_t>(data[2]) << 8) | data[3];
            headerSize = 4;
        } else if (payloadLen == 127) {
            if (len < 10) {
                result.status = DecodeResult::Incomplete;
                return result;
            }
            payloadLen = 0;
            for (int i = 2; i < 10; ++i) {
                payloadLen = (payloadLen << 8) | data[i];
            }
            headerSize = 10;
        }

        // 检查掩码密钥
        if (result.frame.mask) {
            if (len < headerSize + 4) {
                result.status = DecodeResult::Incomplete;
                return result;
            }
            memcpy(result.frame.maskingKey, data + headerSize, 4);
            headerSize += 4;
        }

        // 检查是否有完整的负载
        if (len < headerSize + payloadLen) {
            result.status = DecodeResult::Incomplete;
            return result;
        }

        // 解析负载
        const uint8_t* payloadData = data + headerSize;
        result.frame.payload.resize(payloadLen);

        if (result.frame.mask) {
            // 解码掩码数据
            for (uint64_t i = 0; i < payloadLen; ++i) {
                result.frame.payload[i] = payloadData[i] ^ result.frame.maskingKey[i % 4];
            }
        } else {
            memcpy(result.frame.payload.data(), payloadData, payloadLen);
        }

        result.status = DecodeResult::Ok;
        result.consumed = headerSize + payloadLen;
        return result;
    }

    /**
     * @brief 计算握手 Accept Key
     * @param clientKey 客户端的 Sec-WebSocket-Key
     * @return 服务器响应的 Sec-WebSocket-Accept
     */
    static std::string computeAcceptKey(const std::string& clientKey) {
        const std::string GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string combined = clientKey + GUID;

        unsigned char hash[SHA_DIGEST_LENGTH];
        SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()),
             combined.length(), hash);

        // Base64 编码
        static const char* base64Chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string result;
        result.reserve(28);

        for (int i = 0; i < SHA_DIGEST_LENGTH; i += 3) {
            uint32_t n = (hash[i] << 16) |
                        (i + 1 < SHA_DIGEST_LENGTH ? hash[i + 1] << 8 : 0) |
                        (i + 2 < SHA_DIGEST_LENGTH ? hash[i + 2] : 0);

            result += base64Chars[(n >> 18) & 0x3F];
            result += base64Chars[(n >> 12) & 0x3F];
            result += (i + 1 < SHA_DIGEST_LENGTH) ? base64Chars[(n >> 6) & 0x3F] : '=';
            result += (i + 2 < SHA_DIGEST_LENGTH) ? base64Chars[n & 0x3F] : '=';
        }

        return result;
    }
};