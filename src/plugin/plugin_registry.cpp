/**
 * 插件注册表 — 插件系统的第 3 层 (工具索引)
 *
 * 这是 McpHandler 和 PluginManager 之间的桥梁:
 *   - PluginManager 加载插件后，调用 register_plugin() 把工具注册进来
 *   - McpHandler 处理 tools/list 和 tools/call 时，调 get_all_tools() / call_tool()
 *
 * 内部用 unordered_map<工具名, PluginEntry> 实现 O(1) 查找:
 *   工具名 → { IPlugin* 指针, ToolDef 元信息 }
 *
 * 为什么需要这个注册表?
 *   PluginManager 只知道"哪个 .so 文件加载了哪些插件"，
 *   McpHandler 只知道"有方法名要路由"，
 *   注册表把"工具名"和"插件实例"关联起来，让 tools/call 能快速定位到执行者。
 */

#include "plugin_registry.h"
#include "../log/log.h"
#include "../common.h"

namespace plugin {

/**
 * 注册一个插件的所有工具
 *
 * 一个插件可以提供多个工具 (如 B站插件提供 3 个)，
 * 每个工具以名称作为 key 存入 m_registry。
 * 同名工具会被跳过 (first-come-first-serve)。
 */
void PluginRegistry::register_plugin(IPlugin* plugin) {
    // ── 注册工具 ──
    auto tools = plugin->get_tools();
    for (auto& tool : tools) {
        if (m_tools.find(tool.name) != m_tools.end()) {
            LOG_WARN("Tool name collision: '%s' already registered, skipping",
                     tool.name.c_str());
            continue;
        }
        std::string key = tool.name;
        LOG_INFO("Registered tool: %s (from %s)", key.c_str(), plugin->name());
        m_tools[std::move(key)] = PluginEntry{plugin, tool};
    }

    // ── 注册资源 ──
    auto resources = plugin->get_resources();
    for (auto& res : resources) {
        if (m_resources.find(res.uri) != m_resources.end()) {
            LOG_WARN("Resource URI collision: '%s' already registered, skipping",
                     res.uri.c_str());
            continue;
        }
        LOG_INFO("Registered resource: %s (from %s)", res.uri.c_str(), plugin->name());
        m_resources[res.uri] = {plugin, res};
    }
}

void PluginRegistry::unregister_plugin(IPlugin* plugin) {
    // 清理工具
    auto tit = m_tools.begin();
    while (tit != m_tools.end()) {
        if (tit->second.plugin == plugin)
            tit = m_tools.erase(tit);
        else
            ++tit;
    }
    // 清理资源
    auto rit = m_resources.begin();
    while (rit != m_resources.end()) {
        if (rit->second.first == plugin)
            rit = m_resources.erase(rit);
        else
            ++rit;
    }
}

std::vector<mcp::ToolDef> PluginRegistry::get_all_tools() const {
    std::vector<mcp::ToolDef> tools;
    tools.reserve(m_tools.size());
    for (const auto& kv : m_tools) {
        tools.push_back(kv.second.tool_def);
    }
    return tools;
}

std::optional<mcp::ToolCallResult> PluginRegistry::call_tool(
    const std::string& tool_name, const nlohmann::json& arguments) {
    auto it = m_tools.find(tool_name);
    if (it == m_tools.end()) {
        return std::nullopt;
    }
    return it->second.plugin->call_tool(tool_name, arguments);
}

// ── 资源操作 ──

std::vector<mcp::ResourceDef> PluginRegistry::get_all_resources() const {
    std::vector<mcp::ResourceDef> resources;
    resources.reserve(m_resources.size());
    for (const auto& kv : m_resources) {
        resources.push_back(kv.second.second);
    }
    return resources;
}

std::optional<mcp::ResourceReadResult> PluginRegistry::read_resource(const std::string& uri) {
    auto it = m_resources.find(uri);
    if (it == m_resources.end()) {
        return std::nullopt;  // 资源不存在
    }
    return it->second.first->read_resource(uri);
}

} // namespace plugin
