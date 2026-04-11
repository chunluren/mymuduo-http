#include <iostream>
#include <cassert>
#include "src/http/HttpResponse.h"

using namespace std;

void testChunkedEncode() {
    cout << "=== Testing Chunked Encoding ===" << endl;

    HttpResponse resp;
    resp.setStatusCode(HttpStatusCode::OK);
    resp.setContentType("text/plain");
    resp.setChunked(true);

    resp.addChunk("Hello ");
    resp.addChunk("World!");
    resp.addChunk("");  // end

    string result = resp.toString();

    assert(result.find("Transfer-Encoding: chunked") != string::npos);
    assert(result.find("Content-Length") == string::npos);
    assert(result.find("6\r\nHello \r\n") != string::npos);
    assert(result.find("6\r\nWorld!\r\n") != string::npos);
    assert(result.find("0\r\n\r\n") != string::npos);

    cout << "Chunked encoding test passed!" << endl;
}

void testNonChunkedUnchanged() {
    cout << "=== Testing Non-Chunked Unchanged ===" << endl;

    HttpResponse resp;
    resp.setStatusCode(HttpStatusCode::OK);
    resp.setText("Hello");

    string result = resp.toString();
    assert(result.find("Content-Length: 5") != string::npos);
    assert(result.find("Transfer-Encoding") == string::npos);

    cout << "Non-chunked test passed!" << endl;
}

void testChunkedHexLength() {
    cout << "=== Testing Chunked Hex Length ===" << endl;

    HttpResponse resp;
    resp.setStatusCode(HttpStatusCode::OK);
    resp.setContentType("text/plain");
    resp.setChunked(true);

    string data(256, 'A');
    resp.addChunk(data);
    resp.addChunk("");

    string result = resp.toString();
    assert(result.find("100\r\n") != string::npos);

    cout << "Chunked hex length test passed!" << endl;
}

int main() {
    cout << "Starting Chunked Transfer Tests..." << endl << endl;
    testChunkedEncode();
    testNonChunkedUnchanged();
    testChunkedHexLength();
    cout << endl << "All Chunked tests passed!" << endl;
    return 0;
}
