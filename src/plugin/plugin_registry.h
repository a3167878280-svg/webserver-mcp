#pragma once

#include "plugin_interface.h"
#include "../mcp/mcp_types.h"
#include <string>
#include <unordered_map>
#include <optional>
#include <vector>

namespace plugin {

struct PluginEntry {
    IPlugin* plugin;         // 非拥有指针 (PluginManager 管理生命周期)
    mcp::ToolDef tool_def;
};

/**
 * 插件注册表 — Tools + Resources 的统一索引
 *
 * 维护两个映射:
 *   m_tools:     工具名 → {IPlugin*, ToolDef}     (O(1) 查找)
 *   m_resources: 资源URI → {IPlugin*, ResourceDef} (O(1) 查找)
 */
class PluginRegistry {
public:
    // ── 工具操作 ──
    void register_plugin(IPlugin* plugin);
    void unregister_plugin(IPlugin* plugin);

    std::vector<mcp::ToolDef> get_all_tools() const;
    std::optional<mcp::ToolCallResult> call_tool(
        const std::string& tool_name,
        const nlohmann::json& arguments);

    // ── 资源操作 ──
    std::vector<mcp::ResourceDef> get_all_resources() const;
    std::optional<mcp::ResourceReadResult> read_resource(const std::string& uri);

    size_t size() const { return m_tools.size(); }

private:
    std::unordered_map<std::string, PluginEntry> m_tools;
    std::unordered_map<std::string, std::pair<IPlugin*, mcp::ResourceDef>> m_resources;
};

} // namespace plugin
