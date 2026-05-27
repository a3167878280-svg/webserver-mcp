#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace mcp {

// JSON-RPC 2.0 预定义错误码
namespace ErrorCode {
    constexpr int PARSE_ERROR      = -32700;
    constexpr int INVALID_REQUEST  = -32600;
    constexpr int METHOD_NOT_FOUND = -32601;
    constexpr int INVALID_PARAMS   = -32602;
    constexpr int INTERNAL_ERROR   = -32603;
}

// JSON-RPC 错误对象
struct JsonRpcError {
    int code = 0;
    std::string message;
    nlohmann::json data;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["code"] = code;
        j["message"] = message;
        if (!data.is_null()) {
            j["data"] = data;
        }
        return j;
    }
};

// JSON-RPC 请求
struct JsonRpcRequest {
    std::string jsonrpc;         // 必须为 "2.0"
    std::string method;
    nlohmann::json params;       // 可选，默认 null
    nlohmann::json id;           // string / number / null (null = 通知)

    bool is_notification() const { return id.is_null(); }
};

// JSON-RPC 响应
struct JsonRpcResponse {
    std::string jsonrpc = "2.0";
    nlohmann::json result;       // 成功时有效
    JsonRpcError error;          // error.code != 0 表示错误
    nlohmann::json id;           // 回显请求 id

    bool is_error() const { return error.code != 0; }

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["jsonrpc"] = jsonrpc;
        j["id"] = id;
        if (is_error()) {
            j["error"] = error.to_json();
        } else {
            j["result"] = result;
        }
        return j;
    }
};

} // namespace mcp
