#include <iostream>
#include <cassert>
#include <cstring>
#include "src/http/GzipMiddleware.h"

using namespace std;

void testCompress() {
    cout << "=== Testing Gzip Compress ===" << endl;
    string input = "Hello World! This is a test string for gzip compression. "
                   "Repeated content helps compression ratio. "
                   "Hello World! This is a test string for gzip compression.";
    auto compressed = GzipCodec::compress(input);
    assert(!compressed.empty());
    assert(compressed.size() < input.size());
    cout << "Compress test passed!" << endl;
}

void testDecompress() {
    cout << "=== Testing Gzip Decompress ===" << endl;
    string input = "Hello World! Compress then decompress should return original.";
    auto compressed = GzipCodec::compress(input);
    auto decompressed = GzipCodec::decompress(compressed);
    assert(decompressed == input);
    cout << "Decompress test passed!" << endl;
}

void testEmptyInput() {
    cout << "=== Testing Empty Input ===" << endl;
    auto compressed = GzipCodec::compress("");
    assert(compressed.empty());
    cout << "Empty input test passed!" << endl;
}

void testLargeData() {
    cout << "=== Testing Large Data ===" << endl;
    string large(1024 * 1024, 'A');
    auto compressed = GzipCodec::compress(large);
    assert(compressed.size() < large.size() / 100);
    auto decompressed = GzipCodec::decompress(compressed);
    assert(decompressed == large);
    cout << "Large data test passed!" << endl;
}

void testShouldCompress() {
    cout << "=== Testing shouldCompress ===" << endl;
    assert(GzipCodec::shouldCompress("text/html"));
    assert(GzipCodec::shouldCompress("text/plain; charset=utf-8"));
    assert(GzipCodec::shouldCompress("application/json"));
    assert(GzipCodec::shouldCompress("application/javascript"));
    assert(!GzipCodec::shouldCompress("image/png"));
    assert(!GzipCodec::shouldCompress("image/jpeg"));
    assert(!GzipCodec::shouldCompress("application/octet-stream"));
    cout << "shouldCompress test passed!" << endl;
}

int main() {
    cout << "Starting Gzip Tests..." << endl << endl;
    testCompress();
    testDecompress();
    testEmptyInput();
    testLargeData();
    testShouldCompress();
    cout << endl << "All Gzip tests passed!" << endl;
    return 0;
}
