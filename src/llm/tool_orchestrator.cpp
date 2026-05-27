#include "tool_orchestrator.h"
#include "../mcp/jsonrpc.h"
#include "../log/log.h"
#include "../common.h"
#include <algorithm>

namespace llm {

ToolOrchestrator::ToolOrchestrator(mcp::McpHandler& handler)
    : m_handler(handler) {}

void ToolOrchestrator::process(
    const std::string& user_message,
    const std::string& api_key,
    const std::string& base_url,
    const std::string& model,
    const std::vector<Message>& history,
    const std::vector<std::string>& disabled_tools,
    OrchestratorCallback callback) {

    // 构建初始 messages
    std::vector<Message> messages = history;
    Message user_msg;
    user_msg.role = "user";
    user_msg.content = user_message;
    messages.push_back(user_msg);

    // 获取可用工具
    nlohmann::json tools_result;
    {
        mcp::JsonRpcRequest req;
        req.jsonrpc = "2.0";
        req.method = "tools/list";
        req.id = "internal";  // 必须有 id，否则被当通知不返回响应
        auto resp = m_handler.handle(req);
        if (resp.has_value()) {
            tools_result = resp->result;
        }
    }

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

    // 过滤禁用的工具
    if (!disabled_tools.empty()) {
        tools.erase(std::remove_if(tools.begin(), tools.end(),
            [&](const mcp::ToolDef& t) {
                return std::find(disabled_tools.begin(), disabled_tools.end(), t.name) != disabled_tools.end();
            }), tools.end());
    }

    LlmClient client;
    int round = 0;

    while (round < MAX_ROUNDS) {
        round++;

        std::string full_content;
        std::vector<ToolCall> final_tool_calls;
        bool llm_finished = false;

        bool ok = client.chat_stream(base_url, api_key, model,
            messages, tools,
            [&](const std::string& delta, const std::vector<ToolCall>& tc, bool finished) {
                if (!delta.empty()) {
                    full_content += delta;
                    callback("delta", delta);
                }
                if (finished && !tc.empty()) {
                    final_tool_calls = tc;
                }
                if (finished) llm_finished = true;
            });

        if (!ok) {
            callback("error", "Failed to connect to LLM API");
            return;
        }

        // LLM 要求调用工具
        if (!final_tool_calls.empty()) {
            // 构建 assistant message (含 tool_calls)
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

            // 执行每个工具调用
            for (auto& tc : final_tool_calls) {
                callback("tool_start",
                    "{\"tool\":\"" + tc.function_name + "\",\"args\":" + tc.arguments + "}");

                // 解析 arguments
                nlohmann::json args;
                try {
                    args = nlohmann::json::parse(tc.arguments);
                } catch (...) {
                    args = nlohmann::json::object();
                }

                // 调用 MCP tools/call
                mcp::JsonRpcRequest req;
                req.jsonrpc = "2.0";
                req.method = "tools/call";
                req.id = "internal";
                req.params = {
                    {"name", tc.function_name},
                    {"arguments", args}
                };

                std::string tool_result_text;
                auto resp = m_handler.handle(req);
                if (resp.has_value()) {
                    tool_result_text = resp->result.dump();
                } else {
                    tool_result_text = "{\"isError\":true,\"content\":[{\"text\":\"Tool execution failed\"}]}";
                }

                callback("tool_result", tool_result_text);

                // 添加 tool result 到 messages
                Message tool_msg;
                tool_msg.role = "tool";
                tool_msg.tool_call_id = tc.id;
                tool_msg.content = tool_result_text;
                messages.push_back(tool_msg);
            }
            continue;  // 继续循环，让 LLM 处理工具结果  // 继续循环，让 LLM 处理工具结果
        }

        // LLM 返回最终文本
        callback("done", full_content);
        return;
    }

    callback("error", "Max tool calling rounds reached");
}

} // namespace llm
