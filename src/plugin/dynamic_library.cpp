/**
 * 动态库加载器实现
 *
 * Linux 下核心就是三个 POSIX 调用:
 *   dlopen()  → 把 .so 映射到进程地址空间
 *   dlsym()   → 在 .so 的符号表里按名查找函数地址
 *   dlclose() → 卸载 .so
 *
 * Windows 对应 API: LoadLibrary / GetProcAddress / FreeLibrary
 */

#include "dynamic_library.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace plugin {

void* DynamicLibrary::open(const std::string& path) {
#ifdef _WIN32
    void* handle = LoadLibraryA(path.c_str());
#else
    // RTLD_NOW: 加载时立即解析所有未定义符号，如果找不到直接报错
    // 比 RTLD_LAZY 安全: 不会等到调用时才 segfault
    void* handle = dlopen(path.c_str(), RTLD_NOW);
#endif
    return handle;
}

void* DynamicLibrary::sym(void* handle, const std::string& symbol_name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(
        reinterpret_cast<HMODULE>(handle), symbol_name.c_str()));
#else
    return dlsym(handle, symbol_name.c_str());
#endif
}

void DynamicLibrary::close(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

std::string DynamicLibrary::error() {
#ifdef _WIN32
    DWORD err = GetLastError();
    if (err == 0) return "";
    LPSTR buf = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                   nullptr, err, 0, reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string msg(buf ? buf : "Unknown error");
    if (buf) LocalFree(buf);
    return msg;
#else
    const char* msg = dlerror();
    return msg ? msg : "";
#endif
}

} // namespace plugin
