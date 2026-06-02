#include "file_plugin.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

// 构建 ToolDef 的辅助函数
static mcp::ToolDef make_tool(const std::string& name,
                               const std::string& desc,
                               const nlohmann::json& schema) {
    mcp::ToolDef t;
    t.name = name;
    t.description = desc;
    t.inputSchema = schema;
    return t;
}

// 构建错误结果的辅助函数
static mcp::ToolCallResult make_error(const std::string& msg) {
    mcp::ToolCallResult r;
    r.isError = true;
    r.content.push_back(mcp::TextContent{"text", msg});
    return r;
}

// 构建成功结果
static mcp::ToolCallResult make_result(const std::string& text) {
    mcp::ToolCallResult r;
    r.isError = false;
    r.content.push_back(mcp::TextContent{"text", text});
    return r;
}

std::vector<mcp::ToolDef> FilePlugin::get_tools() const {
    nlohmann::json read_schema = {
        {"type", "object"},
        {"properties", {
            {"path", {
                {"type", "string"},
                {"description", "Absolute path to the file to read"}
            }}
        }},
        {"required", {"path"}}
    };

    nlohmann::json list_schema = {
        {"type", "object"},
        {"properties", {
            {"directory", {
                {"type", "string"},
                {"description", "Absolute path to the directory to list"}
            }}
        }},
        {"required", {"directory"}}
    };

    return {
        make_tool("file_read", "Read the contents of a file", read_schema),
        make_tool("file_list", "List files and directories at a given path", list_schema)
    };
}

mcp::ToolCallResult FilePlugin::call_tool(const std::string& tool_name,
                                           const nlohmann::json& args) {
    if (tool_name == "file_read") {
        return handle_file_read(args);
    } else if (tool_name == "file_list") {
        return handle_file_list(args);
    }
    return make_error("Unknown tool: " + tool_name);
}

mcp::ToolCallResult FilePlugin::handle_file_read(const nlohmann::json& args) {
    std::string path = args.value("path", "");
    if (path.empty()) {
        return make_error("Missing required parameter: path");
    }
    if (path[0] != '/') {
        return make_error("Only absolute paths are allowed");
    }

    try {
        std::ifstream f(path);
        if (!f.is_open()) {
            return make_error("Cannot open file: " + path);
        }
        std::ostringstream oss;
        oss << f.rdbuf();
        return make_result(oss.str());
    } catch (const std::exception& e) {
        return make_error(std::string("Error reading file: ") + e.what());
    }
}

mcp::ToolCallResult FilePlugin::handle_file_list(const nlohmann::json& args) {
    std::string dir = args.value("directory", "");
    if (dir.empty()) {
        return make_error("Missing required parameter: directory");
    }
    if (dir[0] != '/') {
        return make_error("Only absolute paths are allowed");
    }

    try {
        if (!fs::exists(dir) || !fs::is_directory(dir)) {
            return make_error("Not a directory: " + dir);
        }

        std::ostringstream oss;
        for (const auto& entry : fs::directory_iterator(dir)) {
            std::string type = entry.is_directory() ? "[DIR] " : "[FILE] ";
            oss << type << entry.path().filename().string() << "\n";
        }
        return make_result(oss.str());
    } catch (const std::exception& e) {
        return make_error(std::string("Error listing directory: ") + e.what());
    }
}

// ═══════════════════════════════════════════════════════════════
// Resource 接口 — 与上面的 Tool 接口共享底层文件读取能力
// ═══════════════════════════════════════════════════════════════

/**
 * 暴露可用资源列表
 *
 * Tool vs Resource 的核心区别在这里一目了然:
 *   Tool file_read:  LLM 必须知道路径 → 主动传入 path 参数
 *   Resource:        服务器预先注册 URI → 客户端浏览列表 → 点击读取
 *
 * 同一个底层能力 (读文件)，两种不同的交互模式。
 */
std::vector<mcp::ResourceDef> FilePlugin::get_resources() const {
    std::vector<mcp::ResourceDef> resources;

    // 注册 config://server — 即使不知道 config.json 在哪也能访问
    mcp::ResourceDef config_res;
    config_res.uri = "config://server";
    config_res.name = "服务器配置";
    config_res.description = "当前 MCP 服务器运行配置 (config.json 内容)";
    config_res.mimeType = "application/json";
    resources.push_back(config_res);

    return resources;
}

mcp::ResourceReadResult FilePlugin::read_resource(const std::string& uri) {
    if (uri == "config://server") {
        return read_file_resource("config.json", "application/json");
    }
    return {};  // 未知资源
}

// 辅助函数: 读文件并包装成 ResourceReadResult
mcp::ResourceReadResult FilePlugin::read_file_resource(
    const std::string& path, const std::string& mimeType) {

    mcp::ResourceReadResult result;
    try {
        std::ifstream f(path);
        if (!f.is_open()) {
            // 资源不存在 → 返回空，McpHandler 会抛异常
            return {};
        }
        std::ostringstream oss;
        oss << f.rdbuf();

        mcp::ResourceContent rc;
        rc.uri = "file://" + fs::absolute(path).string();
        rc.mimeType = mimeType;
        rc.text = oss.str();
        result.contents.push_back(rc);
    } catch (...) {
        return {};
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════
// Prompt 接口 — 预定义对话模板
// ═══════════════════════════════════════════════════════════════

std::vector<mcp::PromptDef> FilePlugin::get_prompts() const {
    mcp::PromptDef prompt;
    prompt.name = "file_analyzer";
    prompt.description = "分析指定文件的内容并给出摘要和关键信息";
    prompt.arguments = {
        {"file_path", "要分析的文件的绝对路径", true},
        {"focus", "关注的方面: content(内容)/structure(结构)/summary(摘要)，默认 summary", false}
    };
    return {prompt};
}

mcp::PromptGetResult FilePlugin::get_prompt(
    const std::string& name, const nlohmann::json& arguments) {

    mcp::PromptGetResult result;

    if (name == "file_analyzer") {
        std::string file_path = arguments.value("file_path", "");
        std::string focus = arguments.value("focus", "summary");

        result.description = "分析文件 " + file_path + " (关注: " + focus + ")";

        // 尝试读取文件内容，嵌入到 prompt 中
        std::string file_content;
        try {
            std::ifstream f(file_path);
            if (f.is_open()) {
                std::ostringstream oss;
                oss << f.rdbuf();
                file_content = oss.str();
            }
        } catch (...) {}

        // 构建引导 LLM 的预设消息
        mcp::PromptMessage msg;
        msg.role = "user";

        std::string text = "请分析以下文件内容";
        if (focus == "structure") {
            text += "，重点关注其结构和组织方式";
        } else if (focus == "content") {
            text += "，深入解读其内容含义";
        } else {
            text += "，给出简洁的摘要";
        }
        text += ":\n\n文件路径: " + file_path;

        if (!file_content.empty()) {
            // 限制嵌入内容长度，避免超出 LLM 上下文
            if (file_content.size() > 8000) {
                file_content = file_content.substr(0, 8000) + "\n...(内容已截断)";
            }
            text += "\n\n```\n" + file_content + "\n```";
        } else {
            text += "\n\n(文件无法读取，请用 file_read 工具先读取)";
        }

        msg.content.text = text;
        result.messages.push_back(msg);
    }

    return result;
}

// dlopen 入口
extern "C" plugin::IPlugin* create_plugin() {
    return new FilePlugin();
}

extern "C" void destroy_plugin(plugin::IPlugin* p) {
    delete p;
}
