/**
 * Tool Orchestrator — LLM Agent 循环 (整个聊天功能的大脑)
 *
 * ═══════════ 什么是 Agent 循环? ═══════════
 *
 * 普通的 LLM 调用:  用户问 → LLM 答 → 结束
 * Agent 循环:        用户问 → LLM → 需要工具? → 调工具 → 结果喂 LLM → 还要工具? → ... → 最终答
 *
 * 这就是为什么 ChatGPT/Claude 能帮你查天气、读文件 — 它们不是"知道"天气，
 * 而是看到你问天气，就调用 query_weather 工具，拿到结果后再告诉你。
 *
 * 循环最多 10 轮，防止工具调用出错导致无限循环。
 *
 * ═══════════ 数据流 ═══════════
 *
 *   process(user_message)
 *       │
 *       ├─ 1. 调 tools/list → 拿到可用工具列表
 *       ├─ 2. 过滤掉 disabled_tools
 *       │
 *       └─ 3. while (round < 10):
 *              │
 *              ├─ LlmClient::chat_stream(messages, tools)
 *              │    ↓ SSE 流式返回
 *              │   callback("delta", "我来查一下...")  → 推给浏览器 (打字效果)
 *              │   callback("delta", "今天")
 *              │   callback("delta", "北京")
 *              │   ... finished=true, tool_calls=[
 *              │         {id:"call_1", name:"query_weather", args:'{"city":"Beijing"}'}
 *              │       ]
 *              │
 *              ├─ 如果 LLM 返回了 tool_calls:
 *              │   │
 *              │   ├─ callback("tool_start", "{...}")       → 推给浏览器
 *              │   │
 *              │   ├─ 调 tools/call → 执行 weather 插件
 *              │   │
 *              │   ├─ callback("tool_result", "{...}")      → 推给浏览器
 *              │   │
 *              │   ├─ 工具结果追加到 messages 数组
 *              │   └─ continue (回到循环开头，让 LLM 处理结果)
 *              │
 *              └─ 如果 LLM 直接回复了文本 (无 tool_calls):
 *                 callback("done", full_content)  → 结束
 *
 * ═══════════ 回调事件类型 ═══════════
 *
 *   "delta"       — LLM 输出的增量文本 (每次一个字/词，打字机效果)
 *   "tool_start"  — LLM 决定调用工具 (data = JSON，含 tool 名和 args)
 *   "tool_result" — 工具执行完毕 (data = JSON，含执行结果)
 *   "done"        — 对话完成 (data = LLM 的完整回复文本)
 *   "error"       — 出错 (data = 错误消息)
 */

#pragma once

#include "llm_client.h"
#include "../mcp/mcp_handler.h"
#include <string>
#include <functional>

namespace llm {

using OrchestratorCallback = std::function<void(
    const std::string& event_type,
    const std::string& data)>;

class ToolOrchestrator {
public:
    ToolOrchestrator(mcp::McpHandler& handler);

    /**
     * 处理用户消息 — Agent 循环入口
     *
     * @param user_message   用户在聊天框输入的消息
     * @param api_key        用户提供的 LLM API key
     * @param base_url       用户选择的 LLM API 地址
     * @param model          用户选择的模型
     * @param history        历史对话记录 (从 ConversationManager 加载)
     * @param disabled_tools 用户禁用的工具列表
     * @param callback       每步推给前端的回调
     */
    void process(
        const std::string& user_message,
        const std::string& api_key,
        const std::string& base_url,
        const std::string& model,
        const std::vector<Message>& history,
        const std::vector<std::string>& disabled_tools,
        OrchestratorCallback callback);

private:
    mcp::McpHandler& m_handler;
    static constexpr int MAX_ROUNDS = 10;  // 防止死循环: 最多调 10 轮工具
};

} // namespace llm
