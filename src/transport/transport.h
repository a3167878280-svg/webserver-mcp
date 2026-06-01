/**
 * 传输层抽象接口
 *
 * Transport 是 MCP 服务器最底层的"邮差"，负责:
 *   - 接收客户端发来的 JSON-RPC 字符串 (方向: 入站)
 *   - 发送服务器构造好的 JSON-RPC 字符串 (方向: 出站)
 *
 * 当前有两种实现:
 *   StdioTransport    — 通过 stdin/stdout 通信 (Claude Desktop 等本地 MCP 客户端)
 *   HttpSseTransport  — 通过 HTTP POST + SSE 长连接通信 (浏览器/远程客户端)
 *
 * 两种实现共享完全相同的上层逻辑 (解析→路由→执行→序列化)
 * 因为它们只管"字符串的传输"，不理解字符串的内容
 */
#pragma once

#include <functional>
#include <string>

namespace transport {

class Transport {
public:
    virtual ~Transport() = default;

    virtual void start() = 0;
    virtual void stop() = 0;

    /**
     * 发送 JSON-RPC 字符串到客户端
     * stdio 模式: 加 Content-Length 头后写入 stdout
     * HTTP 模式:  放入 SSE session 队列，通过 /sse 长连接异步推送
     */
    virtual void send(const std::string& json_message) = 0;

    /**
     * 消息回调 — 当收到完整 JSON-RPC 消息时调用
     * 回调由 main.cpp 设置，内部执行 parse → route → handle → serialize → send 管道
     */
    using MessageCallback = std::function<void(const std::string&)>;
    virtual void set_on_message(MessageCallback callback) = 0;
};

} // namespace transport
