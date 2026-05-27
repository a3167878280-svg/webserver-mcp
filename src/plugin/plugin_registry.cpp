#include "plugin_registry.h"
#include "../log/log.h"
#include "../common.h"

namespace plugin {

void PluginRegistry::register_plugin(IPlugin* plugin) {
    auto tools = plugin->get_tools();
    for (auto& tool : tools) {
        if (m_registry.find(tool.name) != m_registry.end()) {
            LOG_WARN("Tool name collision: '%s' already registered, skipping",
                     tool.name.c_str());
            continue;
        }
        std::string key = tool.name;
        LOG_INFO("Registered tool: %s (from %s)", key.c_str(), plugin->name());
        m_registry[std::move(key)] = PluginEntry{plugin, tool};
    }
}

void PluginRegistry::unregister_plugin(IPlugin* plugin) {
    auto it = m_registry.begin();
    while (it != m_registry.end()) {
        if (it->second.plugin == plugin) {
            it = m_registry.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<mcp::ToolDef> PluginRegistry::get_all_tools() const {
    std::vector<mcp::ToolDef> tools;
    tools.reserve(m_registry.size());
    for (const auto& kv : m_registry) {
        tools.push_back(kv.second.tool_def);
    }
    return tools;
}

std::optional<mcp::ToolCallResult> PluginRegistry::call_tool(
    const std::string& tool_name, const nlohmann::json& arguments) {
    auto it = m_registry.find(tool_name);
    if (it == m_registry.end()) {
        return std::nullopt;
    }
    return it->second.plugin->call_tool(tool_name, arguments);
}

} // namespace plugin
