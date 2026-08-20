#include "mcp/jsonrpc_parser.h"
#include "mcp/jsonrpc_serializer.h"
#include "mcp/mcp_handler.h"
#include "plugin/plugin_registry.h"
#include "mcp_test_plugin.h"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>

int m_close_log = 1;

int main(int argc, char* argv[]) {
    const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 200000;
    TestPlugin plugin;
    plugin::PluginRegistry registry;
    registry.register_plugin(&plugin);
    mcp::McpHandler handler(registry);
    const std::string raw = R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"value":"benchmark"}}})";

    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto request = mcp::JsonRpcParser::parse(raw);
        if (!request) return EXIT_FAILURE;
        auto response = handler.handle(*request);
        if (!response) return EXIT_FAILURE;
        volatile auto bytes = mcp::JsonRpcSerializer::serialize(*response).size();
        (void)bytes;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "mcp_core_pipeline iterations=" << iterations
              << " seconds=" << std::fixed << std::setprecision(3) << seconds
              << " qps=" << std::setprecision(0) << iterations / seconds << '\n';
    return EXIT_SUCCESS;
}
