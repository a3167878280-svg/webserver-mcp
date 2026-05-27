#include "llm_client.h"
#include "../log/log.h"
#include "../common.h"
#include "httplib.h"
#include <sstream>

namespace llm {

LlmClient::LlmClient() = default;

bool LlmClient::chat_stream(
    const std::string& base_url,
    const std::string& api_key,
    const std::string& model,
    const std::vector<Message>& messages,
    const std::vector<mcp::ToolDef>& tools,
    StreamCallback callback) {

    // HTTPS 暂不支持 (需 OpenSSL 3.x dev headers + 重新编译)
    if (base_url.find("https://") == 0) {
        LOG_ERROR("HTTPS is not supported in current build. Use an HTTP endpoint (e.g. Ollama on localhost:11434)");
        return false;
    }

    std::string host, path_prefix;
    std::string url = base_url;
    if (url.find("http://") == 0) {
        url = url.substr(7);
    }

    size_t slash = url.find('/');
    if (slash != std::string::npos) {
        host = url.substr(0, slash);
        path_prefix = url.substr(slash);
    } else {
        host = url;
        path_prefix = "";
    }

    httplib::Client cli(host);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(120);
    cli.set_write_timeout(30);

    // 构建请求体
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

    // 工具定义
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

    std::string path = path_prefix + "/chat/completions";

    // 累积的 tool_calls (跨多个 delta)
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
                if (json_str == "[DONE]") {
                    callback("", accumulated_tools, true);
                    return true;
                }

                std::string delta;
                std::vector<ToolCall> tc;
                bool finished = false;
                parse_chunk(json_str, delta, tc, finished);

                if (!delta.empty()) {
                    callback(delta, {}, false);
                }

                // 累积 tool_calls
                for (auto& t : tc) {
                    // 按 index 合并
                    bool found = false;
                    for (auto& at : accumulated_tools) {
                        if (at.id == t.id) {
                            at.function_name += t.function_name;
                            at.arguments += t.arguments;
                            found = true;
                            break;
                        }
                    }
                    if (!found && !t.id.empty()) {
                        accumulated_tools.push_back(t);
                    }
                }

                if (finished && !accumulated_tools.empty()) {
                    callback("", accumulated_tools, true);
                }
            }
            return true;
        });

    if (!res) {
        LOG_ERROR("LLM API request failed: %s", httplib::to_string(res.error()).c_str());
        return false;
    }
    if (res->status != 200) {
        LOG_ERROR("LLM API error %d: %s", res->status, res->body.c_str());
        return false;
    }

    return true;
}

void LlmClient::parse_chunk(const std::string& data,
                             std::string& delta_out,
                             std::vector<ToolCall>& tc_out,
                             bool& finish_out) {
    delta_out.clear();
    tc_out.clear();
    finish_out = false;

    try {
        auto j = nlohmann::json::parse(data);
        if (!j.contains("choices")) return;
        auto& choices = j["choices"];
        if (!choices.is_array() || choices.empty()) return;

        auto& choice = choices[0];
        if (choice.value("finish_reason", "") != "") {
            finish_out = true;
        }

        if (choice.contains("delta")) {
            auto& delta = choice["delta"];

            if (delta.contains("content") && delta["content"].is_string()) {
                delta_out = delta["content"].get<std::string>();
            }

            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                for (auto& tc : delta["tool_calls"]) {
                    ToolCall t;
                    if (tc.contains("id") && !tc["id"].is_null()) {
                        t.id = tc["id"].get<std::string>();
                    }
                    if (tc.value("index", -1) >= 0 && tc["index"].is_number()) {
                        t.id = std::to_string(tc["index"].get<int>());
                    }
                    if (tc.contains("function")) {
                        auto& fn = tc["function"];
                        if (fn.contains("name")) {
                            t.function_name = fn["name"].get<std::string>();
                        }
                        if (fn.contains("arguments")) {
                            t.arguments = fn["arguments"].get<std::string>();
                        }
                    }
                    tc_out.push_back(t);
                }
            }
        }
    } catch (const std::exception&) {
        // 忽略解析错误
    }
}

} // namespace llm
