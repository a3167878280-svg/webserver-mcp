/**
 * 动态库加载器 — 最底层的 dlopen 封装
 *
 * 这是插件系统的第 1 层 (最底层)，封装了操作系统原生的动态库 API:
 *   Linux:   dlopen / dlsym / dlclose / dlerror
 *   Windows: LoadLibrary / GetProcAddress / FreeLibrary / GetLastError
 *   macOS:   同 Linux (都用的 dlfcn.h)
 *
 * 插件就是 .so 文件 (Linux) 或 .dll 文件 (Windows)，
 * 运行时动态加载到进程空间，不需要重新编译主程序。
 */

#pragma once

#include <string>

namespace plugin {

class DynamicLibrary {
public:
    // 加载 .so 文件到内存，返回句柄 (失败返回 nullptr)
    // Linux: dlopen(path, RTLD_NOW)  — RTLD_NOW 表示立即解析所有符号
    static void* open(const std::string& path);

    // 从句柄中查找符号地址 (函数指针)
    // 例: sym(handle, "create_plugin") → &create_plugin 函数
    static void* sym(void* handle, const std::string& symbol_name);

    // 卸载动态库 (引用计数-1，归零时从内存移除)
    static void close(void* handle);

    // 最后一次错误信息 (dlerror / GetLastError)
    static std::string error();
};

} // namespace plugin
