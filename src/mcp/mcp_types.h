/**
 * MCP 协议专用类型定义
 *
 * 这些类型位于 JSON-RPC 之上，定义了 MCP 协议的语义:
 *   - 握手 (initialize) 的请求/响应
 *   - 工具定义 (ToolDef)、资源定义 (ResourceDef)
 *   - 工具调用 (tools/call)、资源读取 (resources/read)
 *
 * MCP 三大能力:
 *   Tools     — "做一件事" (动词), LLM 主动调用
 *   Resources — "读一个东西" (名词), 服务器被动暴露，客户端浏览发现
 *   Prompts   — 预定义提示模板 (未实现)
 */

#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mcp {

/**
 * 服务器能力声明 — 告诉客户端"我能做什么"
 */
struct ServerCapabilities {
    struct Tools {
        bool listChanged = false;
        nlohmann::json to_json() const {
            return {{"listChanged", listChanged}};
        }
    };
    Tools tools;

    struct Resources {
        bool subscribe = false;       // 是否支持资源变更订阅
        bool listChanged = false;
        nlohmann::json to_json() const {
            return {{"subscribe", subscribe}, {"listChanged", listChanged}};
        }
    };
    Resources resources;

    nlohmann::json to_json() const {
        return {
            {"tools", tools.to_json()},
            {"resources", resources.to_json()}
        };
    }
};

/**
 * MCP 文本内容块 — 通用的内容载体
 * 用于 Tool 返回结果和 Resource 读取结果
 */
struct TextContent {
    std::string type = "text";
    std::string text;

    nlohmann::json to_json() const {
        return {{"type", type}, {"text", text}};
    }
};

// ═══════════════════════════════════════════════════════════════
// Tools 相关类型
// ═══════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════
// Resources 相关类型 (新增)
// ═══════════════════════════════════════════════════════════════

/**
 * 资源定义 — 描述一个可被客户端/LLM 读取的数据源
 *
 * 示例:
 *   ResourceDef {
 *     uri: "file:///tmp/data.txt",
 *     name: "data.txt",
 *     description: "用户数据文件",
 *     mimeType: "text/plain"
 *   }
 *
 * URI 格式: [scheme]://[path]
 *   file:///path/to/file     — 本地文件
 *   config://server           — 服务器配置
 *   log://latest              — 最新日志
 */
struct ResourceDef {
    std::string uri;
    std::string name;
    std::string description;
    std::string mimeType = "text/plain";

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["uri"] = uri;
        j["name"] = name;
        j["description"] = description;
        if (!mimeType.empty()) j["mimeType"] = mimeType;
        return j;
    }
};

/**
 * 资源内容 — resources/read 的返回结果中的一个条目
 *
 * 对应 MCP 规范的 ResourceContents:
 *   - text 资源: {uri, mimeType, text}
 *   - blob 资源: {uri, mimeType, blob} (未实现)
 */
struct ResourceContent {
    std::string uri;
    std::string mimeType = "text/plain";
    std::string text;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["uri"] = uri;
        if (!mimeType.empty()) j["mimeType"] = mimeType;
        j["text"] = text;
        return j;
    }
};

/**
 * resources/read 的请求参数
 * 客户端发来: {"uri":"file:///tmp/data.txt"}
 */
struct ResourceReadParams {
    std::string uri;

    static ResourceReadParams from_json(const nlohmann::json& j) {
        ResourceReadParams p;
        p.uri = j.value("uri", "");
        return p;
    }
};

/**
 * resources/read 的响应结果
 * 服务器回复: {"contents":[{"uri":"file:///tmp/data.txt","text":"..."}]}
 */
struct ResourceReadResult {
    std::vector<ResourceContent> contents;

    nlohmann::json to_json() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : contents) {
            arr.push_back(c.to_json());
        }
        return {{"contents", arr}};
    }
};

// ═══════════════════════════════════════════════════════════════
// Initialize 相关类型
// ═══════════════════════════════════════════════════════════════

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
