// test_websocket_frame.cpp - WebSocket 帧编解码测试
#include <iostream>
#include <cassert>
#include <cstring>
#include "src/websocket/WebSocketFrame.h"

using namespace std;

void testFrameCodec() {
    cout << "=== Testing WebSocketFrameCodec ===" << endl;

    // 测试文本帧编码
    WebSocketFrame textFrame;
    textFrame.fin = true;
    textFrame.opcode = WsOpcode::Text;
    textFrame.payload = {'H', 'e', 'l', 'l', 'o'};

    auto encoded = WebSocketFrameCodec::encode(textFrame);
    // 文本帧：FIN=1, opcode=1, length=5
    assert(encoded.size() == 7);  // 2 bytes header + 5 bytes payload
    assert((encoded[0] & 0x80) != 0);  // FIN set
    assert((encoded[0] & 0x0F) == 0x01);  // Text opcode

    // 测试解码
    auto decoded = WebSocketFrameCodec::decode(encoded.data(), encoded.size());
    assert(decoded.status == WebSocketFrameCodec::DecodeResult::Ok);
    assert(decoded.frame.fin == true);
    assert(decoded.frame.opcode == WsOpcode::Text);
    assert(decoded.frame.payload.size() == 5);
    assert(decoded.frame.textPayload() == "Hello");

    cout << "Frame encoding/decoding test passed!" << endl;
}

void testBinaryFrame() {
    cout << "=== Testing Binary Frame ===" << endl;

    std::vector<uint8_t> binaryData = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE};
    auto encoded = WebSocketFrameCodec::encodeBinary(binaryData);

    auto decoded = WebSocketFrameCodec::decode(encoded.data(), encoded.size());
    assert(decoded.status == WebSocketFrameCodec::DecodeResult::Ok);
    assert(decoded.frame.opcode == WsOpcode::Binary);
    assert(decoded.frame.payload == binaryData);

    cout << "Binary frame test passed!" << endl;
}

void testCloseFrame() {
    cout << "=== Testing Close Frame ===" << endl;

    auto encoded = WebSocketFrameCodec::encodeClose(1000, "Normal Closure");

    auto decoded = WebSocketFrameCodec::decode(encoded.data(), encoded.size());
    assert(decoded.status == WebSocketFrameCodec::DecodeResult::Ok);
    assert(decoded.frame.opcode == WsOpcode::Close);
    assert(decoded.frame.payload.size() >= 2);

    // 解析关闭码
    uint16_t code = (static_cast<uint16_t>(decoded.frame.payload[0]) << 8) |
                    static_cast<uint16_t>(decoded.frame.payload[1]);
    assert(code == 1000);

    cout << "Close frame test passed!" << endl;
}

void testPingPongFrames() {
    cout << "=== Testing Ping/Pong Frames ===" << endl;

    // Ping
    std::vector<uint8_t> pingData = {'p', 'i', 'n', 'g'};
    auto pingEncoded = WebSocketFrameCodec::encodePing(pingData);

    auto pingDecoded = WebSocketFrameCodec::decode(pingEncoded.data(), pingEncoded.size());
    assert(pingDecoded.status == WebSocketFrameCodec::DecodeResult::Ok);
    assert(pingDecoded.frame.opcode == WsOpcode::Ping);
    assert(pingDecoded.frame.payload == pingData);

    // Pong
    auto pongEncoded = WebSocketFrameCodec::encodePong(pingData);

    auto pongDecoded = WebSocketFrameCodec::decode(pongEncoded.data(), pongEncoded.size());
    assert(pongDecoded.status == WebSocketFrameCodec::DecodeResult::Ok);
    assert(pongDecoded.frame.opcode == WsOpcode::Pong);
    assert(pongDecoded.frame.payload == pingData);

    cout << "Ping/Pong frame test passed!" << endl;
}

void testMaskedFrame() {
    cout << "=== Testing Masked Frame ===" << endl;

    WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = WsOpcode::Text;
    frame.payload = {'H', 'e', 'l', 'l', 'o'};

    // 客户端需要掩码
    auto encoded = WebSocketFrameCodec::encode(frame, true);
    assert((encoded[1] & 0x80) != 0);  // MASK bit set
    assert(encoded.size() == 11);  // 2 header + 4 mask key + 5 payload

    // 解码时自动解掩码
    auto decoded = WebSocketFrameCodec::decode(encoded.data(), encoded.size());
    assert(decoded.status == WebSocketFrameCodec::DecodeResult::Ok);
    assert(decoded.frame.textPayload() == "Hello");

    cout << "Masked frame test passed!" << endl;
}

void testLargeFrame() {
    cout << "=== Testing Large Frame ===" << endl;

    // 测试 16-bit 长度
    std::vector<uint8_t> largeData(1000, 'X');
    WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = WsOpcode::Binary;
    frame.payload = largeData;

    auto encoded = WebSocketFrameCodec::encode(frame);
    // 2 bytes header + 2 bytes extended length + payload
    assert(encoded.size() == 1004);
    assert((encoded[1] & 0x7F) == 126);  // 16-bit length indicator

    auto decoded = WebSocketFrameCodec::decode(encoded.data(), encoded.size());
    assert(decoded.status == WebSocketFrameCodec::DecodeResult::Ok);
    assert(decoded.frame.payload.size() == 1000);

    cout << "Large frame test passed!" << endl;
}

void testAcceptKey() {
    cout << "=== Testing Accept Key ===" << endl;

    // RFC 6455 示例
    std::string clientKey = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string acceptKey = WebSocketFrameCodec::computeAcceptKey(clientKey);

    // 预期结果: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
    assert(acceptKey == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    cout << "Accept key test passed!" << endl;
}

void testIncompleteFrame() {
    cout << "=== Testing Incomplete Frame ===" << endl;

    // 只有部分数据
    uint8_t partialData[] = {0x81, 0x05, 'H', 'e'};  // 声明长度5，但只有2字节负载
    auto result = WebSocketFrameCodec::decode(partialData, sizeof(partialData));

    assert(result.status == WebSocketFrameCodec::DecodeResult::Incomplete);

    cout << "Incomplete frame test passed!" << endl;
}

int main() {
    cout << "Starting WebSocket Frame Tests..." << endl << endl;

    testFrameCodec();
    testBinaryFrame();
    testCloseFrame();
    testPingPongFrames();
    testMaskedFrame();
    testLargeFrame();
    testAcceptKey();
    testIncompleteFrame();

    cout << endl << "All WebSocket Frame tests passed!" << endl;
    return 0;
}