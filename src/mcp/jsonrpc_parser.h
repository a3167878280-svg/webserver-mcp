#pragma once

#include "jsonrpc.h"
#include <optional>
#include <string>

namespace mcp {

class JsonRpcParser {
public:
    // 解析原始 JSON 字符串为 JsonRpcRequest
    // 返回 nullopt 表示 JSON 格式错误 (调用方应发送 PARSE_ERROR)
    // 返回的 request 若缺少必填字段 (jsonrpc/method) 也返回 nullopt
    static std::optional<JsonRpcRequest> parse(const std::string& raw_json);

    // 预定义错误响应工厂方法
    static JsonRpcResponse make_parse_error(const nlohmann::json& id);
    static JsonRpcResponse make_invalid_request(const nlohmann::json& id,
                                                const std::string& message);
    static JsonRpcResponse make_method_not_found(const nlohmann::json& id,
                                                 const std::string& method);
    static JsonRpcResponse make_invalid_params(const nlohmann::json& id,
                                               const std::string& message);
    static JsonRpcResponse make_internal_error(const nlohmann::json& id,
                                               const std::string& message);
};

} // namespace mcp
