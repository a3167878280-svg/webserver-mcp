#pragma once

#include "plugin_registry.h"
#include <string>
#include <vector>

namespace plugin {

class PluginManager {
public:
    PluginManager() = default;
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // 扫描 plugin_dir 目录下所有 .so 文件并加载
    // 返回成功加载的插件数量
    int load_all(const std::string& plugin_dir);

    // 卸载全部插件
    void unload_all();

    PluginRegistry& registry() { return m_registry; }
    const PluginRegistry& registry() const { return m_registry; }

    int loaded_count() const { return static_cast<int>(m_loaded.size()); }

private:
    struct LoadedPlugin {
        void* handle = nullptr;
        IPlugin* instance = nullptr;
        void (*destroy)(IPlugin*) = nullptr;
        std::string path;
    };

    bool load_single(const std::string& so_path);

    PluginRegistry m_registry;
    std::vector<LoadedPlugin> m_loaded;
};

} // namespace plugin
