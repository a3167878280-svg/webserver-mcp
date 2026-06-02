/**
 * LLM 客户端实现 — 调用各大模型 API 的核心代码
 *
 * ═══════════ SSE 流式协议的通用模式 ═══════════
 *
 * 无论是 OpenAI 还是 Anthropic，流式 API 都是同一套 SSE 协议:
 *
 *   客户端 POST 请求 (设置 stream: true)
 *       ↓
 *   服务器返回 SSE 流:
 *     data: {"choices":[{"delta":{"content":"你"}}]}
 *     data: {"choices":[{"delta":{"content":"好"}}]}
 *     data: {"choices":[{"delta":{"content":"!"}}]}
 *     data: [DONE]
 *
 * 每行 "data: <json>\n" 是一个 chunk，客户端逐 chunk 解析。
 * 非流式场景下 LLM 一次性返回完整 JSON，流式则逐 token 返回。
 *
 * ═══════════ 为什么用 httplib 的流式回调? ═══════════
 *
 *   httplib 支持 content receiver callback — 不是等整个响应收完再处理，
 *   而是每收到一块数据就调一次回调。这样就可以实现"打字机"效果:
 *   收到一个 token → 解析 → 立即推送给浏览器 (SSE 转发)。
 */

#include "llm_client.h"
#include "../log/log.h"
#include "../common.h"
#include "httplib.h"
#include <sstream>
#include <algorithm>
#include <memory>
#include <type_traits>

namespace llm {

LlmClient::LlmClient() = default;

// ── URL 解析 ──────────────────────────────────────────────
struct UrlInfo {
    std::string host;
    std::string path;
    bool use_https = false;
    bool is_anthropic = false;   // 自动检测: url 含 "anthropic" 就发 Anthropic 格式
};

/**
 * 从 base_url 中拆出 host + path + https标记 + API类型
 *
 * 例: "https://api.openai.com/v1" →
 *     host="api.openai.com", path="/v1", use_https=true, is_anthropic=false
 */
static UrlInfo parse_url(const std::string& base_url) {
    UrlInfo info;
    std::string url = base_url;

    if (url.find("https://") == 0) { info.use_https = true; url = url.substr(8); }
    else if (url.find("http://") == 0) { url = url.substr(7); }

    size_t slash = url.find('/');
    if (slash != std::string::npos) {
        info.host = url.substr(0, slash);
        info.path = url.substr(slash);
    } else {
        info.host = url;
        info.path = "";
    }

    // 自动检测: url 里含 "anthropic" → Anthropic 格式，否则 → OpenAI 格式
    std::string lower = base_url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    info.is_anthropic = (lower.find("anthropic") != std::string::npos);

    return info;
}

// ── 公开接口 ──────────────────────────────────────────────
/**
 * 流式聊天入口 — 自动路由到 OpenAI 或 Anthropic 实现
 *
 * 根据 parse_url 检测到的 API 类型，自动补全 endpoint 路径:
 *   OpenAI:   /v1/chat/completions
 *   Anthropic: /v1/messages
 */
bool LlmClient::chat_stream(
    const std::string& base_url, const std::string& api_key,
    const std::string& model, const std::vector<Message>& messages,
    const std::vector<mcp::ToolDef>& tools, StreamCallback callback) {

    auto u = parse_url(base_url);

    if (u.is_anthropic) {
        // Anthropic endpoint: /v1/messages
        if (u.path.empty() || u.path == "/") {
            u.path = "/v1/messages";
        } else if (u.path.find("/v1/messages") == std::string::npos) {
            if (u.path.back() == '/') u.path.pop_back();
            u.path += "/v1/messages";
        }
        return chat_anthropic(u.host, u.path, api_key, model, messages, tools, callback);
    } else {
        // OpenAI / DeepSeek / Ollama endpoint: /v1/chat/completions
        if (u.path.empty() || u.path == "/") {
            u.path = "/v1/chat/completions";
        } else if (u.path.find("/chat/completions") == std::string::npos) {
            if (u.path.back() == '/') u.path.pop_back();
            u.path += "/chat/completions";
        }
        return chat_openai(u.host, u.path, api_key, model, messages, tools, callback);
    }
}

// ── OpenAI API ────────────────────────────────────────────
/**
 * 调 OpenAI 兼容 API (OpenAI / DeepSeek / Ollama 等)
 *
 * 请求格式:
 *   POST /v1/chat/completions
 *   {
 *     "model": "gpt-4o",
 *     "stream": true,
 *     "messages": [{role, content}, ...],
 *     "tools": [{type:"function", function:{name, description, parameters}}, ...],
 *     "tool_choice": "auto"   ← LLM 自己决定要不要调工具
 *   }
 *
 * 响应是 SSE 流:
 *   data: {"choices":[{"delta":{"content":"你"}}]}
 *   data: {"choices":[{"delta":{"tool_calls":[{"id":"call_1","function":{"name":"q","arguments":"{\"c"}}]}]}]}
 *   data: [DONE]
 *
 * 关键: tool_calls 的 arguments 是增量 JSON 片段, 需要拼起来才能得到完整 JSON
 */
bool LlmClient::chat_openai(
    const std::string& host, const std::string& path,
    const std::string& api_key, const std::string& model,
    const std::vector<Message>& messages,
    const std::vector<mcp::ToolDef>& tools,
    StreamCallback callback) {

    // ── 构建请求体 (在 lambda 之前) ──
    std::vector<ToolCall> accumulated_tools;
    nlohmann::json req_body;
    req_body["model"] = model;
    // 本地 llama.cpp 不支持 stream + tools 同时用，有工具时走非流式
    req_body["stream"] = tools.empty();
    nlohmann::json msgs_arr = nlohmann::json::array();
    for (const auto& msg : messages) {
        nlohmann::json m;
        m["role"] = msg.role;
        if (!msg.content.empty()) m["content"] = msg.content;
        if (!msg.tool_call_id.empty()) m["tool_call_id"] = msg.tool_call_id;
        if (!msg.tool_calls.is_null()) m["tool_calls"] = msg.tool_calls;
        msgs_arr.push_back(m);
    }
    req_body["messages"] = msgs_arr;
    if (!tools.empty()) {
        nlohmann::json tools_arr = nlohmann::json::array();
        for (const auto& tool : tools) {
            nlohmann::json t;
            t["type"] = "function";
            t["function"]["name"] = tool.name;
            t["function"]["description"] = tool.description;
            t["function"]["parameters"] = tool.inputSchema;
            tools_arr.push_back(t);
        }
        req_body["tools"] = tools_arr;
        req_body["tool_choice"] = "auto";
    }
    std::string body = req_body.dump();
    httplib::Headers headers = {
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + api_key}
    };

