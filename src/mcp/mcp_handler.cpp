/**
 * MCP 方法路由器 — 数据管道第 2 段 (核心)
 *
 * 职责: 根据 JSON-RPC 请求的 method 字段，路由到对应的处理函数
 *
 * 支持的方法:
 *   initialize                  — 握手: 交换协议版本和能力声明
 *   ping                        — 心跳: 客户端检测服务器是否存活
 *   tools/list                  — 列举工具: 返回所有插件的工具名称+描述+参数schema
 *   tools/call                  — 执行工具: 找到对应插件，传入参数，返回执行结果
 *   resources/list              — 列举资源: 返回所有插件暴露的可用资源
 *   resources/read              — 读取资源: 按 URI 读取某个资源的内容
 *   notifications/initialized   — 通知: 客户端告知"初始化完成" (无需回复)
 *
 * 路由机制: method 字符串 → handler 函数指针 (注册在 m_routes 映射表中)
 */

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

/**
 * 注册路由表 — 把 5 个 MCP 方法名映射到 C++ 处理函数
 * 所有 handler 签名统一: nlohmann::json → nlohmann::json
 */
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
    m_routes["resources/list"] = [this](const nlohmann::json& p) {
        return handle_resources_list(p);
    };
    m_routes["resources/read"] = [this](const nlohmann::json& p) {
        return handle_resources_read(p);
    };
    m_routes["notifications/initialized"] = [this](const nlohmann::json& p) {
        return handle_notifications_initialized(p);
    };
}

/**
 * 请求入口 — 所有 JSON-RPC 请求的统一处理点
 *
 * 流程:
 *   1. 查路由表 m_routes，找到对应的 handler
 *   2. 调用 handler，传入 params
 *   3. 如果请求是 Notification (id=null) → 不返回响应
 *   4. 否则把 handler 返回的 json 包装成 JsonRpcResponse
 *
 * 返回 nullopt = Notification，调用方不应发送响应
 */
std::optional<JsonRpcResponse> McpHandler::handle(const JsonRpcRequest& request) {
    // 步骤 1: 查路由表
    auto it = m_routes.find(request.method);
    if (it == m_routes.end()) {
        // 方法不存在 → -32601 Method not found
        auto err = JsonRpcParser::make_method_not_found(request.id, request.method);
        return err;
    }

    // 步骤 2: 执行 handler
    nlohmann::json result;
    try {
        result = it->second(request.params);  // 调用 handle_xxx(params)
    } catch (const std::exception& e) {
        // handler 抛异常 → -32603 Internal error
        auto err = JsonRpcParser::make_internal_error(request.id, e.what());
        return err;
    }

    // 步骤 3: 通知不返回响应
    // Notification 示例: {"jsonrpc":"2.0","method":"notifications/initialized"}
    // 这种请求没有 id，客户端不期望收到回复
    if (request.is_notification()) {
        return std::nullopt;
    }

    // 步骤 4: 构造成功响应
    JsonRpcResponse resp;
    resp.id = request.id;          // 回显请求 id (客户端用来匹配请求和响应)
    resp.result = std::move(result);
    return resp;
}

/**
 * initialize — MCP 协议握手 (客户端连接后第一个请求)
 *
 * 客户端发送:
 *   {"jsonrpc":"2.0","id":1,"method":"initialize",
 *    "params":{"protocolVersion":"2024-11-05","clientInfo":{"name":"Claude Desktop","version":"1.0"}}}
 *
 * 服务器回复:
 *   {"jsonrpc":"2.0","id":1,"result":{
 *     "protocolVersion":"2024-11-05",
 *     "capabilities":{"tools":{"listChanged":false}},
 *     "serverInfo":{"name":"TinyWebServer-MCP","version":"0.1.0"}
 *   }}
 */
nlohmann::json McpHandler::handle_initialize(const nlohmann::json& params) {
    InitializeRequestParams req = InitializeRequestParams::from_json(params);

    InitializeResult result;
    result.protocolVersion = "2024-11-05";
    result.serverInfo = {
        {"name", "TinyWebServer-MCP"},
        {"version", "0.1.0"}
    };
    // 声明服务器能力: 只支持 tools，不支持 prompts/resources
    result.capabilities.tools.listChanged = false;
    result.capabilities.resources.subscribe = false;
    result.capabilities.resources.listChanged = false;

    return result.to_json();
}

