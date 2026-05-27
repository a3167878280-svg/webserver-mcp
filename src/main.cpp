#include "common.h"
#include "log/log.h"
#include "config.h"
#include "mcp/jsonrpc.h"
#include "mcp/jsonrpc_parser.h"
#include "mcp/jsonrpc_serializer.h"
#include "mcp/mcp_handler.h"
#include "transport/stdio_transport.h"
#include "transport/http_sse_transport.h"
#include "plugin/plugin_manager.h"
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

// 日志宏引用的全局变量
int m_close_log = 0;

static std::atomic<bool> g_running{true};

static void signal_handler(int /*sig*/) {
    g_running = false;
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
    LOG_INFO("MCP Server starting (V3, mode=%s)...", config.mode.c_str());

    // 3. 注册退出信号
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 4. 加载插件
    plugin::PluginManager plugin_mgr;
    int loaded = plugin_mgr.load_all(config.plugin_dir);
    LOG_INFO("Loaded %d plugin(s) from %s", loaded, config.plugin_dir.c_str());

    // 5. 创建 MCP 方法路由器 (注入插件注册表)
    mcp::McpHandler mcp_handler(plugin_mgr.registry());

    // 6. 消息处理回调 (两种 transport 共用)
    auto on_message = [&](transport::Transport& t, const std::string& raw_json) {
        auto maybe_req = mcp::JsonRpcParser::parse(raw_json);
        if (!maybe_req.has_value()) {
            auto err = mcp::JsonRpcParser::make_parse_error(nlohmann::json(nullptr));
            t.send(mcp::JsonRpcSerializer::serialize(err));
            return;
        }

        auto maybe_resp = mcp_handler.handle(maybe_req.value());
        if (maybe_resp.has_value()) {
            t.send(mcp::JsonRpcSerializer::serialize(maybe_resp.value()));
        }
    };

    // 7. 根据模式选择传输层
    if (config.mode == "http") {
        transport::HttpSseTransport transport;
        transport.set_on_message([&](const std::string& raw) {
            on_message(transport, raw);
        });

        // 配置聊天功能
        transport::ChatConfig chat_cfg;
        chat_cfg.llm_base_url = config.llm_base_url;
        chat_cfg.llm_model = config.llm_model;
        chat_cfg.mcp_handler = &mcp_handler;
        transport.set_chat_config(chat_cfg);

        transport.start(config.port);
        fprintf(stderr, "HTTP+SSE mode, listening on http://localhost:%d\n", transport.port());
        fprintf(stderr, "Chat UI: http://localhost:%d/chat.html\n", transport.port());
        LOG_INFO("HTTP+SSE mode, listening on http://localhost:%d", config.port);
        LOG_INFO("Chat available at http://localhost:%d/chat.html", config.port);

        // 主线程等待信号
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        transport.stop();
    } else {
        transport::StdioTransport transport;
        transport.set_on_message([&](const std::string& raw) {
            on_message(transport, raw);
        });
        transport.start();  // 阻塞直到 stdin 关闭
        transport.stop();
    }

    // 8. 清理
    LOG_INFO("MCP Server shutting down.");
    return 0;
}
