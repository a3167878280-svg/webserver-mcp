/**
 * HTTP+SSE 传输层实现
 *
 * ═══════════ 核心架构: 双通道异步模式 ═══════════
 *
 *   stdio 模式:  stdin ──→ 处理 ──→ stdout  (一条管道，同步)
 *   HTTP 模式:   POST /message ──→ 处理 ──→ SSE 推送  (两条通道，异步)
 *                       ↑                    ↑
 *                   客户端发请求          服务器推响应
 *
 * 为什么不能直接在 POST 的 response 里返回 JSON-RPC 结果？
 * 因为 SSE 模式下的 MCP 客户端期望响应通过事件流异步到达，
 * 而不是在 HTTP response body 里。这是 MCP 协议的 streamable HTTP 传输规范。
 *
 * ═══════════ 一个请求的完整路径 ═══════════
 *
 *   1. 浏览器 GET  /sse              → 建立 SSE 长连接，拿到 session_id
 *   2. 浏览器 POST /message?sid=xxx  → 发送 JSON-RPC 请求体
 *   3. 服务器 set_current_session()  → 记录"当前请求属于哪个 session"
 *   4. 服务器 m_callback(req.body)   → 走 on_message 管道 (parse→route→execute→serialize)
 *   5. 管道末端 t.send(json)         → push 到 session.pending 队列
 *   6. SSE 线程被 cv 唤醒            → 从队列 pop，sink.write() 推送给浏览器
 *   7. POST 返回 HTTP 202 Accepted   → 响应已接受 (不是实际结果!)
 */

#include "http_sse_transport.h"
#include "../log/log.h"
#include "../common.h"
#include "../mcp/mcp_handler.h"
#include "../mcp/jsonrpc.h"
#include "../llm/tool_orchestrator.h"
#include "../server/conversation_manager.h"

// 本机 OpenSSL 3.x 运行时库有此符号但 1.1.1 头文件未声明，手动声明
#include <openssl/ssl.h>
extern "C" X509* SSL_get1_peer_certificate(const SSL* ssl);

#include "httplib.h"
#include <random>
#include <sstream>
#include <fstream>

namespace transport {

// 生成 32 位十六进制随机 session ID (类似 UUID 但更轻量)
static std::string gen_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string uuid;
    for (int i = 0; i < 32; ++i) {
        uuid += hex[dis(gen)];
    }
    return uuid;
}

HttpSseTransport::HttpSseTransport() = default;

HttpSseTransport::~HttpSseTransport() {
    stop();
}

void HttpSseTransport::start() {
    start(m_port);
}

/**
 * 启动 HTTP 服务器 (在独立线程中运行)
 *
 * 注册的路由:
 *   GET  /sse               — SSE 长连接 (通道1: 服务器→客户端)
 *   POST /message            — JSON-RPC 请求入口 (通道2: 客户端→服务器)
 *   GET  /health             — 健康检查
 *   POST /api/chat           — 聊天接口 (LLM 对话，SSE 流式返回)
 *   GET  /api/models         — 模型列表
 *   GET  /api/tools          — 工具列表 (给前端插件面板用)
 *   GET  /api/conversations  — 对话列表
 *   POST /api/conversations  — 新建对话
 *   DELETE /api/conversations— 删除对话
 *   GET  /chat.html          — 聊天页面静态文件
 */
