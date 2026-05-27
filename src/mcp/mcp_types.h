#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mcp {

// 服务器能力声明
struct ServerCapabilities {
    struct Tools {
        bool listChanged = false;
        nlohmann::json to_json() const {
            return {{"listChanged", listChanged}};
        }
    };
    Tools tools;

    nlohmann::json to_json() const {
        return {{"tools", tools.to_json()}};
    }
};

// MCP 文本内容块 (用于工具返回结果)
struct TextContent {
    std::string type = "text";
    std::string text;

    nlohmann::json to_json() const {
        return {{"type", type}, {"text", text}};
    }
};

// 工具定义 (用于 tools/list 响应)
struct ToolDef {
    std::string name;
    std::string description;
    nlohmann::json inputSchema;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["name"] = name;
        j["description"] = description;
        j["inputSchema"] = inputSchema;
        return j;
    }
};

// 工具调用结果
struct ToolCallResult {
    std::vector<TextContent> content;
    bool isError = false;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["isError"] = isError;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : content) {
            arr.push_back(c.to_json());
        }
        j["content"] = arr;
        return j;
    }
};

// tools/call 请求参数
struct ToolCallParams {
    std::string name;
    nlohmann::json arguments;

    static ToolCallParams from_json(const nlohmann::json& j) {
        ToolCallParams p;
        p.name = j.value("name", "");
        p.arguments = j.value("arguments", nlohmann::json::object());
        return p;
    }
};

// initialize 请求参数
struct InitializeRequestParams {
    std::string protocolVersion;
    nlohmann::json capabilities;
    nlohmann::json clientInfo;

    static InitializeRequestParams from_json(const nlohmann::json& j) {
        InitializeRequestParams p;
        p.protocolVersion = j.value("protocolVersion", "");
        p.capabilities = j.value("capabilities", nlohmann::json::object());
        p.clientInfo = j.value("clientInfo", nlohmann::json::object());
        return p;
    }
};

// initialize 响应结果
struct InitializeResult {
    std::string protocolVersion = "2024-11-05";
    ServerCapabilities capabilities;
    nlohmann::json serverInfo;

    nlohmann::json to_json() const {
        return {
            {"protocolVersion", protocolVersion},
            {"capabilities", capabilities.to_json()},
            {"serverInfo", serverInfo}
        };
    }
};

} // namespace mcp
