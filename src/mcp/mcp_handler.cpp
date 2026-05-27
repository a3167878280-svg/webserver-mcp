#include "mcp_handler.h"
#include "jsonrpc_parser.h"
#include "../plugin/plugin_registry.h"

namespace mcp {

McpHandler::McpHandler() {
    register_handlers();
}

McpHandler::McpHandler(plugin::PluginRegistry& registry)
    : m_registry(&registry) {
    register_handlers();
}

void McpHandler::register_handlers() {
    m_routes["initialize"] = [this](const nlohmann::json& p) {
        return handle_initialize(p);
    };
    m_routes["ping"] = [this](const nlohmann::json& p) {
        return handle_ping(p);
    };
    m_routes["tools/list"] = [this](const nlohmann::json& p) {
        return handle_tools_list(p);
    };
    m_routes["tools/call"] = [this](const nlohmann::json& p) {
        return handle_tools_call(p);
    };
    m_routes["notifications/initialized"] = [this](const nlohmann::json& p) {
        return handle_notifications_initialized(p);
    };
}

std::optional<JsonRpcResponse> McpHandler::handle(const JsonRpcRequest& request) {
    auto it = m_routes.find(request.method);
    if (it == m_routes.end()) {
        // 方法不存在
        auto err = JsonRpcParser::make_method_not_found(request.id, request.method);
        return err;
    }

    nlohmann::json result;
    try {
        result = it->second(request.params);
    } catch (const std::exception& e) {
        auto err = JsonRpcParser::make_internal_error(request.id, e.what());
        return err;
    }

    // 通知不返回响应
    if (request.is_notification()) {
        return std::nullopt;
    }

    // 成功响应
    JsonRpcResponse resp;
    resp.id = request.id;
    resp.result = std::move(result);
    return resp;
}

nlohmann::json McpHandler::handle_initialize(const nlohmann::json& params) {
    InitializeRequestParams req = InitializeRequestParams::from_json(params);

    InitializeResult result;
    result.protocolVersion = "2024-11-05";
    result.serverInfo = {
        {"name", "TinyWebServer-MCP"},
        {"version", "0.1.0"}
    };
    // V1: 只声明 tools 能力，后续版本扩展 prompts/resources
    result.capabilities.tools.listChanged = false;

    return result.to_json();
}

nlohmann::json McpHandler::handle_ping(const nlohmann::json& /*params*/) {
    return nlohmann::json::object();
}

nlohmann::json McpHandler::handle_tools_list(const nlohmann::json& /*params*/) {
    nlohmann::json tools_arr = nlohmann::json::array();
    if (m_registry) {
        auto all = m_registry->get_all_tools();
        for (auto& tool : all) {
            tools_arr.push_back(tool.to_json());
        }
    }
    nlohmann::json j;
    j["tools"] = std::move(tools_arr);
    return j;
}

nlohmann::json McpHandler::handle_tools_call(const nlohmann::json& params) {
    ToolCallParams p = ToolCallParams::from_json(params);

    if (!m_registry) {
        throw std::runtime_error("No plugin registry configured");
    }

    auto result = m_registry->call_tool(p.name, p.arguments);
    if (!result.has_value()) {
        throw std::runtime_error("Unknown tool: " + p.name);
    }
    return result->to_json();
}

nlohmann::json McpHandler::handle_notifications_initialized(const nlohmann::json& /*params*/) {
    return nlohmann::json::object();
}

} // namespace mcp
