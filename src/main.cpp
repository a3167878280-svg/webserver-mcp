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
#include "server/conversation_manager.h"
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

/**
 * 程序启动流程 (以 stdio 模式为例):
 *
 *  1. 加载 config.json
 *  2. 初始化日志系统
 *  3. 加载 .so 插件 → 每个插件向 PluginRegistry 注册自己的工具
 *  4. 创建 McpHandler (方法路由器)
 *  5. 定义 on_message 回调 — 这是整个请求处理的核心管道:
 *     原始JSON → parse → route → 业务处理 → serialize → 发送
 *  6. 启动 StdioTransport — 阻塞在 read_loop() 等待 stdin 数据
 *
 *  HTTP+SSE 模式 (config.mode == "http"):
 *  同样的 on_message 回调，只是传输层换成了 HTTP Server。
 *  客户端 POST /message 发 JSON-RPC，响应通过 /sse 长连接流式返回。
 */
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

    // 3. 注册退出信号 (Ctrl+C → 优雅停止)
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 4. 从 plugin_dir 加载所有 .so 插件
    //    加载时每个插件调用 PluginRegistry::register_plugin()
    //    把自己的工具 (name + description + inputSchema) 注册进去
    plugin::PluginManager plugin_mgr;
    int loaded = plugin_mgr.load_all(config.plugin_dir);
    LOG_INFO("Loaded %d plugin(s) from %s", loaded, config.plugin_dir.c_str());

    // 4.5 创建对话管理器 (HTTP 模式下的多轮对话支持)
    //     注入 Log 系统，对话持久化通过 Log::persist() 异步落盘
    server::ConversationManager conv_mgr(Log::get_instance());

    // 5. 创建 MCP 方法路由器，注入 PluginRegistry
    //    McpHandler 内部维护一个 method → handler 映射表:
    //      "initialize"   → handle_initialize
    //      "ping"         → handle_ping
    //      "tools/list"   → handle_tools_list
    //      "tools/call"   → handle_tools_call
    //      "notifications/initialized" → handle_notifications_initialized
    mcp::McpHandler mcp_handler(plugin_mgr.registry());

    // 6. 消息处理回调 — 这是数据管道的核心，两种 transport 共用
    //
    //    管道: raw_json → parse → struct → route+handle → struct → serialize → send
    //
    //    每个请求必经这 3 步:
    //      A) JsonRpcParser::parse()   — JSON 字符串 → JsonRpcRequest 结构体
    //      B) McpHandler::handle()     — 根据 method 路由到具体处理函数
    //      C) JsonRpcSerializer::serialize() — JsonRpcResponse 结构体 → JSON 字符串
    //
    //    注意: 通知 (id=null) 不产生响应，McpHandler::handle() 返回 nullopt
    auto on_message = [&](transport::Transport& t, const std::string& raw_json) {
        // === 管道第 1 段: JSON 字符串 → 结构体 ===
        auto maybe_req = mcp::JsonRpcParser::parse(raw_json);
        if (!maybe_req.has_value()) {
            // JSON 格式错误 → 返回 PARSE_ERROR (-32700)
            auto err = mcp::JsonRpcParser::make_parse_error(nlohmann::json(nullptr));
            t.send(mcp::JsonRpcSerializer::serialize(err));
            return;
        }

        // === 管道第 2 段: 方法路由 + 业务处理 ===
        auto maybe_resp = mcp_handler.handle(maybe_req.value());
        if (maybe_resp.has_value()) {
            // === 管道第 3 段: 结构体 → JSON 字符串 → 发送 ===
            t.send(mcp::JsonRpcSerializer::serialize(maybe_resp.value()));
        }
        // 如果 maybe_resp 为空 → 这是一个通知 (notification)，不回复
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
        chat_cfg.conv_manager = &conv_mgr;
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