void HttpSseTransport::start(int port) {
    m_port = port;
    m_running = true;

    m_server_thread = std::thread([this]() {
        httplib::Server srv;

        // CORS 中间件 — 允许浏览器跨域访问 (前端通常跑在不同端口)
        srv.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            return httplib::Server::HandlerResponse::Unhandled;
        });

        // OPTIONS 预检 — 浏览器在跨域 POST 之前会先发 OPTIONS 问"可以吗"
        srv.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.status = 204;  // No Content
        });

        // 健康检查 — 负载均衡/监控用
        srv.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("OK", "text/plain");
        });

        // ═══════════════════════════════════════════════════════════════
        // 通道 1: GET /sse — SSE 长连接 (服务器 → 客户端)
        // ═══════════════════════════════════════════════════════════════
        //
        // 这是 MCP HTTP 模式的"出站通道"。浏览器先打开这个连接，
        // 服务器通过它持续推送 JSON-RPC 响应。
        //
        // SSE 协议格式 (纯文本):
        //   event: endpoint\ndata: /message?session_id=abc123\n\n
        //   event: message\ndata: {"jsonrpc":"2.0","id":1,"result":{...}}\n\n
        //
        // 每条消息以 \n\n 分隔，event: 是事件类型，data: 是数据体
        srv.Get("/sse", [this](const httplib::Request& req, httplib::Response& res) {
            std::string session_id;
            if (req.has_param("session_id")) {
                // 重连: 浏览器带了已有的 session_id
                session_id = req.get_param_value("session_id");
            } else {
                // 首次连接: 生成新 session_id
                session_id = new_session_id();
            }

            auto session = get_session(session_id);

            // SSE 必需的响应头
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");

            std::string endpoint_url = "/message?session_id=" + session_id;

            /**
             * Content Provider — httplib 的"长连接"机制
             *
             * 普通 HTTP handler 返回后就关闭连接了。Content Provider 则不同:
             * 函数可以无限期运行，不断调用 sink.write() 推送数据。
             * 返回 false 表示客户端断开，返回 true 表示正常结束。
             *
             * 这里实现了一个经典的 生产者-消费者 模式:
             *   - POST /message 是生产者 → send() 把响应 push 到 pending 队列
             *   - SSE Content Provider 是消费者 → 从 pending 队列 pop，推送给浏览器
             *   - cv (条件变量) 协调两者: 队列空时消费者 sleep，生产者 push 后唤醒
             */
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, session, endpoint_url](size_t /*offset*/, httplib::DataSink& sink) {
                    // 第一步: 先发送 endpoint 事件，告诉浏览器"往这个地址发请求"
                    // 浏览器收到: event: endpoint\ndata: /message?session_id=abc123\n\n
                    std::string ev = "event: endpoint\ndata: " + endpoint_url + "\n\n";
                    sink.write(ev.data(), ev.size());

                    // 第二步: 无限循环，等待并转发响应消息
                    while (m_running) {
                        std::unique_lock<std::mutex> lock(session->mtx);
                        // 等待队列有数据或服务器停止 (最多等 500ms，之后检查 m_running)
                        session->cv.wait_for(lock, std::chrono::milliseconds(500),
                            [&] { return !session->pending.empty() || !m_running; });

                        // 把队列中所有积压消息一次性发出
                        while (!session->pending.empty()) {
                            std::string msg = std::move(session->pending.front());
                            session->pending.pop();
                            lock.unlock();

                            // SSE 帧格式: event: message\ndata: <JSON-RPC 响应>\n\n
                            std::string framed = "event: message\ndata: " + msg + "\n\n";
                            if (!sink.write(framed.data(), framed.size())) {
                                return false;  // write 失败 → 客户端断开连接
                            }

                            lock.lock();
                        }

                        if (!m_running) break;
                    }
                    return true;
                });
        });

        // ═══════════════════════════════════════════════════════════════
        // 通道 2: POST /message — JSON-RPC 请求入口 (客户端 → 服务器)
        // ═══════════════════════════════════════════════════════════════
        //
        // 浏览器把 JSON-RPC 请求 POST 到这里。关键点:
        //   1. 不直接在 HTTP response 中返回 JSON-RPC 结果 — 返回 202 Accepted
        //   2. 结果通过 SSE 通道异步推送 (步骤见 send() 方法)
        //   3. 调用的是同一个 m_callback (即 main.cpp 的 on_message)
        srv.Post("/message", [this](const httplib::Request& req, httplib::Response& res) {
            std::string session_id;
            if (req.has_param("session_id")) {
                session_id = req.get_param_value("session_id");
            } else {
                res.status = 400;
                res.set_content("Missing session_id", "text/plain");
                return;
            }

            // 校验 session 存在 (SSE 连接必须先建立)
            {
                std::lock_guard<std::mutex> lk(m_sessions_mtx);
                if (m_sessions.find(session_id) == m_sessions.end()) {
                    res.status = 404;
                    res.set_content("Session not found", "text/plain");
                    return;
                }
            }

            /**
             * 设置当前 session — 这是整个双通道架构的关键!
             *
             * m_callback (on_message 管道) 最终会调用 t.send(json_response),
             * send() 需要知道"往哪个 session 的 SSE 推送"。
             * set_current_session() 就是告诉 send(): "这个请求的响应,
             * 推到 session_id 这个队列里"。
             *
             * httplib 是多线程处理请求的，所以用 thread_local 的思路 +
             * mutex 保护，避免并发请求互相干扰。
             */
            set_current_session(session_id);

            // 调用同一个 on_message 管道!
            // req.body = 原始 JSON-RPC 字符串，和 stdio 模式收到的完全一样
            // 管道内部: parse → route → execute → serialize → send()
            // 管道的 send() 会把响应 push 到 session 队列，SSE 线程负责推送
            if (m_callback) {
                m_callback(req.body);
            }

            // 返回 202 Accepted — "请求收到了，结果通过 SSE 给你"
            // 注意: 这里返回的不是 JSON-RPC 响应，只是一个确认
            res.set_content("Accepted", "text/plain");
            res.status = 202;
        });

        // ═══════════════════════════════════════════════════════════════
        // 聊天 API — LLM 对话 (也是 SSE 流式返回，但在同一个 HTTP 响应中)
        // ═══════════════════════════════════════════════════════════════
        //
        // 与 MCP /message 不同: /api/chat 的响应直接通过当前 HTTP 连接的
        // SSE Content Provider 返回，不需要 session 队列中转。
        //
        // 原因: 聊天请求-响应是一对一的，不需要 session 持久化；
        //       MCP 请求是多对一的 (多个 POST /message → 同一个 SSE 流)，
        //       所以需要队列。
        //
        // processed flag: httplib 的 content provider 会被多次调用
        // (每次 offset 递增)，我们只想处理一次。
        srv.Post("/api/chat", [this](const httplib::Request& req, httplib::Response& res) {
            auto processed = std::make_shared<bool>(false);
            res.set_chunked_content_provider("text/event-stream",
                [this, body = req.body, processed](size_t /*offset*/, httplib::DataSink& sink) {
                    if (*processed) return true;  // 防止重复执行
                    *processed = true;
                    handle_chat_request(body,
                        [&sink](const std::string& event, const std::string& data) {
                            std::string framed = "event: " + event + "\ndata: " + data + "\n\n";
                            return sink.write(framed.data(), framed.size());
                        });
                    sink.done();  // 告知 httplib 流结束
                    return true;
                });
        });

        // 模型列表
        srv.Get("/api/models", [this](const httplib::Request&, httplib::Response& res) {
            nlohmann::json j;
            j["models"] = nlohmann::json::array({
                {{"id", "gpt-4o"}, {"name", "GPT-4o (OpenAI)"}},
                {{"id", "gpt-4o-mini"}, {"name", "GPT-4o Mini (OpenAI)"}},
                {{"id", "gpt-4.1"}, {"name", "GPT-4.1 (OpenAI)"}},
                {{"id", "claude-opus-4-7"}, {"name", "Claude Opus 4.7 (Anthropic)"}},
                {{"id", "claude-sonnet-4-6"}, {"name", "Claude Sonnet 4.6 (Anthropic)"}},
                {{"id", "claude-haiku-4-5"}, {"name", "Claude Haiku 4.5 (Anthropic)"}},
                {{"id", "claude-3-5-sonnet-20241022"}, {"name", "Claude 3.5 Sonnet (Anthropic)"}},
                {{"id", "claude-3-opus-20240229"}, {"name", "Claude 3 Opus (Anthropic)"}},
                {{"id", "deepseek-v3"}, {"name", "DeepSeek V3"}},
                {{"id", "deepseek-r1"}, {"name", "DeepSeek R1"}},
                {{"id", "qwen2.5:7b"}, {"name", "Qwen 2.5 7B (Ollama)"}},
                {{"id", "qwen2.5:14b"}, {"name", "Qwen 2.5 14B (Ollama)"}},
                {{"id", "llama3:8b"}, {"name", "Llama 3 8B (Ollama)"}},
                {{"id", "llama3:70b"}, {"name", "Llama 3 70B (Ollama)"}},
            });
            res.set_content(j.dump(), "application/json");
        });

        // 工具列表 (供前端插件面板使用)
        srv.Get("/api/tools", [this](const httplib::Request&, httplib::Response& res) {
            nlohmann::json j;
            j["tools"] = nlohmann::json::array();
            if (m_chat_config.mcp_handler) {
                mcp::JsonRpcRequest req;
                req.jsonrpc = "2.0";
                req.method = "tools/list";
                req.id = "internal";
                auto resp = m_chat_config.mcp_handler->handle(req);
                if (resp.has_value() && resp->result.contains("tools")) {
                    j["tools"] = resp->result["tools"];
                }
            }
            res.set_content(j.dump(), "application/json");
        });

        // ── 对话管理 API ──
        srv.Get("/api/conversations", [this](const httplib::Request&, httplib::Response& res) {
            if (!m_chat_config.conv_manager) {
                res.status = 500; res.set_content("No conv manager", "text/plain"); return;
            }
            auto list = m_chat_config.conv_manager->list();
            nlohmann::json arr = nlohmann::json::array();
            for (auto& c : list) {
                arr.push_back({{"id", c.id}, {"title", c.title},
                               {"created_at", c.created_at}});
            }
            res.set_content(arr.dump(), "application/json");
        });

        srv.Post("/api/conversations", [this](const httplib::Request& req, httplib::Response& res) {
            if (!m_chat_config.conv_manager) return;
            std::string title = "新对话";
            try {
                auto j = nlohmann::json::parse(req.body);
                title = j.value("title", title);
            } catch(...) {}
            auto info = m_chat_config.conv_manager->create(title);
            nlohmann::json r;
            r["id"] = info.id; r["title"] = info.title; r["created_at"] = info.created_at;
            res.set_content(r.dump(), "application/json");
        });

        srv.Delete("/api/conversations", [this](const httplib::Request& req, httplib::Response& res) {
            if (!m_chat_config.conv_manager) return;
            std::string id;
            if (req.has_param("id")) id = req.get_param_value("id");
            if (id.empty()) { res.status = 400; return; }
            bool ok = m_chat_config.conv_manager->remove(id);
            res.set_content(ok ? "{\"ok\":true}" : "{\"ok\":false}", "application/json");
        });

        // 获取对话消息历史 (前端加载历史对话)
        srv.Get(R"(/api/conversations/([^/]+)/messages)", [this](const httplib::Request& req, httplib::Response& res) {
            if (!m_chat_config.conv_manager) { res.status = 500; return; }
            std::string id = req.matches[1];
            auto history = m_chat_config.conv_manager->get_history(id);
            nlohmann::json arr = nlohmann::json::array();
            for (auto& h : history) {
                nlohmann::json m;
                m["role"] = h.role;
                m["content"] = h.content;
                arr.push_back(m);
            }
            res.set_content(arr.dump(), "application/json");
        });

        // 导出对话为文本文件
        srv.Get(R"(/api/conversations/([^/]+)/export)", [this](const httplib::Request& req, httplib::Response& res) {
            if (!m_chat_config.conv_manager) { res.status = 500; return; }
            std::string id = req.matches[1];
            auto history = m_chat_config.conv_manager->get_history(id);
            std::ostringstream oss;
            oss << "=== Conversation History ===\n";
            oss << "ID: " << id << "\n\n";
            for (auto& h : history) {
                oss << "[" << h.role << "] " << h.content << "\n\n";
            }
            res.set_header("Content-Type", "text/plain; charset=utf-8");
            res.set_header("Content-Disposition", "attachment; filename=conversation_" + id + ".txt");
            res.set_content(oss.str(), "text/plain");
        });

        // 聊天页面
        srv.Get("/chat.html", [](const httplib::Request&, httplib::Response& res) {
            std::ifstream f("../chat/chat.html");
            if (f) {
                std::ostringstream oss;
                oss << f.rdbuf();
                res.set_content(oss.str(), "text/html");
            } else {
                // 尝试从 src 同级目录
                std::ifstream f2("chat/chat.html");
                if (f2) {
                    std::ostringstream oss;
                    oss << f2.rdbuf();
                    res.set_content(oss.str(), "text/html");
                } else {
                    res.status = 404;
                    res.set_content("chat.html not found", "text/plain");
                }
            }
        });

        LOG_INFO("HTTP+SSE transport listening on port %d", m_port);
        srv.listen("0.0.0.0", m_port);
        LOG_INFO("HTTP server stopped");
    });
}

