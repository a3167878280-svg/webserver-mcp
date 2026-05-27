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

namespace transport {

struct ChatConfig {
    std::string llm_base_url;
    std::string llm_model;
    mcp::McpHandler* mcp_handler = nullptr;
};

class HttpSseTransport : public Transport {
public:
    HttpSseTransport();
    ~HttpSseTransport() override;

    void start() override;
    void start(int port);
    void stop() override;
    void send(const std::string& json_message) override;
    void set_on_message(MessageCallback callback) override;

    void set_chat_config(const ChatConfig& cfg) { m_chat_config = cfg; }

    int port() const { return m_port; }

private:
    struct Session {
        std::string id;
        std::queue<std::string> pending;
        std::mutex mtx;
        std::condition_variable cv;
    };

    std::string new_session_id();
    std::shared_ptr<Session> get_session(const std::string& id);

    MessageCallback m_callback;
    int m_port = 9006;
    std::thread m_server_thread;
    std::atomic<bool> m_running{false};

    std::mutex m_sessions_mtx;
    std::unordered_map<std::string, std::shared_ptr<Session>> m_sessions;

    // send() 时使用的目标 session ID
    std::mutex m_current_id_mtx;
    std::string m_current_session_id;
    void set_current_session(const std::string& id);
    std::string current_session();

    // 聊天功能
    ChatConfig m_chat_config;
    void setup_chat_routes();
    void handle_chat_request(const std::string& body, std::function<void(const std::string&, const std::string&)> sse_send);
};

} // namespace transport
