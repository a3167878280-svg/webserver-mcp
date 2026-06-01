/**
 * stdio 传输层 — MCP 协议最底层的"邮差"
 *
 * MCP 使用 stdin/stdout 作为传输通道，帧格式极其简单:
 *
 *   入站 (stdin):   Content-Length: <字节数>\r\n\r\n<JSON-RPC 消息体>
 *   出站 (stdout):   Content-Length: <字节数>\r\n\r\n<JSON-RPC 响应体>
 *
 * 这本质上就是一个"长度前缀"协议，解决 TCP 流式传输中的粘包问题。
 */

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

/**
 * 启动读线程，阻塞等待 stdin 数据
 * 对于 stdio 模式，start() 是阻塞的 — 它会一直运行直到 stdin 关闭
 */
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

/**
 * 发送 JSON-RPC 响应到 stdout
 *
 * MCP 出站帧格式:
 *   Content-Length: 89\r\n
 *   \r\n
 *   {"jsonrpc":"2.0","id":1,"result":{...}}
 *
 * 注意: 写入 stdout 后立即 fflush — Claude Desktop 在管道另一端等待数据
 */
void StdioTransport::send(const std::string& json_message) {
    std::lock_guard<std::mutex> lock(m_write_mutex);
    // 构建 MCP stdio 帧: 长度头 + 空行 + JSON body
    std::string header = "Content-Length: " + std::to_string(json_message.size()) + "\r\n\r\n";
    std::string full = header + json_message;
    fwrite(full.data(), 1, full.size(), stdout);
    fflush(stdout);
}

void StdioTransport::set_on_message(MessageCallback callback) {
    m_callback = std::move(callback);
}

/**
 * stdin 读线程 — 核心循环
 *
 * MCP 入站帧格式示例:
 *   Content-Length: 85\r\n
 *   \r\n
 *   {"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}
 *
 * 解析流程:
 *   1. 逐字节读取直到遇到 "\r\n\r\n" → 得到 Header
 *   2. 从 Header 中提取 Content-Length 的值 N
 *   3. 精确读取 N 字节 → 得到 JSON-RPC 消息体
 *   4. 通过 m_callback 回调传给上层 (main.cpp 的 on_message)
 */
void StdioTransport::read_loop() {
    std::string header_buf;
    header_buf.reserve(256);

    while (m_running) {
        // ── 阶段 1: 读取 Header，直到 "\r\n\r\n" ──
        header_buf.clear();
        int crlf_seq = 0;  // 跟踪连续 \r\n 序列，需要连续两次 \r\n 才算结束
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
                // 等待下一个字符判断是 \r\n
            } else if (ch == '\n') {
                if (header_buf.size() >= 2 && header_buf[header_buf.size() - 2] == '\r') {
                    crlf_seq++;
                    if (crlf_seq == 2) {  // 检测到 \r\n\r\n → header 结束
                        break;
                    }
                }
            } else {
                crlf_seq = 0;
            }
        }

        if (!m_running) break;

        // ── 阶段 2: 从 Header 提取 Content-Length ──
        // 去掉末尾的 \r\n\r\n (4字节)
        if (header_buf.size() >= 4) {
            header_buf.resize(header_buf.size() - 4);
        }

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

        // ── 阶段 3: 精准读取 N 字节 body ──
        std::string body = read_exact(content_length);
        if (body.size() != static_cast<size_t>(content_length)) {
            LOG_ERROR("Incomplete body: expected %d, got %zu", content_length, body.size());
            if (!m_running) break;
            continue;
        }

        // ── 阶段 4: 交给上层处理 ──
        // 这里的 m_callback 就是 main.cpp 中设置的 on_message lambda
        // body 是一个完整的 JSON-RPC 字符串，如:
        //   {"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}
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
