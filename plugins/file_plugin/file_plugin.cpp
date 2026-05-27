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

// dlopen 入口
extern "C" plugin::IPlugin* create_plugin() {
    return new FilePlugin();
}

extern "C" void destroy_plugin(plugin::IPlugin* p) {
    delete p;
}
