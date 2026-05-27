#pragma once

#include "plugin/plugin_interface.h"
#include <string>

class BilibiliPlugin : public plugin::IPlugin {
public:
    const char* name() const override    { return "bilibili_plugin"; }
    const char* version() const override { return "1.0.0"; }

    std::vector<mcp::ToolDef> get_tools() const override;
    mcp::ToolCallResult call_tool(
        const std::string& tool_name,
        const nlohmann::json& arguments) override;

private:
    mcp::ToolCallResult handle_up_videos(const nlohmann::json& args);
    mcp::ToolCallResult handle_up_info(const nlohmann::json& args);
    mcp::ToolCallResult handle_hot(const nlohmann::json& args);
    static std::string http_get(const std::string& url);
};
