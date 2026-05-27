#pragma once

#include "llm_client.h"
#include "../mcp/mcp_handler.h"
#include <string>
#include <functional>

namespace llm {

// 编排回调: event_type 可以是 "delta", "tool_start", "tool_result", "error", "done"
using OrchestratorCallback = std::function<void(
    const std::string& event_type,
    const std::string& data)>;

class ToolOrchestrator {
public:
    ToolOrchestrator(mcp::McpHandler& handler);

    // 处理用户消息，通过 callback 流式返回结果
    void process(
        const std::string& user_message,
        const std::string& api_key,
        const std::string& base_url,
        const std::string& model,
        const std::vector<Message>& history,
        OrchestratorCallback callback);

private:
    mcp::McpHandler& m_handler;
    static constexpr int MAX_ROUNDS = 10;  // 防止无限循环
};

} // namespace llm
