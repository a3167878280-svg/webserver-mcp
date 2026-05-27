#include "jsonrpc_serializer.h"

namespace mcp {

std::string JsonRpcSerializer::serialize(const JsonRpcResponse& response) {
    return response.to_json().dump();
}

} // namespace mcp
