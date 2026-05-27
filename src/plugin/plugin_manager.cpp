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

int PluginManager::load_all(const std::string& plugin_dir) {
    int count = 0;

    if (!fs::exists(plugin_dir) || !fs::is_directory(plugin_dir)) {
        LOG_WARN("Plugin directory not found: %s", plugin_dir.c_str());
        return 0;
    }

    for (const auto& entry : fs::directory_iterator(plugin_dir)) {
        if (!entry.is_regular_file()) continue;

        std::string path = entry.path().string();
#ifdef _WIN32
        bool is_lib = path.size() > 4 &&
                      path.compare(path.size() - 4, 4, ".dll") == 0;
#elif defined(__APPLE__)
        bool is_lib = path.size() > 6 &&
                      path.compare(path.size() - 6, 6, ".dylib") == 0;
#else
        bool is_lib = path.size() > 3 &&
                      (path.compare(path.size() - 3, 3, ".so") == 0 ||
                       path.find(".so.") != std::string::npos);
#endif
        if (!is_lib) continue;

        if (load_single(path)) {
            count++;
        }
    }

    return count;
}

bool PluginManager::load_single(const std::string& so_path) {
    void* handle = DynamicLibrary::open(so_path);
    if (!handle) {
        LOG_ERROR("Failed to dlopen %s: %s", so_path.c_str(),
                  DynamicLibrary::error().c_str());
        return false;
    }

    using CreateFn = IPlugin* (*)();
    using DestroyFn = void (*)(IPlugin*);

    auto* create = reinterpret_cast<CreateFn>(
        DynamicLibrary::sym(handle, "create_plugin"));
    if (!create) {
        LOG_ERROR("Symbol 'create_plugin' not found in %s", so_path.c_str());
        DynamicLibrary::close(handle);
        return false;
    }

    auto* destroy = reinterpret_cast<DestroyFn>(
        DynamicLibrary::sym(handle, "destroy_plugin"));
    if (!destroy) {
        LOG_WARN("Symbol 'destroy_plugin' not found in %s, plugin will leak on unload",
                 so_path.c_str());
    }

    IPlugin* instance = create();
    if (!instance) {
        LOG_ERROR("create_plugin() returned null for %s", so_path.c_str());
        DynamicLibrary::close(handle);
        return false;
    }

    LOG_INFO("Loaded plugin: %s v%s (%s)", instance->name(),
             instance->version(), so_path.c_str());

    m_registry.register_plugin(instance);
    m_loaded.push_back({handle, instance, destroy, so_path});

    return true;
}

void PluginManager::unload_all() {
    for (auto it = m_loaded.rbegin(); it != m_loaded.rend(); ++it) {
        m_registry.unregister_plugin(it->instance);

        if (it->destroy) {
            it->destroy(it->instance);
        }
        if (it->handle) {
            DynamicLibrary::close(it->handle);
        }
        LOG_INFO("Unloaded plugin: %s", it->path.c_str());
    }
    m_loaded.clear();
}

} // namespace plugin
