#pragma once

#include "jsonrpc.h"
#include <string>

namespace mcp {

class JsonRpcSerializer {
public:
    // 序列化响应为紧凑 JSON 字符串
    static std::string serialize(const JsonRpcResponse& response);
};

} // namespace mcp
