#include "review_plugin.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>

namespace fs = std::filesystem;

static mcp::ToolCallResult make_error(const std::string& msg) {
    mcp::ToolCallResult r;
    r.isError = true;
    r.content.push_back(mcp::TextContent{"text", msg});
    return r;
}

static mcp::ToolCallResult make_result(const std::string& text) {
    mcp::ToolCallResult r;
    r.isError = false;
    r.content.push_back(mcp::TextContent{"text", text});
    return r;
}

std::vector<mcp::ToolDef> ReviewPlugin::get_tools() const {
    mcp::ToolDef review;
    review.name = "code_review";
    review.description = "对指定代码文件进行静态审查，检查常见问题 (TODO, 硬编码常量, 长函数, 缺失头文件保护等)";
    review.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"file_path", {
                {"type", "string"},
                {"description", "要审查的代码文件绝对路径"}
            }},
            {"language", {
                {"type", "string"},
                {"description", "编程语言: cpp, python, java, go, js 等 (可选，自动检测)"}
            }}
        }},
        {"required", {"file_path"}}
    };

    mcp::ToolDef stats;
    stats.name = "code_stats";
    stats.description = "统计代码文件的基本信息 (行数、注释行数、函数数量、类数量等)";
    stats.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"file_path", {
                {"type", "string"},
                {"description", "要统计的代码文件绝对路径"}
            }}
        }},
        {"required", {"file_path"}}
    };

    return {review, stats};
}

mcp::ToolCallResult ReviewPlugin::call_tool(const std::string& name,
                                              const nlohmann::json& args) {
    if (name == "code_review") return handle_code_review(args);
    if (name == "code_stats") return handle_code_stats(args);
    return make_error("Unknown tool: " + name);
}

// 检测语言
static std::string detect_lang(const std::string& path) {
    if (path.find(".cpp") != std::string::npos || path.find(".h") != std::string::npos) return "cpp";
    if (path.find(".py") != std::string::npos) return "python";
    if (path.find(".java") != std::string::npos) return "java";
    if (path.find(".go") != std::string::npos) return "go";
    if (path.find(".js") != std::string::npos || path.find(".ts") != std::string::npos) return "javascript";
    if (path.find(".c") != std::string::npos) return "c";
    if (path.find(".sh") != std::string::npos) return "shell";
    return "unknown";
}

mcp::ToolCallResult ReviewPlugin::handle_code_review(const nlohmann::json& args) {
    std::string path = args.value("file_path", "");
    if (path.empty()) return make_error("Missing required parameter: file_path");
    if (!fs::exists(path)) return make_error("File not found: " + path);

    std::ifstream f(path);
    if (!f.is_open()) return make_error("Cannot open: " + path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    std::string lang = args.value("language", detect_lang(path));
    std::ostringstream report;
    report << "=== Code Review: " << fs::path(path).filename().string()
           << " (" << lang << ") ===\n\n";

    std::vector<std::string> issues;
    std::istringstream stream(content);
    std::string line;
    int line_num = 0;
    int comment_lines = 0;
    int blank_lines = 0;
    int total_lines = 0;
    int max_indent = 0;

    while (std::getline(stream, line)) {
        total_lines++;
        if (line.empty() || line.find_first_not_of(" \t") == std::string::npos) {
            blank_lines++;
            continue;
        }

        // 注释检测
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        if (trimmed.find("//") == 0 || trimmed.find("#") == 0 || trimmed.find("--") == 0) {
            comment_lines++;
        }

        // TODO/FIXME/HACK 检测
        if (line.find("TODO") != std::string::npos || line.find("FIXME") != std::string::npos ||
            line.find("HACK") != std::string::npos) {
            issues.push_back("Line " + std::to_string(line_num + 1) + ": " + trimmed);
        }

        // 缩进检测
        size_t indent = 0;
        while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) indent++;
        if (indent > static_cast<size_t>(max_indent)) max_indent = indent;

        line_num++;
    }

    report << "Total lines: " << total_lines << "\n";
    report << "Blank lines: " << blank_lines << "\n";
    report << "Comment lines: " << comment_lines << "\n";
    report << "Code lines: " << (total_lines - blank_lines - comment_lines) << "\n";
    report << "Max indent: " << max_indent << " spaces\n\n";

    // 头文件保护检测
    if (lang == "cpp" || lang == "c") {
        bool has_pragma = content.find("#pragma once") != std::string::npos;
        bool has_ifndef = content.find("#ifndef") != std::string::npos;
        if (!has_pragma && !has_ifndef && (path.find(".h") != std::string::npos)) {
            issues.push_back("Missing header guard (#pragma once or #ifndef)");
        }
    }

    // 硬编码常量
    std::regex hardcode(R"((password|secret|token|key)\s*=\s*\"[^\"]+\")");
    std::sregex_iterator it(content.begin(), content.end(), hardcode);
    std::sregex_iterator end;
    while (it != end) {
        issues.push_back("Hardcoded credential: " + it->str());
        ++it;
    }

    if (issues.empty()) {
        report << "✅ No issues found!\n";
    } else {
        report << "Issues found (" << issues.size() << "):\n";
        for (auto& i : issues) report << "  - " << i << "\n";
    }

    // 函数/类统计
    int func_count = 0, class_count = 0;
    if (lang == "cpp" || lang == "c" || lang == "java" || lang == "go") {
        std::regex func_re(R"((\w+)\s+(\w+)\s*\([^)]*\)\s*\{)");
        std::sregex_iterator fi(content.begin(), content.end(), func_re);
        for (; fi != end; ++fi) func_count++;
    } else if (lang == "python") {
        std::regex py_func(R"(^\s*def\s+(\w+)\s*\()");
        std::sregex_iterator fi(content.begin(), content.end(), py_func);
        for (; fi != end; ++fi) func_count++;
    }

    if (lang == "cpp" || lang == "java") {
        std::regex class_re(R"(\bclass\s+(\w+))");
        std::sregex_iterator ci(content.begin(), content.end(), class_re);
        for (; ci != end; ++ci) class_count++;
    }

    report << "\nFunctions detected: " << func_count << "\n";
    if (class_count > 0) report << "Classes detected: " << class_count << "\n";

    // 长函数警告
    if (func_count > 0 && (total_lines / func_count) > 50) {
        report << "⚠ Average function length > 50 lines, consider refactoring\n";
    }

    return make_result(report.str());
}

