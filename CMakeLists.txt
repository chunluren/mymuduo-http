cmake_minimum_required(VERSION 3.10)
project(mymuduo-http)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -O2")

# 查找 Protobuf
find_package(Protobuf REQUIRED)

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

# 生成 Protobuf 代码
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc/proto/rpc.proto)
set(PROTO_SRC ${CMAKE_CURRENT_BINARY_DIR}/rpc.pb.cc)
set(PROTO_HDR ${CMAKE_CURRENT_BINARY_DIR}/rpc.pb.h)

add_custom_command(
    OUTPUT ${PROTO_SRC} ${PROTO_HDR}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/src/rpc/proto ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "Generating protobuf sources"
)

add_library(rpc_proto STATIC ${PROTO_SRC})
target_include_directories(rpc_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${Protobuf_INCLUDE_DIRS})
target_link_libraries(rpc_proto ${Protobuf_LIBRARIES})

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
    ${CMAKE_CURRENT_BINARY_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

# HTTP Server
add_executable(http_server examples/http_server.cpp)
target_link_libraries(http_server mymuduo nlohmann_json::nlohmann_json pthread)

# RPC Server (JSON)
add_executable(rpc_server examples/rpc_server.cpp)
target_link_libraries(rpc_server mymuduo nlohmann_json::nlohmann_json pthread)

# RPC Client (JSON)
add_executable(rpc_client examples/rpc_client.cpp)
target_link_libraries(rpc_client mymuduo nlohmann_json::nlohmann_json pthread)

# RPC Server (Protobuf)
add_executable(rpc_server_pb examples/rpc_server_pb.cpp)
target_link_libraries(rpc_server_pb mymuduo rpc_proto ${Protobuf_LIBRARIES} pthread)

# RPC Client (Protobuf)
add_executable(rpc_client_pb examples/rpc_client_pb.cpp)
target_link_libraries(rpc_client_pb mymuduo rpc_proto ${Protobuf_LIBRARIES} pthread)

# Full Server
add_executable(full_server examples/full_server.cpp)
target_link_libraries(full_server mymuduo nlohmann_json::nlohmann_json pthread)

# 安装
install(TARGETS mymuduo rpc_proto LIBRARY DESTINATION lib)
install(DIRECTORY src/ DESTINATION include/mymuduo-http FILES_MATCHING PATTERN "*.h")
install(FILES ${PROTO_HDR} DESTINATION include/mymuduo-http/rpc/proto)