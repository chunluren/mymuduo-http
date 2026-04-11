#include <iostream>
#include <cassert>
#include "src/http/MultipartParser.h"

using namespace std;

void testExtractBoundary() {
    cout << "=== Testing Extract Boundary ===" << endl;
    string ct = "multipart/form-data; boundary=----WebKitFormBoundary7MA4";
    string boundary = MultipartParser::extractBoundary(ct);
    assert(boundary == "----WebKitFormBoundary7MA4");
    cout << "Extract boundary test passed!" << endl;
}

void testParseTextFields() {
    cout << "=== Testing Parse Text Fields ===" << endl;

    string boundary = "----boundary123";
    string body =
        "------boundary123\r\n"
        "Content-Disposition: form-data; name=\"username\"\r\n"
        "\r\n"
        "alice\r\n"
        "------boundary123\r\n"
        "Content-Disposition: form-data; name=\"password\"\r\n"
        "\r\n"
        "secret123\r\n"
        "------boundary123--\r\n";

    auto parts = MultipartParser::parse(body, boundary);
    assert(parts.size() == 2);
    assert(parts[0].name == "username");
    assert(parts[0].data == "alice");
    assert(!parts[0].isFile());
    assert(parts[1].name == "password");
    assert(parts[1].data == "secret123");

    cout << "Parse text fields test passed!" << endl;
}

void testParseFileUpload() {
    cout << "=== Testing Parse File Upload ===" << endl;

    string boundary = "----boundary456";
    string body =
        "------boundary456\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello World!\r\n"
        "------boundary456\r\n"
        "Content-Disposition: form-data; name=\"description\"\r\n"
        "\r\n"
        "A test file\r\n"
        "------boundary456--\r\n";

    auto parts = MultipartParser::parse(body, boundary);
    assert(parts.size() == 2);
    assert(parts[0].name == "file");
    assert(parts[0].filename == "test.txt");
    assert(parts[0].contentType == "text/plain");
    assert(parts[0].data == "Hello World!");
    assert(parts[0].isFile());
    assert(parts[1].name == "description");
    assert(parts[1].data == "A test file");

    cout << "Parse file upload test passed!" << endl;
}

void testEmptyBody() {
    cout << "=== Testing Empty Body ===" << endl;
    auto parts = MultipartParser::parse("", "boundary");
    assert(parts.empty());
    cout << "Empty body test passed!" << endl;
}

int main() {
    cout << "Starting Multipart Parser Tests..." << endl << endl;
    testExtractBoundary();
    testParseTextFields();
    testParseFileUpload();
    testEmptyBody();
    cout << endl << "All Multipart tests passed!" << endl;
    return 0;
}
