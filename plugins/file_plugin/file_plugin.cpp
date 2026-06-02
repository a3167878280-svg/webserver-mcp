#include "file_plugin.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <iomanip>

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

    nlohmann::json grep_schema = {
        {"type", "object"},
        {"properties", {
            {"path", {
                {"type", "string"},
                {"description", "Absolute path to the file to search in"}
            }},
            {"pattern", {
                {"type", "string"},
                {"description", "Search pattern (supports basic regex like 'error|warn')"}
            }},
            {"context_lines", {
                {"type", "integer"},
                {"description", "Number of context lines to show around each match (default 2)"}
            }},
            {"max_matches", {
                {"type", "integer"},
                {"description", "Maximum matches to return (default 20, to avoid huge output)"}
            }}
        }},
        {"required", {"path", "pattern"}}
    };

    return {
        make_tool("file_read", "Read the contents of a file. For large files, consider using grep_file first to locate relevant sections.", read_schema),
        make_tool("file_list", "List files and directories at a given path", list_schema),
        make_tool("grep_file", "Search for a pattern in a file and return matching lines with context. Useful for finding specific content in large files without reading the entire file.", grep_schema)
    };
}

mcp::ToolCallResult FilePlugin::call_tool(const std::string& tool_name,
                                           const nlohmann::json& args) {
    if (tool_name == "file_read") {
        return handle_file_read(args);
    } else if (tool_name == "file_list") {
        return handle_file_list(args);
    } else if (tool_name == "grep_file") {
        return handle_grep_file(args);
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

/**
 * grep_file — 在文件中搜索匹配行并返回上下文
 *
 * 这个工具解决了"读了大文件不会分析"的问题:
 *   1. 先用 grep_file 定位关键位置
 *   2. 再用 file_read 或上下文分析具体内容
 */
mcp::ToolCallResult FilePlugin::handle_grep_file(const nlohmann::json& args) {
    std::string path = args.value("path", "");
    std::string pattern = args.value("pattern", "");
    int context_lines = args.value("context_lines", 2);
    int max_matches = args.value("max_matches", 20);

    if (path.empty()) return make_error("Missing required parameter: path");
    if (pattern.empty()) return make_error("Missing required parameter: pattern");
    if (context_lines < 0) context_lines = 0;
    if (context_lines > 10) context_lines = 10;
    if (max_matches < 1) max_matches = 1;
    if (max_matches > 100) max_matches = 100;

    try {
        std::ifstream f(path);
        if (!f.is_open()) return make_error("Cannot open file: " + path);

        // 读取所有行
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(f, line)) {
            lines.push_back(line);
        }

        // 搜索匹配 (简单子串匹配，支持 | 分隔的多模式)
        std::vector<std::string> patterns;
        std::istringstream ps(pattern);
        std::string p;
        while (std::getline(ps, p, '|')) {
            // trim
            p.erase(0, p.find_first_not_of(" \t"));
            p.erase(p.find_last_not_of(" \t") + 1);
            if (!p.empty()) patterns.push_back(p);
        }
        if (patterns.empty()) patterns.push_back(pattern);

        struct Match {
            int line_num;
            std::string matched_pattern;
        };
        std::vector<Match> matches;
        for (size_t i = 0; i < lines.size() && (int)matches.size() < max_matches; i++) {
            for (auto& pat : patterns) {
                if (lines[i].find(pat) != std::string::npos) {
                    matches.push_back({(int)i, pat});
                    break;
                }
            }
        }

        if (matches.empty()) {
            return make_result("No matches found for pattern '" + pattern + "' in " + path);
        }

        // 生成上下文输出
        std::ostringstream oss;
        int total_lines = (int)lines.size();
        oss << "Found " << matches.size() << " match(es) for '" << pattern
            << "' in " << path << " (" << total_lines << " lines total):\n\n";

        int last_end = -1;
        for (size_t mi = 0; mi < matches.size(); mi++) {
            auto& m = matches[mi];
            int start = std::max(0, m.line_num - context_lines);
            int end = std::min(total_lines - 1, m.line_num + context_lines);

            // 如果和上一个匹配的上下文重叠，跳过重叠部分
            if (start <= last_end && mi > 0) {
                start = last_end + 1;
            }
            if (start > end) continue;

            oss << "--- Match " << (mi + 1) << " (line " << (m.line_num + 1)
                << ", matched: " << m.matched_pattern << ") ---\n";

            for (int i = start; i <= end; i++) {
                std::string prefix = (i == m.line_num) ? ">>> " : "    ";
                oss << prefix << std::setw(4) << (i + 1) << "| " << lines[i] << "\n";
            }
            last_end = end;
        }

        return make_result(oss.str());
    } catch (const std::exception& e) {
        return make_error(std::string("grep_file error: ") + e.what());
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
