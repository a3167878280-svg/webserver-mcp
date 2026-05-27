#include "llm_client.h"
#include "../log/log.h"
#include "../common.h"
#include "httplib.h"
#include <sstream>
#include <algorithm>

namespace llm {

LlmClient::LlmClient() = default;

// ── URL 解析 ──────────────────────────────────────────────
struct UrlInfo {
    std::string host;
    std::string path;
    bool use_https = false;
    bool is_anthropic = false;   // Anthropic 格式 vs OpenAI 格式
};

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

    // 自动检测 API 类型
    std::string lower = base_url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    info.is_anthropic = (lower.find("anthropic") != std::string::npos);

    return info;
}

// ── 公开接口 ──────────────────────────────────────────────
bool LlmClient::chat_stream(
    const std::string& base_url, const std::string& api_key,
    const std::string& model, const std::vector<Message>& messages,
    const std::vector<mcp::ToolDef>& tools, StreamCallback callback) {

    auto u = parse_url(base_url);

    if (u.is_anthropic) {
        // 如果 path 不以 /v1/messages 结尾，追加
        if (u.path.empty() || u.path == "/") {
            u.path = "/v1/messages";
        } else if (u.path.find("/v1/messages") == std::string::npos) {
            if (u.path.back() == '/') u.path.pop_back();
            u.path += "/v1/messages";
        }
        return chat_anthropic(u.host, u.path, api_key, model, messages, tools, callback);
    } else {
        // OpenAI: 追加 /chat/completions
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
bool LlmClient::chat_openai(
    const std::string& host, const std::string& path,
    const std::string& api_key, const std::string& model,
    const std::vector<Message>& messages,
    const std::vector<mcp::ToolDef>& tools,
    StreamCallback callback) {

    httplib::SSLClient cli(host);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(120);
    cli.set_write_timeout(30);
    cli.enable_server_certificate_verification(false);

    nlohmann::json req_body;
    req_body["model"] = model;
    req_body["stream"] = true;

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

    // host and path already set above

    std::vector<ToolCall> accumulated_tools;

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
        });

    if (!res) {
        LOG_ERROR("OpenAI API error: %s", httplib::to_string(res.error()).c_str());
        return false;
    }
    if (res->status != 200) {
        LOG_ERROR("OpenAI API status %d: %.500s", res->status, res->body.c_str());
        return false;
    }
    return true;
}

// ── Anthropic API ─────────────────────────────────────────
bool LlmClient::chat_anthropic(
    const std::string& host, const std::string& path,
    const std::string& api_key, const std::string& model,
    const std::vector<Message>& messages,
    const std::vector<mcp::ToolDef>& tools,
    StreamCallback callback) {

    m_tool_index_map.clear();

    httplib::SSLClient cli(host);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(120);
    cli.set_write_timeout(30);
    cli.enable_server_certificate_verification(false);

    nlohmann::json req_body;
    req_body["model"] = model;
    req_body["max_tokens"] = 8192;
    req_body["stream"] = true;

    // Anthropic messages 格式转换
    nlohmann::json msgs_arr = nlohmann::json::array();
    for (const auto& msg : messages) {
        if (msg.role == "system") {
            req_body["system"] = msg.content;
            continue;
        }

        if (msg.role == "tool") {
            // 工具结果 → 简化处理: 作为纯文本 user 消息注入
            // DeepSeek 对标准 tool_result 支持不完整
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
            // 助手工具调用 → 简化: 合成文本说明
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
        {"x-api-key", api_key},
        {"anthropic-version", "2023-06-01"}
    };

    // Anthropic request sent

    std::vector<ToolCall> accumulated_tools;
    std::string current_tool_id;
    std::string current_tool_name;

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
void LlmClient::parse_openai_chunk(const std::string& data,
    std::string& delta_out, std::vector<ToolCall>& tc_out, bool& finish_out) {
    delta_out.clear(); tc_out.clear(); finish_out = false;
    try {
        auto j = nlohmann::json::parse(data);
        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;
        auto& choice = j["choices"][0];
        if (choice.value("finish_reason", "") != "") finish_out = true;
        if (choice.contains("delta")) {
            auto& delta = choice["delta"];
            if (delta.contains("content") && delta["content"].is_string())
                delta_out = delta["content"].get<std::string>();
            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                for (auto& tc : delta["tool_calls"]) {
                    ToolCall t;
                    if (tc.contains("id") && !tc["id"].is_null()) t.id = tc["id"].get<std::string>();
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
    } catch (...) {}
}

// ── Anthropic SSE 解析 ──────────────────────────────────
void LlmClient::parse_anthropic_chunk(const std::string& data,
    std::string& delta_out, std::vector<ToolCall>& tc_out, bool& finish_out) {
    delta_out.clear(); tc_out.clear(); finish_out = false;
    try {
        auto j = nlohmann::json::parse(data);
        std::string type = j.value("type", "");

        if (type == "message_start") {
            m_tool_index_map.clear();  // 新消息开始
        } else if (type == "content_block_delta") {
            auto& d = j["delta"];
            std::string dt = d.value("type", "");
            if (dt == "text_delta") {
                delta_out = d.value("text", "");
            } else if (dt == "thinking_delta") {
                // DeepSeek thinking 模式：思考内容也当文本输出
                delta_out = d.value("thinking", "");
            } else if (dt == "input_json_delta") {
                // 工具参数分片: 通过 index 映射获取真实的 tool_use_id
                ToolCall t;
                if (j.contains("index")) {
                    int idx = j["index"].get<int>();
                    auto mit = m_tool_index_map.find(idx);
                    t.id = (mit != m_tool_index_map.end()) ? mit->second : std::to_string(idx);
                }
                t.arguments = d.value("partial_json", "");
                tc_out.push_back(t);
            } else if (dt == "signature_delta") {
                // DeepSeek 签名信息，忽略
            }
        } else if (type == "content_block_start") {
            auto& cb = j["content_block"];
            if (cb.value("type", "") == "tool_use") {
                ToolCall t;
                t.id = cb.value("id", "");  // 使用 LLM 分配的真实 tool_use_id
                t.function_name = cb.value("name", "");
                tc_out.push_back(t);
                // 记录 index→id 映射 (供 input_json_delta 使用)
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
            m_tool_index_map.clear();  // 消息结束，清空映射
        }
    } catch (...) {}
}

} // namespace llm
