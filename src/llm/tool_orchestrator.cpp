/**
 * Tool Orchestrator 实现 — LLM Agent 循环的核心
 *
 * ═══════════ 完整流程示例 ═══════════
 *
 * 用户在浏览器输入: "北京今天天气怎么样?"
 *
 * Round 1:
 *   messages = [history..., {role:"user", content:"北京今天天气怎么样?"}]
 *   tools    = [query_weather, file_read, file_list, shell_exec, ...]
 *   →
 *   LlmClient::chat_stream(messages, tools)
 *   →
 *   LLM 返回: delta="我来查一下北京天气...", tool_calls=[
 *     {id:"call_1", function_name:"query_weather", arguments:'{"city":"Beijing"}'}
 *   ]
 *   →
 *   Orchestrator: 调 m_handler.handle("tools/call", {name:"query_weather", arguments:{city:"Beijing"}})
 *   →
 *   WeatherPlugin 返回: {content:[{text:"Beijing: Sunny, 15°C"}], isError:false}
 *   →
 *   工具结果追加到 messages
 *
 * Round 2:
 *   messages = [..., assistant(tool_calls), tool_result("Beijing: Sunny, 15°C")]
 *   →
 *   LlmClient::chat_stream(messages, tools)
 *   →
 *   LLM 返回: delta="北京今天天气晴朗，气温15°C，适合出行。"
 *   无 tool_calls → 最终回复
 *   →
 *   callback("done", "北京今天天气晴朗，气温15°C，适合出行。")
 */

#include "tool_orchestrator.h"
#include "../mcp/jsonrpc.h"
#include "../log/log.h"
#include "../common.h"
#include "context_manager.h"
#include <algorithm>

