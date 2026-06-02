# TinyMCP — C++ MCP 协议服务器

基于 C++17 的 [Model Context Protocol](https://modelcontextprotocol.io/) 服务器，**完整实现 MCP 三大能力 (Tools + Resources + Prompts)**，通过插件化架构和双传输模式为 AI 客户端提供标准化工具调用和对话模板。

## 项目结构

```
├── src/
│   ├── main.cpp                    入口：加载插件、启动传输层
│   ├── config.h / config.cpp       JSON 配置文件加载
│   ├── common.h                    全局变量声明
│   ├── mcp/                        MCP 协议层
│   │   ├── jsonrpc.h              JSON-RPC 2.0 数据结构
│   │   ├── jsonrpc_parser.*       请求解析器
│   │   ├── jsonrpc_serializer.*    响应序列化器
│   │   ├── mcp_types.h            MCP 类型定义 (Tool/Resource)
│   │   └── mcp_handler.*          方法路由器 (7 个 MCP 方法)
│   ├── transport/                  传输层
│   │   ├── transport.h            抽象接口 (send / on_message)
│   │   ├── stdio_transport.*      stdin/stdout 本地传输
│   │   └── http_sse_transport.*   HTTP+SSE 远程传输
│   ├── plugin/                     插件系统
│   │   ├── plugin_interface.h     IPlugin 抽象接口 (含 Resource 支持)
│   │   ├── plugin_registry.*      工具+资源统一注册表
│   │   ├── plugin_manager.*       .so 扫描、dlopen、生命周期
│   │   └── dynamic_library.*      跨平台动态库封装
│   ├── llm/                        LLM 客户端
│   │   ├── llm_client.*           OpenAI/Anthropic 流式 API
│   │   └── tool_orchestrator.*    Agent 循环 (LLM ⇄ 工具编排)
│   ├── server/
│   │   └── conversation_manager.* 多轮对话持久化
│   ├── log/                        异步日志
│   ├── lock/                       同步原语 (mutex/sem/cond)
│   └── threadpool/                 泛型线程池
├── plugins/                        5 个插件 (编译为 .so, 均支持 Tools + Resources + Prompts)
│   ├── file_plugin/               file_read, file_list + config://server 资源 + file_analyzer prompt
│   ├── weather_plugin/            query_weather (wttr.in)
│   ├── bilibili_plugin/           B站 UP主视频/信息/热门
│   ├── command_plugin/            shell_exec, shell_exec_bg
│   └── review_plugin/             code_review, code_stats + code_review/code_explain prompts
├── chat/
│   └── chat.html                  单页聊天界面
├── third_party/
│   ├── httplib.h                  cpp-httplib (HTTP/HTTPS)
│   └── nlohmann/json.hpp          JSON 解析库
├── config.json                    运行配置
└── CMakeLists.txt                 构建系统
```

## 支持的 MCP 方法

| Method | 功能 | 说明 |
|--------|------|------|
| `initialize` | 协议握手 | 声明 tools + resources 能力 |
| `ping` | 心跳检测 | 返回 `{}` |
| `tools/list` | 工具列表 | 9 个工具，来自 5 个插件 |
| `tools/call` | 执行工具 | 按名称分发到插件执行 |
| `resources/list` | 资源列表 | 返回所有可用资源 URI |
| `resources/read` | 读取资源 | 按 URI 读取资源内容 |
| `prompts/list` | 提示列表 | 3 个 Prompt 模板，来自 2 个插件 |
| `prompts/get` | 获取提示 | 按名称获取 Prompt 的完整消息内容 |
| `notifications/initialized` | 初始化完成 | 通知，无响应 |

## 传输模式

通过 `config.json` 的 `mode` 字段切换：

| mode | 协议 | 帧格式 | 适用场景 |
|------|------|--------|----------|
| `"stdio"` | stdin/stdout | `Content-Length: N\r\n\r\n<body>` | Claude Desktop 本地集成 |
| `"http"` | HTTP+SSE (端口 9006) | SSE 事件流 | 浏览器聊天 / 远程客户端 |

### stdio 模式测试

```bash
BODY='{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
printf 'Content-Length: %d\r\n\r\n%s' ${#BODY} "$BODY" | ./mcp_server
```

### HTTP 模式端点

| 端点 | 方法 | 功能 |
|------|------|------|
| `/sse` | GET | SSE 长连接 (服务器→客户端) |
| `/message` | POST | JSON-RPC 请求入口 |
| `/api/chat` | POST | LLM 聊天 (SSE 流式) |
| `/api/models` | GET | 模型列表 |
| `/api/tools` | GET | 工具列表 |
| `/api/conversations` | GET/POST/DELETE | 对话管理 |
| `/chat.html` | GET | 聊天界面 |
| `/health` | GET | 健康检查 |

## Prompts（提示模板）

Prompts 是 MCP 的第三大能力 — 预定义对话模板，用户选择后系统自动构建高质量的 LLM 对话指令。

### 可用的 Prompt

| Prompt | 来源 | 功能 | 参数 |
|--------|------|------|------|
| `code_review` | ReviewPlugin | 专业代码审查，检查安全/性能/风格 | file_path, language, focus |
| `code_explain` | ReviewPlugin | 逐行解释代码逻辑 | file_path, level |
| `file_analyzer` | FilePlugin | 分析文件内容并给出摘要 | file_path, focus |

### Tool vs Resource vs Prompt

```
同一个代码审查场景，三种能力各司其职:

  Tool (code_review):
    LLM: "我要审查 /tmp/test.cpp"
    → 调 code_review(file_path="/tmp/test.cpp")
    → 执行静态分析 → 返回结果
    ✅ LLM 主动发起，自动化
    ❌ 依赖 LLM 知道工具名

  Resource (config://server):
    服务器: "我有这些数据可读: [config://server, ...]"
    → 客户端浏览 → 点 config → 读
    ✅ 被动暴露，浏览发现
    ❌ 只看不分析

  Prompt (code_review):
    用户点"代码审查助手"
    → 系统自动构建专业审查指令
    → LLM 收到: "你是代码审查专家。请检查: 安全/性能/风格..."
    → LLM 可能还会自己调用 code_review 工具来执行分析
    ✅ 用户一键触发，指令质量稳定
    ✅ LLM 可以在 Prompt 引导下自行决定用哪些工具
```

## 插件开发

插件是独立编译的 `.so` 文件，实现 `IPlugin` 接口即可。一个插件可以同时提供 Tools、Resources 和 Prompts：

```cpp
class MyPlugin : public plugin::IPlugin {
    const char* name() const override { return "my_plugin"; }
    const char* version() const override { return "1.0"; }

    // 工具 (必须)
    vector<mcp::ToolDef> get_tools() const override { /* ... */ }
    ToolCallResult call_tool(name, args) override { /* ... */ }

    // 资源 (可选)
    vector<mcp::ResourceDef> get_resources() const override { /* ... */ }
    ResourceReadResult read_resource(uri) override { /* ... */ }

    // Prompt 模板 (可选)
    vector<mcp::PromptDef> get_prompts() const override { /* ... */ }
    PromptGetResult get_prompt(name, args) override { /* ... */ }
};

extern "C" plugin::IPlugin* create_plugin() { return new MyPlugin(); }
extern "C" void destroy_plugin(plugin::IPlugin* p) { delete p; }
```

编译后放入 `plugins/` 目录，服务器启动时自动加载。

## 构建和运行

### 依赖

- GCC 9.4+ / Clang 10+ (C++17)
- CMake 3.14+
- pthread, dl, OpenSSL
- nlohmann/json (header-only, 已包含)
- cpp-httplib (header-only, 已包含)

### 编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 运行

```bash
# HTTP 模式 (浏览器聊天)
./build/mcp_server

# 浏览器打开
# http://localhost:9006/chat.html
```

### 配置 (`config.json`)

```json
{
    "mode": "http",
    "port": 9006,
    "plugin_dir": "./build/plugins",
    "llm_base_url": "https://api.openai.com/v1",
    "llm_model": "gpt-4o"
}
```

## 架构

```
stdin / HTTP POST
    │
    ▼
┌─────────────────┐
│   Transport 层   │  stdio (Content-Length 帧) / HTTP+SSE (双通道)
└────────┬────────┘
         │ raw JSON string
         ▼
┌─────────────────┐
│   Parser 层      │  JSON 字符串 → JsonRpcRequest 结构体
└────────┬────────┘
         │ 结构化请求
         ▼
┌─────────────────┐
│   Handler 层     │  method → handler 路由 (7 个 MCP 方法)
└────────┬────────┘
         │ tools/call → PluginRegistry → IPlugin
         ▼
┌─────────────────┐
│   Plugin 层      │  .so 动态加载, 9 个工具 + 1 个资源
└────────┬────────┘
         │ ToolCallResult / ResourceReadResult
         ▼
┌─────────────────┐
│  Serializer 层   │  结构体 → JSON 字符串
└────────┬────────┘
         │
         ▼
stdout / SSE 事件流
```

## HTTP 模式下的聊天架构

```
浏览器 → POST /api/chat
    │
    ▼
ToolOrchestrator (Agent 循环, 最多 10 轮)
    │
    ├─→ tools/list (获取可用工具)
    ├─→ LlmClient::chat_stream (调 LLM API)
    │     ├─ OpenAI:   POST /v1/chat/completions (SSE 流)
    │     └─ Anthropic: POST /v1/messages (SSE 流)
    │
    ├─→ 如果 LLM 返回 tool_calls:
    │     └─→ tools/call → 插件执行 → 结果喂回 LLM → 继续循环
    │
    └─→ 如果 LLM 返回文本:
          └─→ SSE 推送给浏览器 (event: done)
```

## 版本历史

| 版本 | 内容 |
|------|------|
| V1 | CMake 构建, JSON-RPC 2.0, stdio 传输, 基础 MCP 方法 |
| V2 | 插件系统 (dlopen + .so), FilePlugin (file_read/list) |
| V3 | HTTP+SSE 远程传输, 双模式切换 |
| V4 | LLM 代理 (OpenAI API) + 聊天前端 |
| V5 | 4 个新插件, Anthropic API, 插件面板, 工具编排 |
| V6 | 多对话管理, 清理死代码, 代码注释 |
| **V7** | **Resources + Prompts 支持, 9 个 MCP 方法, 3 个 Prompt 模板, 项目重命名, 完善文档** |
