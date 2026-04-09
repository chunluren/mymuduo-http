// test_config.cpp - Config class tests
#include <iostream>
#include <cassert>
#include <fstream>
#include <string>
#include <cstdio>
#include "config/Config.h"

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    test_##name(); \
    std::cout << "PASSED" << std::endl; \
} while(0)

// Helper: write a temporary INI file and return its path
static std::string writeTempConfig(const std::string& content) {
    std::string path = "/tmp/test_config_mymuduo.ini";
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

TEST(loadAndGetString) {
    std::string path = writeTempConfig(
        "host=127.0.0.1\n"
        "name=\"mymuduo-http\"\n"
    );

    Config& cfg = Config::instance();
    bool ok = cfg.load(path);
    assert(ok);

    assert(cfg.get("host").asString() == "127.0.0.1");
    // Quoted values should have quotes stripped
    assert(cfg.get("name").asString() == "mymuduo-http");
}

TEST(getIntAndBoolValues) {
    std::string path = writeTempConfig(
        "port=8080\n"
        "threads=4\n"
        "debug=true\n"
        "verbose=0\n"
    );

    Config& cfg = Config::instance();
    cfg.load(path);

    assert(cfg.get("port").asInt() == 8080);
    assert(cfg.get("threads").asInt() == 4);
    assert(cfg.get("debug").asBool() == true);
    assert(cfg.get("verbose").asBool() == false);
}

TEST(defaultValueForMissingKey) {
    Config& cfg = Config::instance();

    // Key that does not exist should return the provided default
    assert(cfg.get("nonexistent_key_xyz", "fallback").asString() == "fallback");
    assert(cfg.get("nonexistent_key_xyz", "42").asInt() == 42);

    // has() should return false
    assert(cfg.has("nonexistent_key_xyz") == false);
}

TEST(sections) {
    std::string path = writeTempConfig(
        "[server]\n"
        "port=9090\n"
        "host=0.0.0.0\n"
        "\n"
        "[database]\n"
        "host=localhost\n"
        "port=3306\n"
    );

    Config& cfg = Config::instance();
    cfg.load(path);

    // Section keys are stored as "section.key"
    assert(cfg.get("server.port").asInt() == 9090);
    assert(cfg.get("server.host").asString() == "0.0.0.0");
    assert(cfg.get("database.host").asString() == "localhost");
    assert(cfg.get("database.port").asInt() == 3306);
}

TEST(setAndHas) {
    Config& cfg = Config::instance();
    cfg.set("runtime.mode", "production");

    assert(cfg.has("runtime.mode") == true);
    assert(cfg.get("runtime.mode").asString() == "production");
}

TEST(reload) {
    std::string path = writeTempConfig("value=first\n");

    Config& cfg = Config::instance();
    cfg.load(path);
    assert(cfg.get("value").asString() == "first");

    // Overwrite the file with a new value and reload
    writeTempConfig("value=second\n");
    bool ok = cfg.reload();
    assert(ok);
    assert(cfg.get("value").asString() == "second");

    // Clean up
    std::remove(path.c_str());
}

int main() {
    std::cout << "=== Config Tests ===" << std::endl;

    RUN_TEST(loadAndGetString);
    RUN_TEST(getIntAndBoolValues);
    RUN_TEST(defaultValueForMissingKey);
    RUN_TEST(sections);
    RUN_TEST(setAndHas);
    RUN_TEST(reload);

    std::cout << std::endl << "All Config tests passed!" << std::endl;
    return 0;
}