    // SSE 流回调 (捕获所有局部变量)
    auto sse_handler = [&](const char* data, size_t len) -> bool {
        if (len == 0) return true;
        std::string chunk(data, len);
        std::istringstream stream(chunk);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty() || line[0] == ':') continue;
            if (line.rfind("data: ", 0) != 0) continue;
            std::string json_str = line.substr(6);
            if (json_str == "[DONE]") { callback("", accumulated_tools, true); return true; }
            std::string delta; std::vector<ToolCall> tc; bool finished = false;
            parse_openai_chunk(json_str, delta, tc, finished);
            if (!delta.empty()) callback(delta, {}, false);
            for (auto& t : tc) {
                bool found = false;
                for (auto& at : accumulated_tools) {
                    if (at.id == t.id) { at.function_name += t.function_name; at.arguments += t.arguments; found = true; break; }
                }
                if (!found && !t.id.empty()) accumulated_tools.push_back(t);
            }
            if (finished && !accumulated_tools.empty()) callback("", accumulated_tools, true);
        }
        return true;
    };

    // 选择 HTTP 或 HTTPS 客户端
    httplib::Result res;
    if (host.find("localhost") != std::string::npos || host.find("127.0.0.1") != std::string::npos || host.find("0.0.0.0") != std::string::npos) {
        httplib::Client cli("http://" + host);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(120);
        cli.set_write_timeout(30);
        if (tools.empty()) {
            // 流式: 用 content receiver
            res = cli.Post(path, headers, body, "application/json", sse_handler);
        } else {
            // 非流式: 不用 receiver, 直接读 body
            res = cli.Post(path, headers, body, "application/json");
        }
    } else {
        httplib::SSLClient cli(host);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(120);
        cli.set_write_timeout(30);
        cli.enable_server_certificate_verification(false);
        if (tools.empty()) {
            res = cli.Post(path, headers, body, "application/json", sse_handler);
        } else {
            res = cli.Post(path, headers, body, "application/json");
        }
    }

    if (!res) {
        LOG_ERROR("OpenAI API error: %s", httplib::to_string(res.error()).c_str());
        return false;
    }
    if (res->status != 200) {
        LOG_ERROR("OpenAI API status %d: %.500s", res->status, res->body.c_str());
        return false;
    }

    // 非流式模式: 解析完整的 response body
    if (!tools.empty() && !res->body.empty()) {
        try {
            auto j = nlohmann::json::parse(res->body);
            if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
                auto& choice = j["choices"][0];
                auto& msg = choice["message"];
                // 提取文本内容 (可能为 null)
                if (msg.contains("content") && !msg["content"].is_null()) {
                    callback(msg["content"].get<std::string>(), {}, false);
                }
                // 提取工具调用
                if (msg.contains("tool_calls") && msg["tool_calls"].is_array() && !msg["tool_calls"].empty()) {
                    std::vector<ToolCall> tcs;
                    for (auto& tc : msg["tool_calls"]) {
                        ToolCall t;
                        t.id = tc.value("id", "");
                        if (tc.contains("function")) {
                            t.function_name = tc["function"].value("name", "");
                            t.arguments = tc["function"].value("arguments", "");
                        }
                        tcs.push_back(t);
                    }
                    callback("", tcs, true);
                }
                else if (msg.contains("content") && !msg["content"].is_null()) {
                    // 只有文本，无工具调用
                    callback("", {}, true);
                }
            }
        } catch (...) {
            fprintf(stderr, "DEBUG L: failed to parse non-stream response\n");
        }
    }

    return true;
}

