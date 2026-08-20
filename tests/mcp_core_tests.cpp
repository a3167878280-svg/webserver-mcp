#include "mcp/jsonrpc_parser.h"
#include "mcp/jsonrpc_serializer.h"
#include "mcp/mcp_handler.h"
#include "plugin/plugin_registry.h"
#include "mcp_test_plugin.h"

#include <cstdlib>
#include <iostream>

int m_close_log = 1;

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

mcp::JsonRpcRequest request(const std::string& method, nlohmann::json params,
                            nlohmann::json id = 1) {
    return {"2.0", method, std::move(params), std::move(id)};
}

void test_parser_and_serializer() {
    auto parsed = mcp::JsonRpcParser::parse(
        R"({"jsonrpc":"2.0","id":7,"method":"tools/list","params":{}})");
    expect(parsed.has_value(), "valid JSON-RPC request parses");
    expect(parsed && parsed->id == 7 && parsed->method == "tools/list",
           "parser preserves id and method");
    expect(!mcp::JsonRpcParser::parse("not json").has_value(), "invalid JSON is rejected");
    expect(!mcp::JsonRpcParser::parse(R"({"jsonrpc":"1.0","method":"ping"})").has_value(),
           "wrong protocol version is rejected");

    mcp::JsonRpcResponse response;
    response.id = 7;
    response.result = {{"ok", true}};
    const auto json = nlohmann::json::parse(mcp::JsonRpcSerializer::serialize(response));
    expect(json["jsonrpc"] == "2.0" && json["id"] == 7 && json["result"]["ok"] == true,
           "serializer produces a JSON-RPC success response");
}

void test_mcp_routes() {
    TestPlugin plugin;
    plugin::PluginRegistry registry;
    registry.register_plugin(&plugin);
    mcp::McpHandler handler(registry);

    auto list = handler.handle(request("tools/list", nlohmann::json::object()));
    expect(list && !list->is_error() && list->result["tools"].size() == 1,
           "tools/list returns registered tools");

    auto call = handler.handle(request("tools/call", {{"name", "echo"}, {"arguments", {{"value", "hello"}}}}));
    expect(call && !call->is_error() && call->result["content"][0]["text"] == "hello",
           "tools/call dispatches to the owning plugin");

    auto resource = handler.handle(request("resources/read", {{"uri", "test://resource"}}));
    expect(resource && resource->result["contents"][0]["text"] == "resource body",
           "resources/read dispatches to the owning plugin");

    auto prompt = handler.handle(request("prompts/get", {{"name", "test_prompt"}, {"arguments", {{"topic", "MCP"}}}}));
    expect(prompt && prompt->result["messages"][0]["content"]["text"] == "Topic: MCP",
           "prompts/get dispatches to the owning plugin");

    auto unknown = handler.handle(request("missing/method", nlohmann::json::object()));
    expect(unknown && unknown->error.code == mcp::ErrorCode::METHOD_NOT_FOUND,
           "unknown method returns the JSON-RPC method-not-found error");

    auto notification = handler.handle(request("notifications/initialized", nlohmann::json::object(), nullptr));
    expect(!notification.has_value(), "notifications do not create responses");
}
}

int main() {
    test_parser_and_serializer();
    test_mcp_routes();
    if (failures == 0) {
        std::cout << "All MCP core tests passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " test assertion(s) failed\n";
    return EXIT_FAILURE;
}
