#include "common.h"
#include "log/log.h"
#include "config.h"
#include "mcp/jsonrpc.h"
#include "mcp/jsonrpc_parser.h"
#include "mcp/jsonrpc_serializer.h"
#include "mcp/mcp_handler.h"
#include "transport/stdio_transport.h"
#include <csignal>

// 日志宏引用的全局变量
int m_close_log = 0;

static transport::StdioTransport* g_transport = nullptr;

static void signal_handler(int /*sig*/) {
    if (g_transport) {
        g_transport->stop();
    }
}

int main(int argc, char* argv[]) {
    // 1. 加载配置
    std::string config_path = "config.json";
    if (argc > 1) {
        config_path = argv[1];
    }
    AppConfig config = AppConfig::load_from_file(config_path);

    // 2. 初始化日志
    m_close_log = config.close_log;
    Log::get_instance()->init(config.log_file.c_str(), config.close_log,
                              config.log_buf_size, config.log_split_lines,
                              config.log_max_queue_size);
    LOG_INFO("MCP Server starting (V1)...");

    // 3. 注册退出信号
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 4. 创建 MCP 方法路由器
    mcp::McpHandler mcp_handler;

    // 5. 创建 stdio 传输层（线程池暂不使用，stdio 模式串行处理即可）
    transport::StdioTransport transport;
    g_transport = &transport;

    // 同步处理：收到消息 → 解析 → 路由 → 发送响应
    transport.set_on_message([&](const std::string& raw_json) {
        auto maybe_req = mcp::JsonRpcParser::parse(raw_json);
        if (!maybe_req.has_value()) {
            auto err = mcp::JsonRpcParser::make_parse_error(nlohmann::json(nullptr));
            transport.send(mcp::JsonRpcSerializer::serialize(err));
            return;
        }

        auto maybe_resp = mcp_handler.handle(maybe_req.value());
        if (maybe_resp.has_value()) {
            transport.send(mcp::JsonRpcSerializer::serialize(maybe_resp.value()));
        }
    });

    // 6. 启动传输层 (阻塞直到 stdin 关闭)
    transport.start();

    // 7. 清理
    LOG_INFO("MCP Server shutting down.");
    transport.stop();

    return 0;
}
