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

class PluginRegistry {
public:
    // 注册插件的所有工具，同名工具跳过并输出警告
    void register_plugin(IPlugin* plugin);

    // 移除某插件的所有工具条目 (卸载时调用)
    void unregister_plugin(IPlugin* plugin);

    // 获取全部已注册工具定义 (for tools/list)
    std::vector<mcp::ToolDef> get_all_tools() const;

    // 按工具名查找并调用，返回 nullopt 表示工具不存在
    std::optional<mcp::ToolCallResult> call_tool(
        const std::string& tool_name,
        const nlohmann::json& arguments);

    size_t size() const { return m_registry.size(); }

private:
    std::unordered_map<std::string, PluginEntry> m_registry;
};

} // namespace plugin
