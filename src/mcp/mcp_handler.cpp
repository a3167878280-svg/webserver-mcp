#include "mcp_handler.h"
#include "jsonrpc_parser.h"

namespace mcp {

McpHandler::McpHandler() {
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
    nlohmann::json j;
    j["tools"] = nlohmann::json::array();  // V1: 空列表，插件在 V2 加入
    return j;
}

nlohmann::json McpHandler::handle_notifications_initialized(const nlohmann::json& /*params*/) {
    return nlohmann::json::object();
}

} // namespace mcp
