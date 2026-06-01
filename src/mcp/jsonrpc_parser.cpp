/**
 * JSON-RPC 解析器 — 数据管道第 1 段
 *
 * 职责: 把原始 JSON 字符串转换成类型化的 JsonRpcRequest 结构体
 *
 * 输入示例: {"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}
 * 输出:     JsonRpcRequest { jsonrpc="2.0", method="tools/list", params={}, id=1 }
 *
 * 同时提供各种 JSON-RPC 标准错误的工厂方法 (-32700, -32600, -32601 等)
 */

#include "jsonrpc_parser.h"

namespace mcp {

/**
 * 解析原始 JSON → JsonRpcRequest
 * 返回 nullopt 表示: 不是合法的 JSON-RPC 2.0 请求
 *
 * 校验规则 (JSON-RPC 2.0 规范):
 *   - 必须是合法的 JSON
 *   - 必须是 JSON Object (不能是 array/string/number)
 *   - 必须有 "jsonrpc":"2.0"
 *   - 必须有 "method" 字符串字段
 *   - "params"  可选，默认 null
 *   - "id"      可选，默认 null (null = 通知，不需要回复)
 */
std::optional<JsonRpcRequest> JsonRpcParser::parse(const std::string& raw_json) {
    // 步骤 1: 解析原始 JSON 字符串
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(raw_json);
    } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;  // 不合法的 JSON
    }

    // 步骤 2: 必须是 JSON Object
    if (!j.is_object()) {
        return std::nullopt;
    }

    // 步骤 3: 校验 jsonrpc 版本字段 (必须精确为 "2.0")
    if (!j.contains("jsonrpc") || !j["jsonrpc"].is_string() || j["jsonrpc"] != "2.0") {
        return std::nullopt;
    }

    // 步骤 4: 校验 method 字段 (必须存在且为字符串)
    if (!j.contains("method") || !j["method"].is_string()) {
        return std::nullopt;
    }

    // 步骤 5: 填充结构体
    JsonRpcRequest req;
    req.jsonrpc = "2.0";
    req.method  = j["method"].get<std::string>();

    // params 可选，不传就是 null
    if (j.contains("params")) {
        req.params = j["params"];
    }

    // id 可选，为 null 时表示 Notification (通知，无需回复)
    // Notification 示例: {"jsonrpc":"2.0","method":"notifications/initialized"}
    if (j.contains("id")) {
        req.id = j["id"];
    }

    return req;
}

// ─── 以下为 JSON-RPC 标准错误的工厂方法 ───
// 每个方法构建一个完整的错误响应，调用方只需填 id 和具体 message

JsonRpcResponse JsonRpcParser::make_parse_error(const nlohmann::json& id) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.error.code = ErrorCode::PARSE_ERROR;    // -32700
    resp.error.message = "Parse error";
    return resp;
}

JsonRpcResponse JsonRpcParser::make_invalid_request(const nlohmann::json& id,
                                                     const std::string& message) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.error.code = ErrorCode::INVALID_REQUEST;  // -32600
    resp.error.message = message;
    return resp;
}

JsonRpcResponse JsonRpcParser::make_method_not_found(const nlohmann::json& id,
                                                      const std::string& method) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.error.code = ErrorCode::METHOD_NOT_FOUND; // -32601
    resp.error.message = "Method not found: " + method;
    return resp;
}

JsonRpcResponse JsonRpcParser::make_invalid_params(const nlohmann::json& id,
                                                    const std::string& message) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.error.code = ErrorCode::INVALID_PARAMS;   // -32602
    resp.error.message = message;
    return resp;
}

JsonRpcResponse JsonRpcParser::make_internal_error(const nlohmann::json& id,
                                                    const std::string& message) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.error.code = ErrorCode::INTERNAL_ERROR;   // -32603
    resp.error.message = message;
    return resp;
}

} // namespace mcp
