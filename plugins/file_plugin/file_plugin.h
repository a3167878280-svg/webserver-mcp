#pragma once

#include "plugin/plugin_interface.h"
#include <string>

/**
 * 文件插件 — 同时提供 Tool 和 Resource 两种访问方式
 *
 * Tools (动词 — LLM 主动调用):
 *   file_read(path)  — LLM 决定读某个文件
 *   file_list(dir)   — LLM 决定列某个目录
 *
 * Resources (名词 — 服务器被动暴露，客户端浏览发现):
 *   config://server  — 服务器配置文件 (预注册，无需知道路径)
 *   未来可扩展: file:///var/log/..., file:///etc/...
 */
class FilePlugin : public plugin::IPlugin {
public:
    const char* name() const override    { return "file_plugin"; }
    const char* version() const override { return "1.0.0"; }

    // ── Tool 接口 ──
    std::vector<mcp::ToolDef> get_tools() const override;
    mcp::ToolCallResult call_tool(
        const std::string& tool_name,
        const nlohmann::json& arguments) override;

    // ── Resource 接口 ──
    std::vector<mcp::ResourceDef> get_resources() const override;
    mcp::ResourceReadResult read_resource(const std::string& uri) override;

private:
    mcp::ToolCallResult handle_file_read(const nlohmann::json& args);
    mcp::ToolCallResult handle_file_list(const nlohmann::json& args);

    // 资源读取的辅助函数
    mcp::ResourceReadResult read_file_resource(const std::string& path,
                                                const std::string& mimeType = "text/plain");
};