namespace llm {

// 去除 <think>...</think> 块 (Qwen3/DeepSeek 思考内容)
static std::string strip_think(const std::string& text) {
    std::string result;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t open = text.find("<think>", pos);
        if (open == std::string::npos) {
            result += text.substr(pos);
            break;
        }
        result += text.substr(pos, open - pos);
        size_t close = text.find("</think>", open + 7);
        if (close == std::string::npos) {
            break;  // 未闭合，丢弃剩余
        }
        pos = close + 8;
    }
    // 清理空行
    while (!result.empty() && (result[0] == '\n' || result[0] == '\r')) result.erase(0, 1);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

ToolOrchestrator::ToolOrchestrator(mcp::McpHandler& handler)
    : m_handler(handler) {}

/**
 * Agent 循环主函数
 *
 * 伪代码:
 *   messages = history + user_message
 *   tools = get_all_tools() - disabled_tools
 *   for round = 1..10:
 *       response = LLM.chat(messages, tools)
 *       if response has tool_calls:
 *           for each tool_call:
 *               result = execute_tool(tool_call)
 *               messages += result
 *           continue  // 再问 LLM
 *       else:
 *           return response.text  // 最终回复
 */
void ToolOrchestrator::process(
    const std::string& user_message,
    const std::string& api_key,
    const std::string& base_url,
    const std::string& model,
    const std::vector<Message>& history,
    const std::vector<std::string>& disabled_tools,
    OrchestratorCallback callback) {

    // ── 步骤 1: 构建初始 messages ──
    std::vector<Message> messages = history;

    // 注入 system prompt — 引导 LLM 如何高效使用工具
    bool has_system = !history.empty() && history[0].role == "system";
    if (!has_system) {
        Message sys;
        sys.role = "system";
        sys.content =
            "/no_think 你是 Linux 助手。需要工具就直接调用，拿到结果用中文一句话总结。不要解释过程。";
        messages.insert(messages.begin(), sys);
    }

    // 把历史记录 + 用户新消息拼成 LLM 认识的 messages 数组
    Message user_msg;
    user_msg.role = "user";
    user_msg.content = user_message;
    messages.push_back(user_msg);

    // ── 步骤 2: 获取可用工具列表 ──
    // 内部走 tools/list → PluginRegistry → 所有插件的所有工具
    nlohmann::json tools_result;
    {
        mcp::JsonRpcRequest req;
        req.jsonrpc = "2.0";
        req.method = "tools/list";
        req.id = "internal";  // 必须有 id (McpHandler 根据 id 是否 null 判断是不是 notification)
        auto resp = m_handler.handle(req);
        if (resp.has_value()) {
            tools_result = resp->result;
        }
    }

    // 从 JSON 解析出 ToolDef 数组
    std::vector<mcp::ToolDef> tools;
    if (tools_result.contains("tools") && tools_result["tools"].is_array()) {
        for (auto& jt : tools_result["tools"]) {
            mcp::ToolDef td;
            td.name = jt.value("name", "");
            td.description = jt.value("description", "");
            td.inputSchema = jt.value("inputSchema", nlohmann::json::object());
            tools.push_back(td);
        }
    }

    // ── 步骤 3: 过滤用户禁用的工具 ──
    if (!disabled_tools.empty()) {
        if (std::find(disabled_tools.begin(), disabled_tools.end(), "*") != disabled_tools.end()) {
            tools.clear();
        } else {
            tools.erase(std::remove_if(tools.begin(), tools.end(),
                [&](const mcp::ToolDef& t) {
                    return std::find(disabled_tools.begin(), disabled_tools.end(), t.name) != disabled_tools.end();
                }), tools.end());
        }
    }

    // ── 步骤 4: Agent 循环 ──
    LlmClient client;
    int round = 0;

    while (round < MAX_ROUNDS) {
        round++;

        // ── 上下文管理: 截断过长工具结果 + 必要时摘要压缩 ──
        prepare_context(messages, model, client, base_url, api_key, callback);

        std::string full_content;            // 累积本轮 LLM 输出的所有文本
        std::vector<ToolCall> final_tool_calls; // LLM 最终决定的工具调用
        bool llm_finished = false;

        // 调 LLM (流式)
        bool ok = client.chat_stream(base_url, api_key, model,
            messages, tools,
            [&](const std::string& delta, const std::vector<ToolCall>& tc, bool finished) {
                // 文本增量 → 立即推给浏览器 (打字效果)
                if (!delta.empty()) {
                    full_content += delta;
                    callback("delta", delta);
                }
                // LLM 完成了 + 携带工具调用
                if (finished && !tc.empty()) {
                    final_tool_calls = tc;
                }
                if (finished) llm_finished = true;
            });

        if (!ok) {
            std::string err = client.last_error().empty()
                ? "Failed to connect to LLM API"
                : client.last_error();
            callback("error", err);
            return;
        }

        // ── 分支 A: LLM 要求调用工具 ──
        if (!final_tool_calls.empty()) {
            // 把 assistant 的 tool_calls 消息追加到历史
            Message assistant_msg;
            assistant_msg.role = "assistant";
            assistant_msg.content = full_content;

            nlohmann::json tool_calls_json = nlohmann::json::array();
            for (auto& tc : final_tool_calls) {
                nlohmann::json tcj;
                tcj["id"] = tc.id;
                tcj["type"] = "function";
                tcj["function"]["name"] = tc.function_name;
                tcj["function"]["arguments"] = tc.arguments;
                tool_calls_json.push_back(tcj);
            }
            assistant_msg.tool_calls = tool_calls_json;
            messages.push_back(assistant_msg);

            // 逐个执行工具
            for (auto& tc : final_tool_calls) {
                // 通知浏览器: "正在调工具"
                callback("tool_start",
                    "{\"tool\":\"" + tc.function_name + "\",\"args\":" + tc.arguments + "}");

                // 解析 arguments JSON 字符串 → nlohmann::json 对象
                nlohmann::json args;
                try {
                    args = nlohmann::json::parse(tc.arguments);
                } catch (...) {
                    args = nlohmann::json::object();
                }

                // 调 MCP tools/call → 走 PluginRegistry → 插件执行
                mcp::JsonRpcRequest req;
                req.jsonrpc = "2.0";
                req.method = "tools/call";
                req.id = "internal";
                req.params = {
                    {"name", tc.function_name},
                    {"arguments", args}
                };

                std::string tool_result_text;
                std::string frontend_text;
                auto resp = m_handler.handle(req);
                if (resp.has_value()) {
                    nlohmann::json& res = resp->result;
                    // 提取纯文本给 LLM (去掉 JSON 包装)
                    if (res.contains("content") && res["content"].is_array() &&
                        !res["content"].empty() && res["content"][0].contains("text")) {
                        tool_result_text = res["content"][0]["text"].get<std::string>();
                    } else {
                        tool_result_text = res.dump();
                    }
                    if (res.value("isError", false)) {
                        tool_result_text = "[工具执行出错] " + tool_result_text;
                    }
                    // 引导 LLM 总结
                    tool_result_text += "\n\n---\n请基于以上数据给用户一个明确、直接的中文回答。不要只重复原始数据，要给出结论。";

                    // 截断过长的工具结果 (单结果不超过上下文预算的 25%)
                    const size_t tool_budget = context::context_budget(model) / 4;
                    if (context::estimate_tokens(tool_result_text) > tool_budget) {
                        size_t orig = context::estimate_tokens(tool_result_text);
                        tool_result_text = context::truncate_tool_result(tool_result_text, tool_budget);
                        LOG_INFO("Truncated tool result: %zu -> %zu estimated tokens",
                                 orig, context::estimate_tokens(tool_result_text));
                    }
                    frontend_text = res.dump();
                } else {
                    tool_result_text = "工具执行失败，请告知用户此工具暂时不可用。";
                    frontend_text = "{}";
                }

                callback("tool_result", frontend_text);

                // 把工具结果追加到 messages (下一轮 LLM 会看到)
                Message tool_msg;
                tool_msg.role = "tool";
                tool_msg.tool_call_id = tc.id;
                tool_msg.content = tool_result_text;
                messages.push_back(tool_msg);
            }
            continue;  // ← 回到循环开头，让 LLM 处理工具结果
        }

        // ── 分支 B: LLM 直接给出最终回复 (无工具调用) ──
        callback("done", strip_think(full_content));
        return;
    }

    // 超过 10 轮 → 强制终止
    callback("error", "Max tool calling rounds reached");
}

/**
 * 上下文准备 — 在每轮 LLM 调用前执行
 *
 * 1. 遍历已有 tool 消息，截断超过预算 25% 的单个结果
 * 2. 如果 messages 总量超出模型上下文预算的 70%，触发摘要压缩
 *    摘要最旧的 ~60% 消息，替换为一条 system 角色摘要
 */
void ToolOrchestrator::prepare_context(
    std::vector<Message>& messages,
    const std::string& model,
    LlmClient& client,
    const std::string& base_url,
    const std::string& api_key,
    OrchestratorCallback callback) {

    const size_t budget = context::context_budget(model);
    const size_t tool_max = budget / 4;  // 单个工具结果最多占 25% 预算

    // 步骤 1: 截断已有的过长 tool 消息
    for (auto& msg : messages) {
        if (msg.role == "tool") {
            size_t tok = context::estimate_tokens(msg.content);
            if (tok > tool_max) {
                msg.content = context::truncate_tool_result(msg.content, tool_max);
                LOG_INFO("Truncated historical tool result: %zu -> %zu tokens",
                         tok, context::estimate_tokens(msg.content));
            }
        }
    }

    // 步骤 2: 如果总量仍超出预算且消息数足够，触发摘要压缩
    if (context::needs_truncation(messages, model) && messages.size() > 6) {
        // 跳过 system 消息，摘要最旧的 ~60% 非 system 消息
        size_t sys_count = 0;
        while (sys_count < messages.size() && messages[sys_count].role == "system") {
            sys_count++;
        }
        size_t non_sys = messages.size() - sys_count;
        size_t summarize_count = non_sys * 3 / 5;  // 60%
        if (summarize_count < 2) summarize_count = 2;
        if (summarize_count > non_sys - 2) summarize_count = non_sys - 2;  // 至少保留最后 2 条

        // 构建要摘要的子区间
        std::vector<Message> to_summarize(
            messages.begin() + sys_count,
            messages.begin() + sys_count + summarize_count);

        // 调 LLM 做摘要
        callback("delta", "\n[压缩历史对话...]\n");
        Message summary = summarize_history(client, base_url, api_key, model,
                                             to_summarize, callback);

        // 替换: 删除 old messages，插入摘要（紧跟 system prompt 后面）
        messages.erase(messages.begin() + sys_count,
                       messages.begin() + sys_count + summarize_count);
        messages.insert(messages.begin() + sys_count, summary);

        LOG_INFO("Summarized %zu messages into ~%zu token summary (budget=%zu)",
                 summarize_count, context::estimate_tokens(summary.content), budget);
    }
}

/**
 * 摘要压缩 — 调 LLM 把旧消息压缩成一句话
 *
 * prompt 设计要点:
 *   - 要求中文输出，一句话总结
 *   - 不传 tools（轻量调用，避免递归工具触发）
 *   - 限制 max_tokens 为 300（输出足够简洁）
 */
Message ToolOrchestrator::summarize_history(
    LlmClient& client,
    const std::string& base_url,
    const std::string& api_key,
    const std::string& model,
    const std::vector<Message>& messages_to_summarize,
    OrchestratorCallback callback) {

    // 拼接待摘要的消息内容
    std::string conversation_text;
    for (const auto& msg : messages_to_summarize) {
        conversation_text += "[" + msg.role + "]: ";
        // 截断每条消息到 500 token，避免摘要 prompt 本身过大
        if (context::estimate_tokens(msg.content) > 500) {
            conversation_text += context::truncate_tool_result(msg.content, 500);
        } else {
            conversation_text += msg.content;
        }
        conversation_text += "\n\n";
    }

    // 构建摘要 prompt
    Message summary_prompt;
    summary_prompt.role = "user";
    summary_prompt.content =
        "请用一句中文总结以下对话历史的关键信息（做了什么、查到了什么、得出了什么结论）：\n\n"
        + conversation_text +
        "\n请直接给出总结，不要加前缀说明。";

    std::vector<Message> summary_msgs = {summary_prompt};
    std::vector<mcp::ToolDef> no_tools;  // 空 tools → 不会触发工具调用

    std::string summary_text;

    // 调 LLM（不带 tools，轻量调用）
    bool ok = client.chat_stream(base_url, api_key, model,
        summary_msgs, no_tools,
        [&](const std::string& delta, const std::vector<llm::ToolCall>& /*tc*/, bool finished) {
            if (!delta.empty()) summary_text += delta;
            (void)finished;
        });

    if (!ok || summary_text.empty()) {
        // 摘要失败 → 降级：手动拼接前几条消息的前几个字
        summary_text = "之前的对话包括: ";
        for (size_t i = 0; i < std::min(messages_to_summarize.size(), size_t(4)); ++i) {
            if (messages_to_summarize[i].role == "user") {
                summary_text += messages_to_summarize[i].content.substr(0, 80) + "; ";
            }
        }
    }

    // 构建 system 角色摘要消息
    Message result;
    result.role = "system";
    result.content = "[历史对话摘要] " + summary_text;
    return result;
}

} // namespace llm
