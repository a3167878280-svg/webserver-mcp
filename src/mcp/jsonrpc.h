/**
 * JSON-RPC 2.0 数据结构定义
 *
 * JSON-RPC 是 MCP 协议底层使用的远程调用协议，定义了三种消息格式:
 *
 *   请求 (Request):
 *     {"jsonrpc":"2.0", "id":1, "method":"tools/list", "params":{...}}
 *      ↑ 必须有          ↑ 用来匹配响应  ↑ 要执行的方法    ↑ 方法参数
 *
 *   通知 (Notification): — 特殊的请求，没有 id，服务器不回复
 *     {"jsonrpc":"2.0", "method":"notifications/initialized"}
 *
 *   响应 (Response):
 *     成功: {"jsonrpc":"2.0", "id":1, "result":{...}}
 *     失败: {"jsonrpc":"2.0", "id":1, "error":{"code":-32601, "message":"Method not found"}}
 *      ↑ 必须回显请求的 id，让客户端能匹配
 */

#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace mcp {

// JSON-RPC 2.0 标准错误码 (规范 RFC 定义的保留范围: -32000 ~ -32099 为自定义)
namespace ErrorCode {
    constexpr int PARSE_ERROR      = -32700;  // JSON 解析失败
    constexpr int INVALID_REQUEST  = -32600;  // 请求体不是合法的 JSON-RPC
    constexpr int METHOD_NOT_FOUND = -32601;  // method 不在路由表中
    constexpr int INVALID_PARAMS   = -32602;  // 参数类型/数量不对
    constexpr int INTERNAL_ERROR   = -32603;  // 服务器内部异常
}

/**
 * JSON-RPC 错误对象
 * 例: {"code":-32601, "message":"Method not found: tools/bad"}
 */
struct JsonRpcError {
    int code = 0;
    std::string message;
    nlohmann::json data;  // 可选的附加调试信息

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

/**
 * JSON-RPC 请求 (从客户端收到的原始 JSON 解析而来)
 */
struct JsonRpcRequest {
    std::string jsonrpc;         // 必须为 "2.0"
    std::string method;          // 要调用的方法名，如 "tools/list"
    nlohmann::json params;       // 方法参数，可选，默认 null
    nlohmann::json id;           // 请求ID，可以是 string/number/null

    // id 为 null → Notification (通知)，不需要回复
    bool is_notification() const { return id.is_null(); }
};

/**
 * JSON-RPC 响应 (服务器处理完成后返回给客户端)
 */
struct JsonRpcResponse {
    std::string jsonrpc = "2.0";
    nlohmann::json result;       // 成功时的返回值
    JsonRpcError error;          // 失败时的错误对象 (code != 0 表示错误)
    nlohmann::json id;           // 回显请求的 id

    bool is_error() const { return error.code != 0; }

    // 序列化为 JSON: 成功返回 result，失败返回 error (二者互斥)
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
