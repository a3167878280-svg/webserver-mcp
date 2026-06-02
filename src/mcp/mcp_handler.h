#pragma once

#include "jsonrpc.h"
#include "mcp_types.h"
#include <functional>
#include <unordered_map>
#include <string>
#include <optional>

namespace plugin { class PluginRegistry; }

namespace mcp {

class McpHandler {
public:
    McpHandler();
    explicit McpHandler(plugin::PluginRegistry& registry);

    // 处理 JSON-RPC 请求，返回响应（请求）或 nullopt（通知）
    std::optional<JsonRpcResponse> handle(const JsonRpcRequest& request);

private:
    using HandlerFn = std::function<nlohmann::json(const nlohmann::json& params)>;
    std::unordered_map<std::string, HandlerFn> m_routes;
    plugin::PluginRegistry* m_registry = nullptr;

    void register_handlers();

    nlohmann::json handle_initialize(const nlohmann::json& params);
    nlohmann::json handle_ping(const nlohmann::json& params);
    nlohmann::json handle_tools_list(const nlohmann::json& params);
    nlohmann::json handle_tools_call(const nlohmann::json& params);
    nlohmann::json handle_resources_list(const nlohmann::json& params);
    nlohmann::json handle_resources_read(const nlohmann::json& params);
    nlohmann::json handle_prompts_list(const nlohmann::json& params);
    nlohmann::json handle_prompts_get(const nlohmann::json& params);
    nlohmann::json handle_notifications_initialized(const nlohmann::json& params);
};

} // namespace mcp
