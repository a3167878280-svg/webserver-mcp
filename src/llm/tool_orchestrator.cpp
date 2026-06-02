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
#include <algorithm>

namespace llm {

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
            "你是一个运行在 Linux 服务器上的 AI 助手，拥有执行命令、读写文件、审查代码等能力。\n\n"
            "## 核心规则 (必须遵守):\n"
            "1. **先探索再操作**: 不确定文件路径时先用 file_list，不确定进程是否运行时先用 shell_exec\n"
            "2. **每个工具调用都要有目的**: 调用工具前在心里明确\"我要通过这个工具得到什么信息\"\n"
            "3. **拿到结果后必须总结**: 工具返回数据后，用自然语言告诉用户结果，不要只展示原始输出\n"
            "4. **多步骤任务要逐步执行**: 先 ls 找到文件 → 再 cat 读取 → 最后分析，不要一步到位猜测\n"
            "5. **回答要直接明确**: 用户问\"有没有运行\"→ 回答\"正在运行\"或\"没有运行\"，不要模棱两可\n"
            "6. **大文件用 grep 搜索**: 文件超过 100 行时，用 grep 定位关键内容，不要整篇通读\n"
            "7. **用中文回复**: 除非用户要求，否则始终用中文回答\n\n"
            "## 可用能力:\n"
            "- 读写文件 (file_read, file_list)\n"
            "- 执行命令 (shell_exec, shell_exec_bg)\n"
            "- 搜索文件内容 (grep_file)\n"
            "- 检查进程 (process_check)\n"
            "- 查询天气 (query_weather)\n"
            "- 代码审查 (code_review, code_stats)\n"
            "- B站信息 (bilibili_*)";
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
        tools.erase(std::remove_if(tools.begin(), tools.end(),
            [&](const mcp::ToolDef& t) {
                return std::find(disabled_tools.begin(), disabled_tools.end(), t.name) != disabled_tools.end();
            }), tools.end());
    }

    // ── 步骤 4: Agent 循环 ──
    LlmClient client;
    int round = 0;

    while (round < MAX_ROUNDS) {
        round++;

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
            callback("error", "Failed to connect to LLM API");
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
        callback("done", full_content);
        return;
    }

    // 超过 10 轮 → 强制终止
    callback("error", "Max tool calling rounds reached");
}

} // namespace llm
