#include "stdio_transport.h"
#include "common.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>

namespace transport {

StdioTransport::StdioTransport() = default;

StdioTransport::~StdioTransport() {
    stop();
}

void StdioTransport::start() {
    m_running = true;
    m_read_thread = std::thread(&StdioTransport::read_loop, this);
    LOG_INFO("Stdio transport started");

    // 阻塞等待读线程结束 (stdin EOF 或 stop() 被调用)
    m_read_thread.join();
}

void StdioTransport::stop() {
    if (!m_running) return;
    m_running = false;
    LOG_INFO("Stdio transport stopping");
}

void StdioTransport::send(const std::string& json_message) {
    std::lock_guard<std::mutex> lock(m_write_mutex);
    // MCP stdio 格式: Content-Length: <N>\r\n\r\n<json>
    std::string header = "Content-Length: " + std::to_string(json_message.size()) + "\r\n\r\n";
    std::string full = header + json_message;
    fwrite(full.data(), 1, full.size(), stdout);
    fflush(stdout);
}

void StdioTransport::set_on_message(MessageCallback callback) {
    m_callback = std::move(callback);
}

void StdioTransport::read_loop() {
    std::string header_buf;
    header_buf.reserve(256);

    while (m_running) {
        // 逐字符读取直到 "\r\n\r\n" 结束标记
        header_buf.clear();
        int crlf_seq = 0;  // 跟踪 \r\n\r\n 序列
        while (m_running) {
            int c = fgetc(stdin);
            if (c == EOF) {
                if (ferror(stdin)) {
                    LOG_ERROR("stdin read error: %s", strerror(errno));
                }
                m_running = false;
                break;
            }
            char ch = static_cast<char>(c);
            header_buf.push_back(ch);

            if (ch == '\r') {
                // 等待下一个字符
            } else if (ch == '\n') {
                if (header_buf.size() >= 2 && header_buf[header_buf.size() - 2] == '\r') {
                    crlf_seq++;
                    if (crlf_seq == 2) {  // \r\n\r\n
                        break;
                    }
                }
            } else {
                crlf_seq = 0;
            }
        }

        if (!m_running) break;

        // 移除末尾的 \r\n\r\n
        if (header_buf.size() >= 4) {
            header_buf.resize(header_buf.size() - 4);
        }

        // 解析 Content-Length
        const char* prefix = "Content-Length: ";
        size_t pos = header_buf.find(prefix);
        if (pos == std::string::npos) {
            LOG_ERROR("Missing Content-Length header");
            continue;
        }
        pos += strlen(prefix);
        size_t end = header_buf.find('\r', pos);
        std::string len_str = (end != std::string::npos)
                              ? header_buf.substr(pos, end - pos)
                              : header_buf.substr(pos);
        int content_length = std::stoi(len_str);

        // 读取 body
        std::string body = read_exact(content_length);
        if (body.size() != static_cast<size_t>(content_length)) {
            LOG_ERROR("Incomplete body: expected %d, got %zu", content_length, body.size());
            if (!m_running) break;
            continue;
        }

        // 回调处理消息
        if (m_callback) {
            m_callback(std::move(body));
        }
    }
}

std::string StdioTransport::read_exact(size_t num_bytes) {
    std::string result(num_bytes, '\0');
    size_t total = 0;
    while (total < num_bytes && m_running) {
        int n = fread(&result[total], 1, num_bytes - total, stdin);
        if (n <= 0) {
            if (ferror(stdin) || feof(stdin)) {
                LOG_ERROR("stdin read error or EOF at byte %zu/%zu", total, num_bytes);
                m_running = false;
                break;
            }
            continue;
        }
        total += n;
    }
    result.resize(total);
    return result;
}

} // namespace transport
