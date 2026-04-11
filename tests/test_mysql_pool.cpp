#include <iostream>
#include <cassert>
#include "src/pool/MySQLPool.h"

using namespace std;

void testMySQLConnectionWrapper() {
    cout << "=== Testing MySQLConnection Wrapper ===" << endl;
    MySQLConnection conn(nullptr);
    assert(!conn.valid());
    assert(!conn.ping());
    cout << "MySQLConnection wrapper test passed!" << endl;
}

void testMySQLPoolConfig() {
    cout << "=== Testing MySQLPool Config ===" << endl;
    MySQLPoolConfig config;
    config.host = "127.0.0.1";
    config.port = 3306;
    config.user = "root";
    config.password = "";
    config.database = "test";
    config.minSize = 2;
    config.maxSize = 10;
    config.idleTimeoutSec = 60;
    assert(config.host == "127.0.0.1");
    assert(config.port == 3306);
    assert(config.minSize == 2);
    assert(config.maxSize == 10);
    cout << "MySQLPool config test passed!" << endl;
}

void testMySQLPoolCreation() {
    cout << "=== Testing MySQLPool Creation (no DB required) ===" << endl;
    MySQLPoolConfig config;
    config.host = "127.0.0.1";
    config.port = 3306;
    config.user = "nonexistent";
    config.password = "wrong";
    config.database = "nodb";
    config.minSize = 0;  // don't pre-create
    config.maxSize = 5;
    MySQLPool pool(config);
    assert(pool.available() == 0);
    assert(pool.totalCreated() == 0);
    assert(!pool.isClosed());
    cout << "MySQLPool creation test passed!" << endl;
}

int main() {
    cout << "Starting MySQLPool Tests..." << endl << endl;
    testMySQLConnectionWrapper();
    testMySQLPoolConfig();
    testMySQLPoolCreation();
    cout << endl << "All MySQLPool tests passed!" << endl;
    return 0;
}
