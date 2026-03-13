cmake_minimum_required(VERSION 3.10)
project(mymuduo-http)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -O2")

# mymuduo 库
aux_source_directory(. SRC_LIST)
add_library(mymuduo SHARED ${SRC_LIST})

# nlohmann/json (单头文件)
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found, will download")
    include(FetchContent)
    FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz)
    FetchContent_MakeAvailable(json)
endif()

# HTTP Server 示例
add_executable(http_server examples/http_server.cpp)
target_link_libraries(http_server mymuduo nlohmann_json::nlohmann_json pthread)

# RPC Server 示例
add_executable(rpc_server examples/rpc_server.cpp)
target_link_libraries(rpc_server mymuduo nlohmann_json::nlohmann_json pthread)

# RPC Client 示例
add_executable(rpc_client examples/rpc_client.cpp)
target_link_libraries(rpc_client nlohmann_json::nlohmann_json pthread)

# 测试
add_executable(test_http tests/test_http.cpp)
target_link_libraries(test_http mymuduo nlohmann_json::nlohmann_json pthread)