mcp::ToolCallResult ReviewPlugin::handle_code_stats(const nlohmann::json& args) {
    std::string path = args.value("file_path", "");
    if (path.empty()) return make_error("Missing required parameter: file_path");
    if (!fs::exists(path)) return make_error("File not found: " + path);

    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    std::istringstream stream(content);
    std::string line;
    int total = 0, blank = 0, comment = 0;

    while (std::getline(stream, line)) {
        total++;
        std::string t = line;
        t.erase(0, t.find_first_not_of(" \t"));
        if (t.empty()) { blank++; continue; }
        if (t.find("//") == 0 || t.find("#") == 0 || t.find("--") == 0) { comment++; }
    }

    auto ft = fs::last_write_time(path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());

    std::ostringstream out;
    out << "=== File Stats: " << fs::path(path).filename().string() << " ===\n";
    out << "Size: " << fs::file_size(path) << " bytes\n";
    out << "Total lines: " << total << "\n";
    out << "Blank lines: " << blank << "\n";
    out << "Comment lines: " << comment << "\n";
    out << "Code lines: " << (total - blank - comment) << "\n";
    out << "Language: " << detect_lang(path) << "\n";

    return make_result(out.str());
}

// ═══════════════════════════════════════════════════════════════
// Prompt 接口
// ═══════════════════════════════════════════════════════════════

std::vector<mcp::PromptDef> ReviewPlugin::get_prompts() const {
    mcp::PromptDef p1;
    p1.name = "code_review";
    p1.description = "按专业标准审查代码，检查 bug、安全漏洞、性能问题和风格";
    p1.arguments = {
        {"file_path", "要审查的代码文件路径", true},
        {"language", "编程语言 (cpp/python/java/go等，可选自动检测)", false},
        {"focus", "审查重点: all/security/performance/style，默认 all", false}
    };

    mcp::PromptDef p2;
    p2.name = "code_explain";
    p2.description = "逐行解释代码逻辑，适合学习或 code review 前理解代码";
    p2.arguments = {
        {"file_path", "要解释的代码文件路径", true},
        {"level", "详细程度: simple(简单)/detailed(详细)，默认 detailed", false}
    };

    return {p1, p2};
}

mcp::PromptGetResult ReviewPlugin::get_prompt(
    const std::string& name, const nlohmann::json& arguments) {

    mcp::PromptGetResult result;

    if (name == "code_review") {
        std::string file_path = arguments.value("file_path", "");
        std::string language = arguments.value("language", "auto");
        std::string focus = arguments.value("focus", "all");

        result.description = "审查代码: " + file_path;

        mcp::PromptMessage msg;
        msg.role = "user";

        std::string text = "你是一位资深代码审查专家。请审查以下代码";
        if (!language.empty() && language != "auto")
            text += "（语言: " + language + "）";

        text += "。\n\n审查要求:\n";
        if (focus == "all" || focus == "security") {
            text += "1. **安全检查**: 硬编码密码/密钥、SQL注入、XSS、缓冲区溢出、权限问题\n";
        }
        if (focus == "all" || focus == "performance") {
            text += "2. **性能分析**: 不必要的拷贝、低效算法、内存泄漏风险、IO 瓶颈\n";
        }
        if (focus == "all" || focus == "style") {
            text += "3. **代码风格**: 命名规范、注释质量、函数长度、圈复杂度\n";
        }
        text += "4. **逻辑错误**: 边界条件、空指针、类型转换、并发问题\n";
        text += "\n请给出: 问题列表(含行号) + 严重程度 + 修复建议\n";
        text += "文件: " + file_path + "\n";
        text += "请用 file_read 工具读取该文件后进行审查。";

        msg.content.text = text;
        result.messages.push_back(msg);

    } else if (name == "code_explain") {
        std::string file_path = arguments.value("file_path", "");
        std::string level = arguments.value("level", "detailed");

        result.description = "解释代码: " + file_path;

        mcp::PromptMessage msg;
        msg.role = "user";

        std::string text = "请";
        if (level == "simple") {
            text += "用简单的语言概括这段代码的功能和主要逻辑:\n";
        } else {
            text += "逐行详细解释这段代码，包括每个函数的作用、关键变量的含义、数据流向:\n";
        }
        text += "文件: " + file_path + "\n";
        text += "请用 file_read 工具读取该文件后进行解释。";

        msg.content.text = text;
        result.messages.push_back(msg);
    }

    return result;
}

extern "C" plugin::IPlugin* create_plugin() { return new ReviewPlugin(); }
extern "C" void destroy_plugin(plugin::IPlugin* p) { delete p; }
