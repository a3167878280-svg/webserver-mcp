#pragma once
#include "plugin/plugin_interface.h"
#include <string>
#include <vector>

class CommandPlugin : public plugin::IPlugin {
public:
    const char* name() const override { return "command_plugin"; }
    const char* version() const override { return "1.0.0"; }
    std::vector<mcp::ToolDef> get_tools() const override;
    mcp::ToolCallResult call_tool(const std::string& name, const nlohmann::json& args) override;

private:
    mcp::ToolCallResult handle_shell_exec(const nlohmann::json& args);
    mcp::ToolCallResult handle_shell_exec_bg(const nlohmann::json& args);
};