// ── Anthropic API ─────────────────────────────────────────
/**
 * 调 Anthropic Messages API (Claude 系列)
 *
 * Anthropic 和 OpenAI 的核心区别:
 *
 *   OpenAI:  token 是 {delta: {content: "你"}}
 *   Anthropic: token 是 {type: "content_block_delta", delta: {type: "text_delta", text: "你"}}
 *
 * Anthropic 使用基于 content_block 的事件模型:
 *   content_block_start  → 一个新的内容块开始 (文本块 / tool_use 块 / thinking 块)
 *   content_block_delta  → 内容块的增量数据
 *   content_block_stop   → 内容块结束
 *   message_delta        → 消息级别信息 (stop_reason)
 *   message_stop         → 整个消息结束
 *
 * 关于工具调用的特殊处理:
 *   Anthropic 原生支持 tool_use content block，但这里做了简化:
 *   把 tool role 消息转成普通 user 消息，把 assistant tool_calls 转成文本。
 *   这样做的原因是 DeepSeek 等兼容 Anthropic 格式的 API 对标准 tool_result 支持不完整。
 */
bool LlmClient::chat_anthropic(
    const std::string& host, const std::string& path,
    const std::string& api_key, const std::string& model,
    const std::vector<Message>& messages,
    const std::vector<mcp::ToolDef>& tools,
    StreamCallback callback) {

    m_tool_index_map.clear();  // 每轮对话重置 index→id 映射

    httplib::SSLClient cli(host);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(120);
    cli.set_write_timeout(30);
    cli.enable_server_certificate_verification(false);

    // ── 构建 Anthropic 格式请求体 ──
    nlohmann::json req_body;
    req_body["model"] = model;
    req_body["max_tokens"] = 8192;
    req_body["stream"] = true;

    /**
     * Anthropic messages 数组 — 与 OpenAI 格式不同:
     *   - system 不在 messages 里，而是顶层字段
     *   - tool role → 简化: 拼成 user 消息 (兼容 DeepSeek)
     *   - assistant 的 tool_calls → 简化: 拼成文本描述
     */
    nlohmann::json msgs_arr = nlohmann::json::array();
    for (const auto& msg : messages) {
        if (msg.role == "system") {
            req_body["system"] = msg.content;  // system 是顶层字段
            continue;
        }

        if (msg.role == "tool") {
            // 工具执行结果 → 包装成 user 消息喂给 LLM
            std::string result_text = msg.content;
            try {
                auto rj = nlohmann::json::parse(msg.content);
                if (rj.contains("content") && rj["content"].is_array() && !rj["content"].empty()) {
                    result_text = rj["content"][0].value("text", msg.content);
                }
            } catch (...) {}
            nlohmann::json m;
            m["role"] = "user";
            m["content"] = "[工具返回] " + result_text +
                           "\n请基于以上工具返回的信息回答用户的问题。";
            msgs_arr.push_back(m);
        } else if (!msg.tool_calls.is_null() && msg.tool_calls.is_array() && !msg.tool_calls.empty()) {
            // 上一轮 assistant 曾调用工具 → 转成文本说明 (简化)
            std::string call_info = msg.content;
            for (auto& tc : msg.tool_calls) {
                call_info += "\n[调用工具: " + tc["function"]["name"].get<std::string>() +
                             ", 参数: " + tc["function"]["arguments"].get<std::string>() + "]";
            }
            nlohmann::json m;
            m["role"] = "assistant";
            m["content"] = call_info;
            msgs_arr.push_back(m);
        } else {
            nlohmann::json m;
            m["role"] = msg.role;
            m["content"] = msg.content;
            msgs_arr.push_back(m);
        }
    }
    req_body["messages"] = msgs_arr;

    // 工具定义 → Anthropic 格式: {name, description, input_schema}
    if (!tools.empty()) {
        nlohmann::json tools_arr = nlohmann::json::array();
        for (const auto& tool : tools) {
            nlohmann::json t;
            t["name"] = tool.name;
            t["description"] = tool.description;
            t["input_schema"] = tool.inputSchema;
            tools_arr.push_back(t);
        }
        req_body["tools"] = tools_arr;
    }

    std::string body = req_body.dump();
    httplib::Headers headers = {
        {"Content-Type", "application/json"},
        {"x-api-key", api_key},                       // Anthropic 用 x-api-key，不是 Bearer
        {"anthropic-version", "2023-06-01"}            // API 版本头
    };

    std::vector<ToolCall> accumulated_tools;

    // SSE 流处理 — 和 OpenAI 模式相同的套路
    auto res = cli.Post(path, headers, body, "application/json",
        [&](const char* data, size_t len) {
            if (len == 0) return true;
            std::string chunk(data, len);
            std::istringstream stream(chunk);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.empty() || line[0] == ':') continue;
                if (line.rfind("data: ", 0) != 0) continue;
                std::string json_str = line.substr(6);
                if (json_str == "[DONE]") { callback("", accumulated_tools, true); return true; }

                std::string delta; std::vector<ToolCall> tc; bool finished = false;
                parse_anthropic_chunk(json_str, delta, tc, finished);

                // 回退: 如果 Anthropic 解析器没产出，尝试 OpenAI 解析器
                // (有些 API 声称是 Anthropic 兼容但实际返回 OpenAI 格式)
                bool had_finished = finished;
                if (delta.empty() && tc.empty() && !finished) {
                    parse_openai_chunk(json_str, delta, tc, finished);
                }
                if (had_finished && finished != had_finished) finished = had_finished;

                if (!delta.empty()) callback(delta, {}, false);
                for (auto& t : tc) {
                    bool found = false;
                    for (auto& at : accumulated_tools) {
                        if (at.id == t.id) { at.function_name += t.function_name; at.arguments += t.arguments; found = true; break; }
                    }
                    if (!found && !t.id.empty()) accumulated_tools.push_back(t);
                }
                if (finished && !accumulated_tools.empty()) {
                    callback("", accumulated_tools, true);
                }
            }
            return true;
        });

    if (!res) {
        fprintf(stderr, "DEBUG anthropic: HTTP error: %s\n", httplib::to_string(res.error()).c_str());
        return false;
    }
    fprintf(stderr, "DEBUG anthropic: HTTP status=%d\n", res->status);
    if (res->status != 200) {
        fprintf(stderr, "DEBUG anthropic error body: %s\n", res->body.c_str());
        fprintf(stderr, "DEBUG anthropic request body: %s\n", body.c_str());
        return false;
    }
    return true;
}

