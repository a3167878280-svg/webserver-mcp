/**
 * 插件管理器 — 插件系统的第 2 层 (加载/卸载)
 *
 * 职责: 扫描插件目录，把每个 .so 文件加载为 IPlugin 实例
 *
 * 加载一个插件的完整流程:
 *
 *   1. dlopen(".so 文件")           → 把 .so 映射到进程空间
 *   2. dlsym(handle, "create_plugin") → 找到 C 入口函数 create_plugin 的地址
 *   3. dlsym(handle, "destroy_plugin")→ 找到 C 入口函数 destroy_plugin 的地址
 *   4. create_plugin()              → 调用, 拿到 IPlugin* 实例 (C++ 对象)
 *   5. plugin->get_tools()          → 问插件提供了哪些工具
 *   6. registry.register_plugin()   → 把工具注册到全局注册表
 *
 * 卸载顺序是 reverse (后进先出) — 防止插件间依赖问题
 *
 * ═══════════ 为什么用 dlopen 而不是静态链接? ═══════════
 *
 *   1. 热插拔: 加新工具不需要重编译主程序，写个 .so 丢进 plugins/ 目录即可
 *   2. 隔离: 插件 crash 不会带崩主程序 (只要 catch 住异常)
 *   3. 独立开发: 插件和主程序只需要约定 IPlugin 接口，各自独立编译
 *   4. 按需加载: 不需要的插件删掉 .so 文件就行
 */

#include "plugin_manager.h"
#include "dynamic_library.h"
#include "../log/log.h"
#include "../common.h"
#include <filesystem>

namespace plugin {

namespace fs = std::filesystem;

PluginManager::~PluginManager() {
    unload_all();
}

/**
 * 扫描插件目录，加载所有 .so 文件
 *
 * 只看文件扩展名: .so (Linux), .dylib (macOS), .dll (Windows)
 * 也会匹配 .so.1.0 这样的带版本号的文件
 */
int PluginManager::load_all(const std::string& plugin_dir) {
    int count = 0;

    if (!fs::exists(plugin_dir) || !fs::is_directory(plugin_dir)) {
        LOG_WARN("Plugin directory not found: %s", plugin_dir.c_str());
        return 0;
    }

    for (const auto& entry : fs::directory_iterator(plugin_dir)) {
        if (!entry.is_regular_file()) continue;

        std::string path = entry.path().string();
        // 根据平台判断是否是动态库文件
#ifdef _WIN32
        bool is_lib = path.size() > 4 &&
                      path.compare(path.size() - 4, 4, ".dll") == 0;
#elif defined(__APPLE__)
        bool is_lib = path.size() > 6 &&
                      path.compare(path.size() - 6, 6, ".dylib") == 0;
#else
        bool is_lib = path.size() > 3 &&
                      (path.compare(path.size() - 3, 3, ".so") == 0 ||
                       path.find(".so.") != std::string::npos);  // 也匹配 .so.1.0 等
#endif
        if (!is_lib) continue;

        if (load_single(path)) {
            count++;
        }
    }

    return count;
}

/**
 * 加载单个 .so 插件文件
 *
 * 每个 .so 必须导出两个 C 符号 (extern "C" 防止 name mangling):
 *
 *   extern "C" IPlugin* create_plugin()   — 工厂函数, new 一个插件实例
 *   extern "C" void destroy_plugin(IPlugin*) — 析构函数, delete 实例
 *
 * 为什么必须是 extern "C"?
 *   C++ 编译器会把函数名 mangling 成类似 _Z13create_pluginv 这样的乱码，
 *   dlsym 按字符串查找就找不到了。extern "C" 保持符号名为 create_plugin。
 */
bool PluginManager::load_single(const std::string& so_path) {
    // 步骤 1: dlopen — 把 .so 加载到进程空间
    void* handle = DynamicLibrary::open(so_path);
    if (!handle) {
        LOG_ERROR("Failed to dlopen %s: %s", so_path.c_str(),
                  DynamicLibrary::error().c_str());
        return false;
    }

    // 函数指针类型定义
    using CreateFn = IPlugin* (*)();
    using DestroyFn = void (*)(IPlugin*);

    // 步骤 2: dlsym — 查找 create_plugin 符号
    auto* create = reinterpret_cast<CreateFn>(
        DynamicLibrary::sym(handle, "create_plugin"));
    if (!create) {
        LOG_ERROR("Symbol 'create_plugin' not found in %s", so_path.c_str());
        DynamicLibrary::close(handle);
        return false;
    }

    // 步骤 3: dlsym — 查找 destroy_plugin 符号 (可选，没有就泄漏)
    auto* destroy = reinterpret_cast<DestroyFn>(
        DynamicLibrary::sym(handle, "destroy_plugin"));
    if (!destroy) {
        LOG_WARN("Symbol 'destroy_plugin' not found in %s, plugin will leak on unload",
                 so_path.c_str());
    }

    // 步骤 4: 调用 create_plugin() 拿到 C++ 对象
    IPlugin* instance = create();
    if (!instance) {
        LOG_ERROR("create_plugin() returned null for %s", so_path.c_str());
        DynamicLibrary::close(handle);
        return false;
    }

    LOG_INFO("Loaded plugin: %s v%s (%s)", instance->name(),
             instance->version(), so_path.c_str());

    // 步骤 5: 把插件提供的工具注册到全局注册表
    m_registry.register_plugin(instance);

    // 步骤 6: 记录到已加载列表 (用于卸载)
    m_loaded.push_back({handle, instance, destroy, so_path});

    return true;
}

/**
 * 卸载所有插件 — 逆序卸载 (后进先出)
 * 顺序: 反注册 → 调 destroy_plugin() → dlclose
 */
void PluginManager::unload_all() {
    for (auto it = m_loaded.rbegin(); it != m_loaded.rend(); ++it) {
        m_registry.unregister_plugin(it->instance);  // 从注册表移除

        if (it->destroy) {
            it->destroy(it->instance);  // delete 插件对象
        }
        if (it->handle) {
            DynamicLibrary::close(it->handle);  // dlclose
        }
        LOG_INFO("Unloaded plugin: %s", it->path.c_str());
    }
    m_loaded.clear();
}

} // namespace plugin
