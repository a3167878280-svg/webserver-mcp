#include "http_sse_transport.h"
#include "../log/log.h"
#include "../common.h"
#include "httplib.h"
#include <random>
#include <sstream>

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

} // namespace transport
