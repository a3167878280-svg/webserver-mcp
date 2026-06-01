cmake_minimum_required(VERSION 3.14)
project(TinyWebServer-MCP VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源文件
set(SOURCES
    src/main.cpp
    src/config.cpp
    src/log/log.cpp
    src/mcp/jsonrpc_parser.cpp
    src/mcp/jsonrpc_serializer.cpp
    src/mcp/mcp_handler.cpp
    src/transport/stdio_transport.cpp
    src/transport/http_sse_transport.cpp
    src/plugin/plugin_registry.cpp
    src/plugin/plugin_manager.cpp
    src/plugin/dynamic_library.cpp
    src/server/conversation_manager.cpp
    src/llm/llm_client.cpp
    src/llm/tool_orchestrator.cpp
)

add_executable(mcp_server ${SOURCES})

# 包含路径
target_include_directories(mcp_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/log
    ${CMAKE_SOURCE_DIR}/src/lock
    ${CMAKE_SOURCE_DIR}/src/threadpool
    ${CMAKE_SOURCE_DIR}/src/mcp
    ${CMAKE_SOURCE_DIR}/src/transport
    ${CMAKE_SOURCE_DIR}/src/plugin
    ${CMAKE_SOURCE_DIR}/src/llm
    ${CMAKE_SOURCE_DIR}/src/server
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(mcp_server PRIVATE
    pthread
    ${CMAKE_DL_LIBS}
    stdc++fs
    ssl
    crypto
)

target_compile_definitions(mcp_server PRIVATE
    CPPHTTPLIB_OPENSSL_SUPPORT
)

# 插件子项目
add_subdirectory(plugins/file_plugin)
add_subdirectory(plugins/weather_plugin)
add_subdirectory(plugins/review_plugin)
add_subdirectory(plugins/bilibili_plugin)
add_subdirectory(plugins/command_plugin)