void HttpSseTransport::stop() {
    if (!m_running) return;
    m_running = false;

    // 唤醒所有 SSE Content Provider 线程 (它们正在 cv.wait_for 中阻塞)
    {
        std::lock_guard<std::mutex> lk(m_sessions_mtx);
        for (auto& kv : m_sessions) {
            std::lock_guard<std::mutex> slk(kv.second->mtx);
            kv.second->cv.notify_all();
        }
    }

    // httplib::Server::listen 是阻塞的，无法从外部优雅停止
    // detach 让 OS 在线程结束时自动回收资源
    if (m_server_thread.joinable()) {
        m_server_thread.detach();
    }
    LOG_INFO("HttpSseTransport stopped");
}

/**
 * HTTP 模式下的 send() — 与 stdio 完全不同
 *
 * stdio 版本:
 *   void send(json) { fwrite(Content-Length + json, stdout); fflush(stdout); }
 *   直接写，同步，简单。
 *
 * HTTP+SSE 版本:
 *   void send(json) {
 *       session = get_session(current_session());  // 找到当前请求的 session
 *       session->pending.push(json);               // 塞进队列
 *       session->cv.notify_one();                  // 唤醒 SSE 线程
 *   }
 *   间接推送，异步，解耦了 HTTP 请求线程和 SSE 推送线程。
 *
 * 为什么用线程局部 session ID?
 * 因为 httplib 在多线程中并发处理 POST /message，每个线程处理不同的 session。
 * m_current_session_id 在进入 on_message 之前设置，send() 时读取。
 */