// ── OpenAI SSE 解析 ─────────────────────────────────────
/**
 * 解析 OpenAI SSE chunk — 从一个 "data: {json}" 行中提取信息
 *
 * OpenAI 的 chunk 结构:
 *   {
 *     "choices": [{
 *       "delta": {
 *         "content": "你好",                          ← 普通文本
 *         "tool_calls": [{                            ← 或工具调用 (与 content 互斥)
 *           "index": 0,
 *           "id": "call_abc123",
 *           "function": {"name": "query_weather", "arguments": "{\"city\":\"Beijing\"}"}
 *         }]
 *       },
 *       "finish_reason": "stop" | "tool_calls" | null  ← 非空=这轮结束
 *     }]
 *   }
 *
 * 注意: 流式场景下 tool_calls 的 arguments 是增量 JSON 片段:
 *   chunk 1: arguments = "{\"city\""
 *   chunk 2: arguments = ":\"Beijing\"}"
 *   需要在上层按 id 拼起来
 */
void LlmClient::parse_openai_chunk(const std::string& data,
    std::string& delta_out, std::vector<ToolCall>& tc_out, bool& finish_out) {
    delta_out.clear(); tc_out.clear(); finish_out = false;
    try {
        auto j = nlohmann::json::parse(data);
        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;
        auto& choice = j["choices"][0];

        // finish_reason 非空 → 这轮对话结束
        if (choice.value("finish_reason", "") != "") finish_out = true;

        if (choice.contains("delta")) {
            auto& delta = choice["delta"];

            // 提取文本增量
            if (delta.contains("content") && delta["content"].is_string())
                delta_out = delta["content"].get<std::string>();

            // 提取工具调用增量 (每个 chunk 可能出现多个 tool_call 分片)
            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                for (auto& tc : delta["tool_calls"]) {
                    ToolCall t;
                    // OpenAI 用 index 标记同一个 tool_call 的多个分片
                    if (tc.contains("id") && !tc["id"].is_null())
                        t.id = tc["id"].get<std::string>();
                    if (tc.value("index", -1) >= 0 && tc["index"].is_number())
                        t.id = std::to_string(tc["index"].get<int>());
                    if (tc.contains("function")) {
                        auto& fn = tc["function"];
                        if (fn.contains("name")) t.function_name = fn["name"].get<std::string>();
                        if (fn.contains("arguments")) t.arguments = fn["arguments"].get<std::string>();
                    }
                    tc_out.push_back(t);
                }
            }
        }
    } catch (...) {}  // JSON 解析失败 → 忽略 (可能是 heartbeat 或格式异常)
}

