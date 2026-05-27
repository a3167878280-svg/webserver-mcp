#pragma once

#include <string>

namespace plugin {

// 跨平台动态库加载封装
class DynamicLibrary {
public:
    // 加载动态库，失败返回 nullptr
    static void* open(const std::string& path);

    // 解析符号，失败返回 nullptr
    static void* sym(void* handle, const std::string& symbol_name);

    // 卸载动态库
    static void close(void* handle);

    // 最后一次错误信息
    static std::string error();
};

} // namespace plugin
