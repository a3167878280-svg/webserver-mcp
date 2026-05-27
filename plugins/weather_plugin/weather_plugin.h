#pragma once

#include "plugin/plugin_interface.h"
#include <string>

class WeatherPlugin : public plugin::IPlugin {
public:
    const char* name() const override    { return "weather_plugin"; }
    const char* version() const override { return "1.0.0"; }

    std::vector<mcp::ToolDef> get_tools() const override;
    mcp::ToolCallResult call_tool(
        const std::string& tool_name,
        const nlohmann::json& arguments) override;

private:
    mcp::ToolCallResult handle_query_weather(const nlohmann::json& args);
    static std::string http_get(const std::string& url);
};