void HttpSseTransport::send(const std::string& json_message) {
    std::string sid = current_session();
    if (sid.empty()) return;  // 没有 session 上下文，丢弃

    auto session = get_session(sid);
    {
        std::lock_guard<std::mutex> lk(session->mtx);
        session->pending.push(json_message);      // 入队
    }
    session->cv.notify_one();                      // 唤醒 SSE Content Provider
}

void HttpSseTransport::set_on_message(MessageCallback callback) {
    m_callback = std::move(callback);
}

std::string HttpSseTransport::new_session_id() {
    std::string id = gen_uuid();
    std::lock_guard<std::mutex> lk(m_sessions_mtx);
    m_sessions[id] = std::make_shared<Session>();
    m_sessions[id]->id = id;
    LOG_INFO("New SSE session: %s", id.c_str());
    return id;
}

std::shared_ptr<HttpSseTransport::Session> HttpSseTransport::get_session(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_sessions_mtx);
    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) return it->second;
    auto s = std::make_shared<Session>();
    s->id = id;
    m_sessions[id] = s;
    return s;
}

void HttpSseTransport::set_current_session(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_current_id_mtx);
    m_current_session_id = id;
}

std::string HttpSseTransport::current_session() {
    std::lock_guard<std::mutex> lk(m_current_id_mtx);
    return m_current_session_id;
}

