#pragma once

#include "plugin/plugin_interface.h"

class TestPlugin : public plugin::IPlugin {
public:
    const char* name() const override { return "test_plugin"; }
    const char* version() const override { return "1.0"; }

    std::vector<mcp::ToolDef> get_tools() const override {
        return {{"echo", "Return the supplied value", {
            {"type", "object"},
            {"properties", {{"value", {{"type", "string"}}}}}
        }}};
    }

    mcp::ToolCallResult call_tool(const std::string& tool_name,
                                  const nlohmann::json& arguments) override {
        if (tool_name != "echo") {
            return {{{"text", "Unknown tool"}}, true};
        }
        return {{{"text", arguments.value("value", "")}}, false};
    }

    std::vector<mcp::ResourceDef> get_resources() const override {
        return {{"test://resource", "Test resource", "Fixture resource", "text/plain"}};
    }

    mcp::ResourceReadResult read_resource(const std::string& uri) override {
        return {{{uri, "text/plain", "resource body"}}};
    }

    std::vector<mcp::PromptDef> get_prompts() const override {
        return {{"test_prompt", "Fixture prompt", {{"topic", "Topic", true}}}};
    }

    mcp::PromptGetResult get_prompt(const std::string& name,
                                    const nlohmann::json& arguments) override {
        return {"Fixture prompt", {{"user", {"text", "Topic: " + arguments.value("topic", "")}}}};
    }
};
