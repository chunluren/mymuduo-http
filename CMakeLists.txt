cmake_minimum_required(VERSION 3.10)
project(mymuduo-http)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -O2")

# mymuduo 库
file(GLOB MUDUO_SOURCES "*.cc")
add_library(mymuduo SHARED ${MUDUO_SOURCES})
target_include_directories(mymuduo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# nlohmann/json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found, downloading...")
    include(FetchContent)
    FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz)
    FetchContent_MakeAvailable(json)
endif()

# 头文件目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/timer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/asynclogger
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pool
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
)

# HTTP Server
add_executable(http_server examples/http_server.cpp)
target_link_libraries(http_server mymuduo nlohmann_json::nlohmann_json pthread)

# RPC Server
add_executable(rpc_server examples/rpc_server.cpp)
target_link_libraries(rpc_server mymuduo nlohmann_json::nlohmann_json pthread)

# RPC Client
add_executable(rpc_client examples/rpc_client.cpp)
target_link_libraries(rpc_client nlohmann_json::nlohmann_json pthread)

# Full Server (使用所有模块)
add_executable(full_server examples/full_server.cpp)
target_link_libraries(full_server mymuduo nlohmann_json::nlohmann_json pthread)

# 安装
install(TARGETS mymuduo LIBRARY DESTINATION lib)
install(DIRECTORY src/ DESTINATION include/mymuduo-http FILES_MATCHING PATTERN "*.h")