#include "command_plugin.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <sstream>
#include <chrono>
#include <thread>
#include <filesystem>

namespace fs = std::filesystem;

// 执行命令并捕获输出，超时秒数
static std::pair<std::string, int> run_cmd(const std::string& cmd, int timeout_sec = 60) {
    std::string result;
    int exit_code = -1;

    // 重定向 stderr 到 stdout
    std::string full_cmd = "(" + cmd + ") 2>&1";
    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) return {"popen failed", -1};

    std::array<char, 4096> buf;
    auto start = std::chrono::steady_clock::now();

    while (true) {
        // 检查超时
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_sec) {
            pclose(pipe);
            return {result + "\n[shell_exec: timeout after " + std::to_string(timeout_sec) + "s]", -1};
        }

        // 非阻塞读取
        if (fgets(buf.data(), buf.size(), pipe) != nullptr) {
            result += buf.data();
        } else {
            if (feof(pipe)) break;
            // 没数据，等 100ms
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    int status = pclose(pipe);
    exit_code = WEXITSTATUS(status);

    if (result.size() > 10000) {
        result = result.substr(0, 10000) + "\n[shell_exec: output truncated at 10KB]";
    }

    return {result, exit_code};
}

std::vector<mcp::ToolDef> CommandPlugin::get_tools() const {
    mcp::ToolDef t1;
    t1.name = "shell_exec";
    t1.description = "执行 shell 命令并返回输出。用 work_dir 指定工作目录，不要在命令里写 cd。支持管道、重定向。默认 60s 超时，最长 300s。";
    t1.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"command", {{"type", "string"}, {"description", "要执行的 shell 命令"}}},
            {"work_dir", {{"type", "string"}, {"description", "工作目录 (可选)"}}},
            {"python_env", {{"type", "string"}, {"description", "Conda 环境名，如 SAFNet。自动替换 python3 为环境路径"}}},
            {"timeout", {{"type", "integer"}, {"description", "超时秒数 (可选，默认 60，最大 300)"}}}
        }},
        {"required", {"command"}}
    };

    mcp::ToolDef t2;
    t2.name = "shell_exec_bg";
    t2.description = "后台启动长时间任务(如模型推理)。用 work_dir 指定目录，command 只写程序名和参数。输出重定向到日志文件，用 shell_exec cat 日志查看进度。";
    t2.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"command", {{"type", "string"}, {"description", "要后台执行的命令"}}},
            {"work_dir", {{"type", "string"}, {"description", "工作目录 (可选)"}}},
            {"python_env", {{"type", "string"}, {"description", "Conda 环境名，如 SAFNet。自动替换 python3 路径"}}}
        }},
        {"required", {"command"}}
    };

    return {t1, t2};
}

mcp::ToolCallResult CommandPlugin::call_tool(
    const std::string& tool_name, const nlohmann::json& args) {

    if (tool_name == "shell_exec") return handle_shell_exec(args);
    if (tool_name == "shell_exec_bg") return handle_shell_exec_bg(args);

    mcp::ToolCallResult r;
    r.isError = true;
    r.content.push_back({"text", "Unknown tool: " + tool_name});
    return r;
}

mcp::ToolCallResult CommandPlugin::handle_shell_exec(const nlohmann::json& args) {
    std::string cmd = args.value("command", "");
    std::string work_dir = args.value("work_dir", "");
    std::string python_env = args.value("python_env", "");  // conda 环境名
    int timeout = args.value("timeout", 60);
    if (timeout > 300) timeout = 300;
    if (timeout < 1) timeout = 60;

    if (cmd.empty()) {
        mcp::ToolCallResult r;
        r.isError = true;
        r.content.push_back({"text", "command is required"});
        return r;
    }

    // 如果指定了 conda 环境，用 conda run 或绝对路径 Python
    if (!python_env.empty()) {
        std::string py_path = "/media/data/miniconda3/envs/" + python_env + "/bin/python3";
        if (fs::exists(py_path)) {
            // 把 python3 替换为 conda 环境的 Python
            size_t pos = cmd.find("python3");
            if (pos != std::string::npos) {
                cmd.replace(pos, 7, py_path);
            } else if (cmd.find("python ") == 0) {
                cmd.replace(0, 7, py_path + " ");
            }
        } else {
            // 回退到 conda run
            cmd = "conda run -n " + python_env + " " + cmd;
        }
    }

    // popen 走 /bin/sh -c，所以 cd && xxx 直接生效
    std::string full_cmd;
    if (!work_dir.empty() && fs::exists(work_dir)) {
        full_cmd = "cd " + work_dir + " && " + cmd;
    } else {
        full_cmd = cmd;
    }

    auto [output, exit_code] = run_cmd(full_cmd, timeout);

    mcp::ToolCallResult r;
    mcp::TextContent tc;
    tc.text = output;
    r.isError = (exit_code != 0);
    r.content.push_back(tc);
    return r;
}

mcp::ToolCallResult CommandPlugin::handle_shell_exec_bg(const nlohmann::json& args) {
    std::string cmd = args.value("command", "");
    std::string work_dir = args.value("work_dir", ".");
    std::string python_env = args.value("python_env", "");

    if (cmd.empty()) {
        mcp::ToolCallResult r;
        r.isError = true;
        r.content.push_back({"text", "command is required"});
        return r;
    }

    // conda 环境替换
    if (!python_env.empty()) {
        std::string py_path = "/media/data/miniconda3/envs/" + python_env + "/bin/python3";
        if (fs::exists(py_path)) {
            size_t pos = cmd.find("python3");
            if (pos != std::string::npos) cmd.replace(pos, 7, py_path);
            else if (cmd.find("python ") == 0) cmd.replace(0, 7, py_path + " ");
        } else {
            cmd = "conda run -n " + python_env + " " + cmd;
        }
    }

    // popen 走 shell，cd && nohup 直接生效
    std::string log_file = "/tmp/cmd_bg_" + std::to_string(std::time(nullptr)) + ".log";
    std::string full_cmd = "cd " + work_dir + " && nohup " + cmd + " > " +
                           log_file + " 2>&1 & echo PID=$!";

    auto [output, exit_code] = run_cmd(full_cmd, 5);

    mcp::ToolCallResult r;
    r.isError = false;
    r.content.push_back({"text", "Background process started:\n" + output +
                         "\nLog file: " + log_file +
                         "\nCheck progress: shell_exec cat " + log_file});
    return r;
}

// C ABI exports
extern "C" plugin::IPlugin* create_plugin() { return new CommandPlugin(); }
extern "C" void destroy_plugin(plugin::IPlugin* p) { delete p; }
