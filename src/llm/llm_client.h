#pragma once

#include "../mcp/mcp_types.h"
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

namespace llm {

struct Message {
    std::string role;       // "user", "assistant", "tool", "system"
    std::string content;
    std::string tool_call_id;    // for role="tool"
    nlohmann::json tool_calls;   // for role="assistant" with tool calls
};

struct ToolCall {
    std::string id;
    std::string function_name;
    std::string arguments;       // JSON string
};

// 流式回调：delta 文本 + 累积的 tool_calls (可能不完整直到 finish)
using StreamCallback = std::function<void(
    const std::string& delta_text,
    const std::vector<ToolCall>& tool_calls,
    bool finished)>;

class LlmClient {
public:
    LlmClient();

    // 流式聊天完成。callback 可能被调用多次。
    // 返回 true 表示成功开始请求，false 表示网络错误
    bool chat_stream(
        const std::string& base_url,
        const std::string& api_key,
        const std::string& model,
        const std::vector<Message>& messages,
        const std::vector<mcp::ToolDef>& tools,
        StreamCallback callback);

private:
    // 解析 SSE data 行，提取 delta 和 tool_calls
    void parse_chunk(const std::string& data,
                     std::string& delta_out,
                     std::vector<ToolCall>& tc_out,
                     bool& finish_out);
};

} // namespace llm
