/**
 * HTTP+SSE 传输层 — 用双通道 HTTP 替代 stdin/stdout
 *
 * ═══════════ 为什么需要双通道？ ═══════════
 *
 * stdio 模式读写同一条管道，天然双向。但 HTTP 是"请求→响应"的单向模式，
 * 服务器不能主动推送。MCP 又要求异步通知能力，所以拆成两条 HTTP 通路:
 *
 *   通道 1: GET  /sse      → 服务器 → 客户端 (SSE 长连接，单向推送)
 *   通道 2: POST /message  → 客户端 → 服务器 (普通 POST，发送 JSON-RPC 请求)
 *
 * 类比: SSE 是一根"水管"，服务器随时往里面灌数据；
 *       POST 是"快递员"，客户端每发一个请求就是寄一次包裹。
 *
 * ═══════════ Session 机制 ═══════════
 *
 * 一个浏览器标签页 = 一个 session (UUID 标识)
 * 同一个 session 的 GET /sse 和 POST /message 通过 session_id 关联:
 *   GET  /sse?session_id=abc123  ← 打开水管
 *   POST /message?session_id=abc123 ← 往水管对应的队列里扔响应
 *
 * Session 内部有一个 pending 队列 (std::queue<std::string>) + 条件变量:
 *   send() → push 到队列 → notify 唤醒 SSE 线程
 *   SSE 线程 → wait 等待 → pop 出队 → 通过 SSE 推送给浏览器
 */

#pragma once

#include "transport.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <unordered_map>
#include <memory>
#include <string>

namespace mcp { class McpHandler; }
namespace server { class ConversationManager; }

namespace transport {

// HTTP 模式下的额外配置: LLM 连接参数 + MCP handler + 对话管理器
struct ChatConfig {
    std::string llm_base_url;
    std::string llm_model;
    mcp::McpHandler* mcp_handler = nullptr;
    server::ConversationManager* conv_manager = nullptr;
};

class HttpSseTransport : public Transport {
public:
    HttpSseTransport();
    ~HttpSseTransport() override;

    void start() override;
    void start(int port);
    void stop() override;

    /**
     * HTTP 模式下的 send() — 与 stdio 完全不同
     *
     * stdio:   直接 fwrite(stdout) + fflush
     * HTTP+SSE: 把 JSON 字符串 push 到 session 的 pending 队列，
     *          然后 notify 唤醒 SSE 线程，SSE 线程异步地把数据推送给浏览器
     *
     * 这意味着 send() 不会阻塞等待客户端接收 — 它是异步的
     */
    void send(const std::string& json_message) override;
    void set_on_message(MessageCallback callback) override;

    void set_chat_config(const ChatConfig& cfg) { m_chat_config = cfg; }

    int port() const { return m_port; }

private:
    /**
     * SSE 会话 — 每个浏览器标签页对应一个 Session
     *
     * pending: 待发送的 JSON-RPC 响应队列
     * cv:      条件变量 — SSE 线程在此等待，send() 调用 notify_one() 唤醒
     */
    struct Session {
        std::string id;
        std::queue<std::string> pending;  // JSON-RPC 响应字符串队列
        std::mutex mtx;
        std::condition_variable cv;
    };

    std::string new_session_id();
    std::shared_ptr<Session> get_session(const std::string& id);

    MessageCallback m_callback;
    int m_port = 9006;
    std::thread m_server_thread;
    std::atomic<bool> m_running{false};

    // 所有活跃 session (key = UUID)
    std::mutex m_sessions_mtx;
    std::unordered_map<std::string, std::shared_ptr<Session>> m_sessions;

    /**
     * 线程局部 session ID — 每个 POST /message 请求在进入 on_message 回调前
     * 设置此值，这样 send() 调用时知道该把响应放到哪个 session 的队列
     *
     * 注意: httplib 是多线程处理请求的，所以用 mutex 保护
     */
    std::mutex m_current_id_mtx;
    std::string m_current_session_id;
    void set_current_session(const std::string& id);
    std::string current_session();

    // 聊天功能配置 (LLM 调用)
    ChatConfig m_chat_config;
    void setup_chat_routes();
    void handle_chat_request(const std::string& body, std::function<void(const std::string&, const std::string&)> sse_send);
};

} // namespace transport
