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

namespace transport {

class HttpSseTransport : public Transport {
public:
    HttpSseTransport();
    ~HttpSseTransport() override;

    void start() override;                             // 非阻塞，启动 HTTP 线程
    void start(int port);                              // 指定端口
    void stop() override;
    void send(const std::string& json_message) override;
    void set_on_message(MessageCallback callback) override;

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

    // send() 时使用的目标 session ID（由 POST 处理器设置）
    std::mutex m_current_id_mtx;
    std::string m_current_session_id;
    void set_current_session(const std::string& id);
    std::string current_session();
};

} // namespace transport
