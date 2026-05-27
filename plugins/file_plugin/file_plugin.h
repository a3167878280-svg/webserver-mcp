#pragma once

#include "plugin/plugin_interface.h"
#include <string>

class FilePlugin : public plugin::IPlugin {
public:
    const char* name() const override    { return "file_plugin"; }
    const char* version() const override { return "1.0.0"; }

    std::vector<mcp::ToolDef> get_tools() const override;
    mcp::ToolCallResult call_tool(
        const std::string& tool_name,
        const nlohmann::json& arguments) override;

private:
    mcp::ToolCallResult handle_file_read(const nlohmann::json& args);
    mcp::ToolCallResult handle_file_list(const nlohmann::json& args);
};
