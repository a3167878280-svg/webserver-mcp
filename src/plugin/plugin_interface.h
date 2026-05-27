#pragma once

#include "../mcp/mcp_types.h"
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace plugin {

// 插件抽象接口 — 所有 .so 插件必须实现此接口
// 由于插件与主程序使用相同编译器 (GCC 9.4.0 + C++17)，
// STL 类型跨 .so 边界传递是 ABI 安全的
class IPlugin {
public:
    virtual ~IPlugin() = default;

    virtual const char* name() const = 0;
    virtual const char* version() const = 0;

    // 返回本插件提供的所有工具定义
    virtual std::vector<mcp::ToolDef> get_tools() const = 0;

    // 分发工具调用，tool_name 由注册表保证是本插件注册过的
    virtual mcp::ToolCallResult call_tool(
        const std::string& tool_name,
        const nlohmann::json& arguments) = 0;
};

} // namespace plugin

// dlopen 入口函数，extern "C" 防止 C++ name mangling
extern "C" {
    plugin::IPlugin* create_plugin();
    void destroy_plugin(plugin::IPlugin* p);
}
