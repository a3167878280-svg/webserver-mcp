#include "jsonrpc_parser.h"

namespace mcp {

std::optional<JsonRpcRequest> JsonRpcParser::parse(const std::string& raw_json) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(raw_json);
    } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;
    }

    // 必须是 object
    if (!j.is_object()) {
        return std::nullopt;
    }

    // 检查 jsonrpc 字段
    if (!j.contains("jsonrpc") || !j["jsonrpc"].is_string() || j["jsonrpc"] != "2.0") {
        return std::nullopt;
    }

    // 检查 method 字段
    if (!j.contains("method") || !j["method"].is_string()) {
        return std::nullopt;
    }

    JsonRpcRequest req;
    req.jsonrpc = "2.0";
    req.method  = j["method"].get<std::string>();

    // params 可选，默认 null
    if (j.contains("params")) {
        req.params = j["params"];
    }

    // id 可选，默认 null (通知无 id)
    if (j.contains("id")) {
        req.id = j["id"];
    }

    return req;
}

JsonRpcResponse JsonRpcParser::make_parse_error(const nlohmann::json& id) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.error.code = ErrorCode::PARSE_ERROR;
    resp.error.message = "Parse error";
    return resp;
}

JsonRpcResponse JsonRpcParser::make_invalid_request(const nlohmann::json& id,
                                                     const std::string& message) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.error.code = ErrorCode::INVALID_REQUEST;
    resp.error.message = message;
    return resp;
}

JsonRpcResponse JsonRpcParser::make_method_not_found(const nlohmann::json& id,
                                                      const std::string& method) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.error.code = ErrorCode::METHOD_NOT_FOUND;
    resp.error.message = "Method not found: " + method;
    return resp;
}

JsonRpcResponse JsonRpcParser::make_invalid_params(const nlohmann::json& id,
                                                    const std::string& message) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.error.code = ErrorCode::INVALID_PARAMS;
    resp.error.message = message;
    return resp;
}

JsonRpcResponse JsonRpcParser::make_internal_error(const nlohmann::json& id,
                                                    const std::string& message) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.error.code = ErrorCode::INTERNAL_ERROR;
    resp.error.message = message;
    return resp;
}

} // namespace mcp
