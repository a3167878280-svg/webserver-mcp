/**
 * LLM 客户端 — 调用 OpenAI / Anthropic / DeepSeek API
 *
 * ═══════════ 这个模块在架构中的位置 ═══════════
 *
 *   ToolOrchestrator (Agent 循环)
 *       │
 *       ├─ 调 tools/list 拿到工具列表
 *       │
 *       └─ 调 LlmClient::chat_stream()  ← 这个文件
 *            │
 *            ├─ OpenAI API   → POST /v1/chat/completions (SSE 流式)
 *            └─ Anthropic API → POST /v1/messages (SSE 流式)
 *
 * ═══════════ LLM 的两种返回模式 ═══════════
 *
 *   1. 纯文本: LLM 直接回复文字
 *      callback("今天北京天气晴朗", {}, false)
 *      callback("", {}, true)  ← finished=true, 无 tool_calls
 *
 *   2. 请求调用工具: LLM 说"我需要调 query_weather 工具"
 *      先是 delta 文本流 (如 "我来查一下天气...")
 *      然后是 tool_calls 流 (增量构建):
 *        callback("", [{id:"call_1", function_name:"query_weather", arguments:""}], false)
 *        callback("", [{id:"call_1", function_name:"", arguments:"{\"city\":"}], false)
 *        callback("", [{id:"call_1", function_name:"", arguments:"\"Beijing\"}"}], false)
 *        callback("", [{id:"call_1", ...}], true)  ← finished=true, 携带完整 tool_calls
 *
 * ═══════════ API 类型自动检测 ═══════════
 *
 *   根据 base_url 是否包含 "anthropic" 来判断:
 *     https://api.openai.com/v1      → chat_openai()
 *     https://api.anthropic.com       → chat_anthropic()
 *     http://localhost:11434/v1       → chat_openai() (Ollama 兼容 OpenAI 格式)
 *     https://api.deepseek.com/v1     → chat_openai() (DeepSeek 兼容 OpenAI 格式)
 */

#pragma once

#include "../mcp/mcp_types.h"
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace llm {

/**
 * 对话消息 — 发送给 LLM 的 messages 数组中的一条
 *
 * 对应 OpenAI Chat Completions API 的 message 结构:
 *   {"role":"user", "content":"你好"}
 *   {"role":"assistant", "content":"你好!", "tool_calls":[...]}
 *   {"role":"tool", "tool_call_id":"call_1", "content":"{...}"}
 */
struct Message {
    std::string role;            // "user" | "assistant" | "tool" | "system"
    std::string content;         // 消息正文
    std::string tool_call_id;    // role="tool" 时填写，关联到哪个 tool_call
    nlohmann::json tool_calls;   // role="assistant" 且 LLM 要调工具时填写
};

/**
 * LLM 返回的工具调用 — 从 SSE 流中解析出来
 *
 * LLM 说"我要调这个工具":
 *   id:            call_abc123  (OpenAI/Anthropic 分配的唯一 ID)
 *   function_name: query_weather
 *   arguments:     {"city":"Beijing"}  (JSON 字符串，不是对象!)
 */
struct ToolCall {
    std::string id;
    std::string function_name;
    std::string arguments;       // JSON 字符串 (流式场景下增量构建)
};

/**
 * 流式回调 — LlmClient 每收到一个 SSE chunk 就调一次
 *
 * @param delta_text  增量文本 (LLM 的打字机效果)
 * @param tool_calls  当前累积的工具调用 (为空 = 纯文本模式)
 * @param finished    true = LLM 这轮回复结束
 */
using StreamCallback = std::function<void(
    const std::string& delta_text,
    const std::vector<ToolCall>& tool_calls,
    bool finished)>;

class LlmClient {
public:
    LlmClient();

    /**
     * 流式聊天 — 一次完整的 LLM 调用
     *
     * @param base_url  API 地址 (自动检测 OpenAI/Anthropic)
     * @param api_key   用户在前端输入的 API key
     * @param model     模型名 如 "gpt-4o" / "claude-sonnet-4-6"
     * @param messages  对话历史 + 当前用户消息
     * @param tools     可用工具列表 (传给 LLM，让它知道可以调哪些工具)
     * @param callback  每次收到 SSE 数据时调用
     * @return true=成功, false=网络错误
     */
    bool chat_stream(
        const std::string& base_url,
        const std::string& api_key,
        const std::string& model,
        const std::vector<Message>& messages,
        const std::vector<mcp::ToolDef>& tools,
        StreamCallback callback);

private:
    bool chat_openai(const std::string& host, const std::string& path,
        const std::string& api_key, const std::string& model,
        const std::vector<Message>& messages,
        const std::vector<mcp::ToolDef>& tools,
        StreamCallback callback);

    bool chat_anthropic(const std::string& host, const std::string& path,
        const std::string& api_key, const std::string& model,
        const std::vector<Message>& messages,
        const std::vector<mcp::ToolDef>& tools,
        StreamCallback callback);

    // SSE chunk 解析器 — 从一行 "data: {...}" 中提取 delta/text/tool_calls/finished
    void parse_openai_chunk(const std::string& data,
        std::string& delta_out, std::vector<ToolCall>& tc_out, bool& finish_out);

    void parse_anthropic_chunk(const std::string& data,
        std::string& delta_out, std::vector<ToolCall>& tc_out, bool& finish_out);

    /**
     * Anthropic 的 tool_use index → id 映射
     *
     * Anthropic 的流式协议中，content_block_start 给出真实 id 和 index，
     * 后续的 input_json_delta 只带 index 不带 id。
     * 所以需要维护 index→id 映射，把参数分片关联到正确的工具调用。
     */
    std::unordered_map<int, std::string> m_tool_index_map;
};

} // namespace llm