/**
 * ping — 心跳检测
 * 请求: {"jsonrpc":"2.0","id":2,"method":"ping"}
 * 回复: {"jsonrpc":"2.0","id":2,"result":{}}
 */
nlohmann::json McpHandler::handle_ping(const nlohmann::json& /*params*/) {
    return nlohmann::json::object();  // 返回空 JSON 对象
}

/**
 * tools/list — 返回所有可用工具
 *
 * 遍历 PluginRegistry 中所有插件的所有工具，收集名称、描述、参数 schema
 *
 * 回复示例:
 *   {"tools":[
 *     {"name":"file_read",  "description":"Read file",        "inputSchema":{...}},
 *     {"name":"file_list",  "description":"List directory",   "inputSchema":{...}},
 *     {"name":"query_weather","description":"Query weather",  "inputSchema":{...}},
 *     ...
 *   ]}
 */
nlohmann::json McpHandler::handle_tools_list(const nlohmann::json& /*params*/) {
    nlohmann::json tools_arr = nlohmann::json::array();
    if (m_registry) {
        // 遍历所有已加载插件的所有注册工具
        auto all = m_registry->get_all_tools();
        for (auto& tool : all) {
            tools_arr.push_back(tool.to_json());  // 每个工具: name + description + inputSchema
        }
    }
    nlohmann::json j;
    j["tools"] = std::move(tools_arr);
    return j;
}

/**
 * tools/call — 执行一个工具调用
 *
 * 客户端发送:
 *   {"jsonrpc":"2.0","id":3,"method":"tools/call",
 *    "params":{"name":"query_weather","arguments":{"city":"Beijing"}}}
 *
 * 处理流程:
 *   1. 从 params 中提取 tool_name 和 arguments
 *   2. 在 PluginRegistry 中查找 tool_name → 找到对应的 IPlugin*
 *   3. 调用 plugin->call_tool(tool_name, arguments)
 *   4. 返回 ToolCallResult (content 数组 + isError 标记)
 */
nlohmann::json McpHandler::handle_tools_call(const nlohmann::json& params) {
    // 解析工具名称和参数
    ToolCallParams p = ToolCallParams::from_json(params);

    if (!m_registry) {
        throw std::runtime_error("No plugin registry configured");
    }

    // 在插件注册表中按名查找并执行
    auto result = m_registry->call_tool(p.name, p.arguments);
    if (!result.has_value()) {
        throw std::runtime_error("Unknown tool: " + p.name);
    }
    return result->to_json();
}

// ═══════════════════════════════════════════════════════════════
// Resources 方法
// ═══════════════════════════════════════════════════════════════

/**
 * resources/list — 返回所有可用资源
 *
 * 回复示例:
 *   {"resources":[
 *     {"uri":"file:///etc/hostname","name":"hostname","description":"系统主机名"},
 *     {"uri":"config://server","name":"服务器配置","description":"当前运行配置"},
 *     ...
 *   ]}
 */
nlohmann::json McpHandler::handle_resources_list(const nlohmann::json& /*params*/) {
    nlohmann::json arr = nlohmann::json::array();
    if (m_registry) {
        auto all = m_registry->get_all_resources();
        for (auto& r : all) {
            arr.push_back(r.to_json());
        }
    }
    return {{"resources", std::move(arr)}};
}

/**
 * resources/read — 按 URI 读取某个资源
 *
 * 客户端发送:
 *   {"jsonrpc":"2.0","id":4,"method":"resources/read",
 *    "params":{"uri":"file:///etc/hostname"}}
 *
 * 服务器回复:
 *   {"contents":[{"uri":"file:///etc/hostname","mimeType":"text/plain","text":"my-host"}]}
 */
nlohmann::json McpHandler::handle_resources_read(const nlohmann::json& params) {
    ResourceReadParams p = ResourceReadParams::from_json(params);

    if (!m_registry) {
        throw std::runtime_error("No plugin registry configured");
    }

    auto result = m_registry->read_resource(p.uri);
    if (!result.has_value()) {
        throw std::runtime_error("Unknown resource: " + p.uri);
    }
    return result->to_json();
}

/**
 * notifications/initialized — 客户端发出的"我准备好了"通知
 * 这是 Notification (无 id)，服务器收到后不回复
 */
nlohmann::json McpHandler::handle_notifications_initialized(const nlohmann::json& /*params*/) {
    return nlohmann::json::object();
}

} // namespace mcp
