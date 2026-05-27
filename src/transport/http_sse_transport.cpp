#include "http_sse_transport.h"
#include "../log/log.h"
#include "../common.h"
#include "../mcp/mcp_handler.h"
#include "../mcp/jsonrpc.h"
#include "../llm/tool_orchestrator.h"
#include "httplib.h"
#include <random>
#include <sstream>
#include <fstream>

namespace transport {

// 生成随机 session ID
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

void HttpSseTransport::start(int port) {
    m_port = port;
    m_running = true;

    m_server_thread = std::thread([this]() {
        httplib::Server srv;

        // CORS 中间件
        srv.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            return httplib::Server::HandlerResponse::Unhandled;
        });

        // OPTIONS 预检
        srv.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.status = 204;
        });

        // 健康检查
        srv.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("OK", "text/plain");
        });

        // SSE 端点
        srv.Get("/sse", [this](const httplib::Request& req, httplib::Response& res) {
            std::string session_id;
            if (req.has_param("session_id")) {
                session_id = req.get_param_value("session_id");
            } else {
                session_id = new_session_id();
            }

            // 创建或复用 session
            auto session = get_session(session_id);

            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");

            // 发送 endpoint 事件
            std::string endpoint_url = "/message?session_id=" + session_id;
            std::string endpoint_event = "event: endpoint\ndata: " + endpoint_url + "\n\n";

            // 使用 chunked 响应 + content provider 实现长连接
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, session, endpoint_url](size_t /*offset*/, httplib::DataSink& sink) {
                    // 先发送 endpoint 事件
                    std::string ev = "event: endpoint\ndata: " + endpoint_url + "\n\n";
                    sink.write(ev.data(), ev.size());

                    // 循环等待并发送 pending 消息
                    while (m_running) {
                        std::unique_lock<std::mutex> lock(session->mtx);
                        session->cv.wait_for(lock, std::chrono::milliseconds(500),
                            [&] { return !session->pending.empty() || !m_running; });

                        while (!session->pending.empty()) {
                            std::string msg = std::move(session->pending.front());
                            session->pending.pop();
                            lock.unlock();

                            std::string framed = "event: message\ndata: " + msg + "\n\n";
                            if (!sink.write(framed.data(), framed.size())) {
                                return false;  // 客户端断开
                            }

                            lock.lock();
                        }

                        if (!m_running) break;
                    }
                    return true;
                });
        });

        // POST 消息端点
        srv.Post("/message", [this](const httplib::Request& req, httplib::Response& res) {
            std::string session_id;
            if (req.has_param("session_id")) {
                session_id = req.get_param_value("session_id");
            } else {
                res.status = 400;
                res.set_content("Missing session_id", "text/plain");
                return;
            }

            // 检查 session 是否存在
            {
                std::lock_guard<std::mutex> lk(m_sessions_mtx);
                if (m_sessions.find(session_id) == m_sessions.end()) {
                    res.status = 404;
                    res.set_content("Session not found", "text/plain");
                    return;
                }
            }

            set_current_session(session_id);

            if (m_callback) {
                m_callback(req.body);
            }

            // 请求已接受（响应通过 SSE 异步返回）
            res.set_content("Accepted", "text/plain");
            res.status = 202;
        });

        // 聊天 API (SSE 流式)
        srv.Post("/api/chat", [this](const httplib::Request& req, httplib::Response& res) {
            auto processed = std::make_shared<bool>(false);
            res.set_chunked_content_provider("text/event-stream",
                [this, body = req.body, processed](size_t /*offset*/, httplib::DataSink& sink) {
                    if (*processed) return true;  // 只处理一次
                    *processed = true;
                    handle_chat_request(body,
                        [&sink](const std::string& event, const std::string& data) {
                            std::string framed = "event: " + event + "\ndata: " + data + "\n\n";
                            return sink.write(framed.data(), framed.size());
                        });
                    sink.done();  // 标记完成
                    return true;
                });
        });

        // 模型列表
        srv.Get("/api/models", [this](const httplib::Request&, httplib::Response& res) {
            nlohmann::json j;
            j["models"] = nlohmann::json::array({
                {{"id", "gpt-4o"}, {"name", "GPT-4o"}},
                {{"id", "gpt-4o-mini"}, {"name", "GPT-4o Mini"}},
                {{"id", "claude-sonnet-4-6"}, {"name", "Claude Sonnet 4.6"}},
                {{"id", "claude-opus-4-7"}, {"name", "Claude Opus 4.7"}},
                {{"id", "qwen2.5:7b"}, {"name", "Qwen 2.5 7B (Ollama)"}},
                {{"id", "llama3:8b"}, {"name", "Llama 3 8B (Ollama)"}},
            });
            res.set_content(j.dump(), "application/json");
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

    // 唤醒所有 session 等待者
    {
        std::lock_guard<std::mutex> lk(m_sessions_mtx);
        for (auto& kv : m_sessions) {
            std::lock_guard<std::mutex> slk(kv.second->mtx);
            kv.second->cv.notify_all();
        }
    }

    // httplib::Server::listen 是阻塞的，无法从外部停止
    // 使用 stop 后主线程需等待 server_thread (析构时 join)
    if (m_server_thread.joinable()) {
        m_server_thread.detach();  // srv.listen 无法优雅停止，detach 让 OS 清理
    }
    LOG_INFO("HttpSseTransport stopped");
}

void HttpSseTransport::send(const std::string& json_message) {
    std::string sid = current_session();
    if (sid.empty()) return;

    auto session = get_session(sid);
    {
        std::lock_guard<std::mutex> lk(session->mtx);
        session->pending.push(json_message);
    }
    session->cv.notify_one();
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

void HttpSseTransport::handle_chat_request(
    const std::string& body,
    std::function<void(const std::string&, const std::string&)> sse_send) {

    if (!m_chat_config.mcp_handler) {
        sse_send("error", "Chat not configured");
        return;
    }

    // 解析请求
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

    if (user_message.empty()) {
        sse_send("error", "Missing message");
        return;
    }
    if (api_key.empty()) {
        sse_send("error", "Missing API key");
        return;
    }

    // 解析历史记录
    std::vector<llm::Message> history;
    if (req_json.contains("history") && req_json["history"].is_array()) {
        for (auto& h : req_json["history"]) {
            llm::Message msg;
            msg.role = h.value("role", "user");
            msg.content = h.value("content", "");
            history.push_back(msg);
        }
    }

    // 编排工具调用
    llm::ToolOrchestrator orch(*m_chat_config.mcp_handler);
    orch.process(user_message, api_key, base_url, model, history, sse_send);
}

} // namespace transport
