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
