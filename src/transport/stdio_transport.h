#pragma once

#include "transport.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <string>

namespace transport {

class StdioTransport : public Transport {
public:
    StdioTransport();
    ~StdioTransport() override;

    void start() override;
    void stop() override;
    void send(const std::string& json_message) override;
    void set_on_message(MessageCallback callback) override;

private:
    void read_loop();
    std::string read_exact(size_t num_bytes);

    MessageCallback m_callback;
    std::thread m_read_thread;
    std::atomic<bool> m_running{false};
    std::mutex m_write_mutex;  // 保护 stdout 写入
};

} // namespace transport