/**
 * 聊天请求处理 — LLM Agent 循环的入口
 *
 * 这是 HTTP 模式独有的功能，stdio 模式下不存在。
 *
 * 流程:
 *   1. 解析前端发来的聊天请求 (message, api_key, model, history...)
 *   2. 加载历史对话 (from ConversationManager)
 *   3. 创建 ToolOrchestrator，启动 LLM Agent 循环:
 *      user_message → LLM → (需要工具?) → tools/call → 工具结果 → LLM → ... → 最终回复
 *   4. 每个中间步骤通过 sse_send() 实时推送给前端 (流式打字效果)
 *
 * sse_send 的事件类型:
 *   "delta"       — LLM 输出的增量文本 (流式打字)
 *   "tool_call"   — LLM 决定调用某个工具
 *   "tool_result" — 工具执行完毕，返回结果
 *   "done"        — 对话完成，data 是完整回复
 *   "error"       — 出错
 */
void HttpSseTransport::handle_chat_request(
    const std::string& body,
    std::function<void(const std::string&, const std::string&)> sse_send) {

    if (!m_chat_config.mcp_handler) {
        sse_send("error", "Chat not configured");
        return;
    }

    // 解析前端发来的 JSON
    nlohmann::json req_json;
    try {
        req_json = nlohmann::json::parse(body);
    } catch (...) {
        sse_send("error", "Invalid JSON");
        return;
    }

    std::string user_message = req_json.value("message", "");
    std::string api_key = req_json.value("api_key", "");
    std::string model = req_json.value("model", m_chat_config.llm_model);
    std::string base_url = req_json.value("base_url", m_chat_config.llm_base_url);
    std::string conv_id = req_json.value("conversation_id", "");
    std::vector<std::string> disabled_tools;
    if (req_json.contains("disabled_tools") && req_json["disabled_tools"].is_array()) {
        for (auto& d : req_json["disabled_tools"]) disabled_tools.push_back(d.get<std::string>());
    }

    if (user_message.empty()) { sse_send("error", "Missing message"); return; }
    if (api_key.empty())     { sse_send("error", "Missing API key"); return; }

    // 加载历史记录 (优先从 ConversationManager 的持久化存储)
    std::vector<llm::Message> history;
    if (!conv_id.empty() && m_chat_config.conv_manager) {
        history = m_chat_config.conv_manager->get_history(conv_id);
    } else if (req_json.contains("history") && req_json["history"].is_array()) {
        for (auto& h : req_json["history"]) {
            llm::Message msg;
            msg.role = h.value("role", "user");
            msg.content = h.value("content", "");
            history.push_back(msg);
        }
    }

    // 保存用户消息到历史
    if (!conv_id.empty() && m_chat_config.conv_manager) {
        llm::Message um; um.role = "user"; um.content = user_message;
        m_chat_config.conv_manager->append_history(conv_id, um);
    }

    // 包装 sse_send，在发送事件的同时自动保存到对话历史
    auto full_reply = std::make_shared<std::string>();
    auto wrapped_send = [sse_send, full_reply, conv_id, this]
                        (const std::string& event, const std::string& data) {
        if (!conv_id.empty() && m_chat_config.conv_manager) {
            if (event == "done") {
                *full_reply = data;
                llm::Message am; am.role = "assistant"; am.content = data;
                m_chat_config.conv_manager->append_history(conv_id, am);
            } else if (event == "tool_result") {
                llm::Message tm; tm.role = "tool"; tm.content = data;
                m_chat_config.conv_manager->append_history(conv_id, tm);
            }
        }
        sse_send(event, data);
    };

    // 启动 LLM Agent 循环 (会反复调用 LLM + 工具，直到 LLM 给出最终回复)
    llm::ToolOrchestrator orch(*m_chat_config.mcp_handler);
    orch.process(user_message, api_key, base_url, model, history, disabled_tools, wrapped_send);
}

} // namespace transport