// ── Anthropic SSE 解析 ──────────────────────────────────
/**
 * 解析 Anthropic SSE chunk
 *
 * Anthropic 的事件类型 (比 OpenAI 更细粒度):
 *
 *   message_start         → {"type":"message_start", "message":{...}}
 *   content_block_start   → {"type":"content_block_start", "index":0,
 *                            "content_block":{"type":"tool_use","id":"toolu_xxx","name":"f"}}
 *   content_block_delta   → {"type":"content_block_delta", "index":0,
 *                            "delta":{"type":"text_delta","text":"你好"}}
 *                            或 {"delta":{"type":"input_json_delta","partial_json":"{\"c"}}
 *   content_block_stop    → {"type":"content_block_stop", "index":0}
 *   message_delta         → {"type":"message_delta", "delta":{"stop_reason":"end_turn"}}
 *   message_stop          → {"type":"message_stop"}
 *
 * 关于 index→id 映射:
 *   Anthropic 在 content_block_start 中给出真实的 tool_use_id，
 *   但后续的 input_json_delta 只带 index 不带 id。
 *   所以需要在 content_block_start 时记录 m_tool_index_map[index] = id，
 *   在 input_json_delta 时通过 index 反查 id。
 */
void LlmClient::parse_anthropic_chunk(const std::string& data,
    std::string& delta_out, std::vector<ToolCall>& tc_out, bool& finish_out) {
    delta_out.clear(); tc_out.clear(); finish_out = false;
    try {
        auto j = nlohmann::json::parse(data);
        std::string type = j.value("type", "");

        if (type == "message_start") {
            m_tool_index_map.clear();  // 新消息 → 清空映射

        } else if (type == "content_block_delta") {
            auto& d = j["delta"];
            std::string dt = d.value("type", "");
            if (dt == "text_delta") {
                // Claude 的普通文本输出
                delta_out = d.value("text", "");
            } else if (dt == "thinking_delta") {
                // DeepSeek / Claude thinking 扩展 — 思考过程
                delta_out = d.value("thinking", "");
            } else if (dt == "input_json_delta") {
                // 工具参数的增量 JSON 分片 → 通过 index 找到 tool_use_id
                ToolCall t;
                if (j.contains("index")) {
                    int idx = j["index"].get<int>();
                    auto mit = m_tool_index_map.find(idx);
                    t.id = (mit != m_tool_index_map.end()) ? mit->second : std::to_string(idx);
                }
                t.arguments = d.value("partial_json", "");
                tc_out.push_back(t);
            }
            // signature_delta — DeepSeek 签名，忽略

        } else if (type == "content_block_start") {
            auto& cb = j["content_block"];
            if (cb.value("type", "") == "tool_use") {
                // 新的工具调用块 → 记录 id, 建立 index→id 映射
                ToolCall t;
                t.id = cb.value("id", "");
                t.function_name = cb.value("name", "");
                tc_out.push_back(t);
                if (j.contains("index")) {
                    int idx = j["index"].get<int>();
                    m_tool_index_map[idx] = t.id;
                }
            }

        } else if (type == "content_block_stop") {
            finish_out = true;
        } else if (type == "message_delta") {
            if (j.contains("delta") && j["delta"].contains("stop_reason"))
                finish_out = true;
        } else if (type == "message_stop") {
            finish_out = true;
            m_tool_index_map.clear();  // 消息结束 → 清空映射
        }
    } catch (...) {}
}

} // namespace llm
