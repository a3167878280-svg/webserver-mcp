/**
 * 插件抽象接口 — 所有 .so 插件必须实现此接口
 *
 * ═══════════ 插件系统的核心契约 ═══════════
 *
 * 主程序和插件之间只通过这个接口交互，不需要知道对方的具体实现:
 *
 *   主程序                         插件 .so
 *   ──────                        ────────
 *   dlopen()  ─────────────────→  加载到进程
 *   dlsym("create_plugin") ────→  返回 IPlugin*
 *   plugin->get_tools() ───────→  返回 vector<ToolDef>
 *   plugin->call_tool() ───────→  执行工具, 返回 ToolCallResult
 *   dlsym("destroy_plugin") ───→  delete IPlugin*
 *
 *   IPlugin 接口 = 3 个纯虚函数:
 *     1. name()      → 插件名称 (日志用)
 *     2. get_tools() → 返回本插件提供的工具列表 (名字 + 描述 + 参数 schema)
 *     3. call_tool() → 分发并执行工具调用
 *
 * ═══════════ 为什么参数用 nlohmann::json? ═══════════
 *
 *   因为 LLM 生成的工具参数天然就是 JSON。使用 nlohmann::json:
 *     - 零序列化开销: 参数从 JSON-RPC 请求直接传到插件
 *     - 灵活: 不同工具的参数结构可以完全不同
 *     - ABI 安全: nlohmann::json 是 header-only，编译进插件和主程序是同一份代码
 *
 * ═══════════ 跨 .so 边界的 ABI 注意事项 ═══════════
 *
 *   插件和主程序必须用**相同版本**的 GCC + C++17 编译，
 *   这样 STL 类型 (std::string, std::vector, nlohmann::json) 的
 *   内存布局完全一致，跨 .so 边界传递才是安全的。
 */

#pragma once

#include "../mcp/mcp_types.h"
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace plugin {

class IPlugin {
public:
    virtual ~IPlugin() = default;

    // 插件名称 (如 "WeatherPlugin"), 用于日志
    virtual const char* name() const = 0;

    // 插件版本 (如 "1.0"), 用于日志
    virtual const char* version() const = 0;

    // 返回本插件提供的所有工具定义
    // 一个插件可以提供多个工具 (如 FilePlugin 提供 file_read + file_list)
    virtual std::vector<mcp::ToolDef> get_tools() const = 0;

    /**
     * 分发工具调用
     *
     * @param tool_name  工具名 (由注册表保证是本插件注册过的)
     * @param arguments  JSON 参数 (如 {"city":"Beijing"})
     * @return           ToolCallResult (content 数组 + isError 标记)
     */
    virtual mcp::ToolCallResult call_tool(
        const std::string& tool_name,
        const nlohmann::json& arguments) = 0;

    /**
     * 返回本插件暴露的资源列表 (可选)
     *
     * 和 Tools 不同，Resources 是"被动暴露的数据":
     *   - Tools: LLM 主动调用函数 → 执行 → 返回
     *   - Resources: 服务器声明"我有这些数据" → 客户端浏览 → 选一个读
     *
     * 默认返回空 — 只有有资源可暴露的插件才需要重写
     */
    virtual std::vector<mcp::ResourceDef> get_resources() const {
        return {};
    }

    /**
     * 读取资源内容
     *
     * @param uri  资源 URI (如 "file:///tmp/data.txt")
     * @return     ResourceReadResult (contents 数组)
     *
     * 默认返回空 — 和 get_resources() 配套重写
     */
    virtual mcp::ResourceReadResult read_resource(const std::string& uri) {
        return {};
    }

    /**
     * 返回本插件提供的 Prompt 模板列表 (可选)
     *
     * 和 Tools/Resources 不同，Prompts 是"对话模板":
     *   - Tools: LLM 主动调函数
     *   - Resources: 客户端浏览数据
     *   - Prompts: 用户选择模板 → 填入参数 → 生成预设对话
     *
     * 默认返回空 — 只有提供 Prompt 的插件才需要重写
     */
    virtual std::vector<mcp::PromptDef> get_prompts() const {
        return {};
    }

    /**
     * 获取 Prompt 的完整内容
     *
     * @param name       Prompt 名称
     * @param arguments  用户填入的参数 (如 {language:"cpp"})
     * @return           PromptGetResult (description + messages 数组)
     *
     * 默认返回空 — 和 get_prompts() 配套重写
     */
    virtual mcp::PromptGetResult get_prompt(
        const std::string& name,
        const nlohmann::json& arguments) {
        return {};
    }
};

} // namespace plugin

/**
 * dlopen 入口函数 — 必须是 extern "C" (禁止 C++ name mangling)
 *
 * 每个 .so 必须导出这两个符号:
 *
 *   extern "C" plugin::IPlugin* create_plugin();
 *   extern "C" void destroy_plugin(plugin::IPlugin* p);
 *
 * 实现非常简单:
 *   create_plugin()  { return new MyPlugin(); }
 *   destroy_plugin(p) { delete p; }
 */
extern "C" {
    plugin::IPlugin* create_plugin();
    void destroy_plugin(plugin::IPlugin* p);
}
