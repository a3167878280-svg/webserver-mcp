/**
 * JSON-RPC 序列化器 — 数据管道第 3 段 (最后一段)
 *
 * 职责: 把 JsonRpcResponse 结构体变成可通过 stdin/stdout/HTTP 传输的 JSON 字符串
 *
 * 输入: JsonRpcResponse { result={tools:[...]}, id=1 }
 * 输出: '{"jsonrpc":"2.0","id":1,"result":{"tools":[...]}}'
 *
 * 实现极其简单: 调 to_json() 构建 nlohmann::json 对象，再 dump() 成字符串
 */

#include "jsonrpc_serializer.h"

namespace mcp {

std::string JsonRpcSerializer::serialize(const JsonRpcResponse& response) {
    // to_json() 把 C++ 结构体转成 nlohmann::json 对象
    // dump()  把 nlohmann::json 对象转成紧凑的 JSON 字符串 (无缩进)
    return response.to_json().dump();
}

} // namespace mcp
