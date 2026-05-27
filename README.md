# TinyWebServer-MCP

基于 [TinyWebServer](https://github.com/qinguoyi/TinyWebServer) 改造的 **MCP (Model Context Protocol) 服务器**，为 AI 客户端提供标准化的工具调用能力。

## 项目路线图

| 版本 | 内容 | 状态 |
|------|------|------|
| **V1** | CMake 构建、JSON-RPC 2.0 协议、stdio 传输、基础 MCP 方法 | ✅ 已完成 |
| **V2** | 插件系统 (dlopen + .so 动态加载) + FilePlugin | ✅ 已完成 |
| **V3** | HTTP+SSE 远程传输 (cpp-httplib) + 双模式切换 | ✅ 已完成 |
| **V4** | LLM 代理 + 聊天前端 (工具编排) | ✅ 已完成 |
| **V5** | 3 个新插件 + Anthropic API + 前端插件面板 | ✅ 已完成 |
| V6 | 多对话管理 (线程池 + 切换 + 日志) | 开发中 |

## V1 新增功能 (2026-05-27)

### 新增模块

```
src/
├── common.h                  extern m_close_log 声明
├── config.h / config.cpp      JSON 配置文件加载
├── main.cpp                   入口：串联各模块
├── mcp/
│   ├── jsonrpc.h              JSON-RPC 2.0 数据结构
│   ├── jsonrpc_parser.h/cpp   JSON-RPC 请求解析器
│   ├── jsonrpc_serializer.h/cpp JSON-RPC 响应序列化器
│   ├── mcp_types.h            MCP 协议类型定义
│   └── mcp_handler.h/cpp      MCP 方法路由器
├── transport/
│   ├── transport.h            传输层抽象基类
│   └── stdio_transport.h/cpp  stdio 传输层实现
├── log/                       异步日志（原项目）
├── lock/                      同步原语（原项目）
└── threadpool/                线程池（已改造为泛型）
```

### 支持的 MCP 方法

| Method | 功能 | 说明 |
|--------|------|------|
| `initialize` | 协议握手 | 返回协议版本、服务器能力、服务器信息 |
| `ping` | 心跳检测 | 返回 `{}` |
| `tools/list` | 工具列表 | V1 返回空列表，插件在 V2 加入 |
| `notifications/initialized` | 初始化完成通知 | 无响应 |

### 传输协议

- **stdio**: 标准输入/输出，`Content-Length: N\r\n\r\n<body>` 帧格式
- 符合 MCP 2024-11-05 协议规范

### 依赖

- **nlohmann/json** (header-only, v3.11.3) — `third_party/nlohmann/json.hpp`
- pthread (系统库)
- C++17

### 构建和运行

```bash
mkdir build && cd build
cmake ..
make

# 测试: 通过 stdio 发送 initialize 请求
BODY='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}'
LEN=${#BODY}
printf 'Content-Length: %d\r\n\r\n%s' "$LEN" "$BODY" | ./mcp_server
```

### 配置

`config.json`（项目根目录）:

```json
{
    "port": 9006,
    "log_file": "ServerLog",
    "close_log": 0,
    "thread_num": 4,
    "plugin_dir": "./plugins"
}
```

### 复用模块

| 模块 | 来源 | 改动 |
|------|------|------|
| `log/` (log.h, log.cpp, block_queue.h) | TinyWebServer | 无 |
| `lock/locker.h` | TinyWebServer | 无 |
| `threadpool/threadpool.h` | TinyWebServer | 改造：`http_conn` → `std::function<void()>` |

### 技术要点

- log 宏引用 `m_close_log` 全局变量，通过 `src/common.h` 声明 + `main.cpp` 定义实现
- 线程池已改为泛型任务执行，接受 `std::function<void()>` 类型
- stdio 传输层 `start()` 为阻塞调用（内部 join 读取线程），主线程等待 stdin EOF 或信号退出

---

## V2 新增功能 (2026-05-27)

### 插件系统

```
src/plugin/
├── plugin_interface.h       IPlugin 抽象基类 + extern "C" dlopen 入口
├── plugin_registry.h/cpp    tool_name → {IPlugin*, ToolDef} 注册表
├── dynamic_library.h/cpp    跨平台 dlopen/LoadLibrary 封装
└── plugin_manager.h/cpp     插件扫描、加载、生命周期管理

plugins/file_plugin/
├── file_plugin.h/cpp        FilePlugin (file_read + file_list)
└── CMakeLists.txt           独立编译为 .so
```

### 插件接口

```cpp
class IPlugin {
    virtual const char* name() const = 0;
    virtual const char* version() const = 0;
    virtual vector<ToolDef> get_tools() const = 0;
    virtual ToolCallResult call_tool(name, args) = 0;
};

// .so 导出 (extern "C" 防止 name mangling)
extern "C" IPlugin* create_plugin();
extern "C" void destroy_plugin(IPlugin*);
```

### 新增 MCP 方法

| Method | 功能 |
|--------|------|
| `tools/list` | 返回已加载插件提供的工具列表 |
| `tools/call` | 按名称调用工具，参数校验后分发到插件 |

### FilePlugin 工具

| 工具 | 参数 | 功能 |
|------|------|------|
| `file_read` | `path: string` (绝对路径) | 读取文件内容 |
| `file_list` | `directory: string` (绝对路径) | 列出目录内容 |

### 验证结果 (5/5 通过)

```
tools/list           → 返回 2 个工具定义
tools/call file_read → 正确返回文件内容
tools/call file_list → 正确列出目录
tools/call 错误路径   → isError=true
tools/call 未知工具   → error -32603
```

---

## V3 新增功能 (2026-05-27)

### HTTP+SSE 远程传输

```
src/transport/
├── http_sse_transport.h/cpp   HTTP+SSE 传输实现
third_party/
└── httplib.h                   cpp-httplib header-only (v0.x)
```

### 传输模式切换

通过 `config.json` 的 `mode` 字段选择：

| mode | 传输方式 | 适用场景 |
|------|----------|----------|
| `"stdio"` | 标准输入/输出 | 本地 Claude Desktop 集成 |
| `"http"` | HTTP+SSE (端口 9006) | Cherry Studio、远程客户端 |

### HTTP 端点

| 端点 | 方法 | 功能 |
|------|------|------|
| `/sse?session_id=xxx` | GET | SSE 长连接，返回 endpoint + message 事件 |
| `/message?session_id=xxx` | POST | JSON-RPC 请求，响应通过 SSE 异步返回 |
| `/health` | GET | 健康检查，返回 "OK" |

### 验证结果 (6/6 通过)

```
health check        → 200 OK
SSE 连接           → event: endpoint (分配 session_id)
POST initialize    → 202 Accepted + SSE event: message (initialize 响应)
POST tools/list    → 202 Accepted + SSE event: message (tools 列表)
POST tools/call    → 202 Accepted + SSE event: message (工具结果)
stdio 模式兼容     → 原有功能正常
```

### 依赖新增

- **cpp-httplib** — header-only HTTP/HTTPS 库 (`third_party/httplib.h`)

---

## V5 新增功能 (2026-05-27)

### 新增插件 (7 个工具)

| 插件 | 工具 | 功能 |
|------|------|------|
| weather_plugin | `query_weather` | 查询城市天气 (wttr.in API) |
| review_plugin | `code_review`, `code_stats` | 代码审查 + 统计 |
| bilibili_plugin | `up_videos`, `up_info`, `hot` | B站UP主视频/热门 (公开API) |

### Anthropic Messages API 适配

- 完整支持 Anthropic 流式格式 (thinking_delta / input_json_delta / tool_use)
- OpenAI↔Anthropic 消息格式自动转换
- DeepSeek 兼容: tool_result 简化为纯文本注入

### 前端插件面板

- 侧边栏显示所有工具 + 开关按钮
- 禁用工具不会传给 LLM
- 开关状态 localStorage 持久化
- 自定义模型名输入 (datalist + 自由输入)

### 工具编排

```
用户消息 → LLM (含 tools)
  → LLM 返回 tool_call → 执行 MCP 工具
  → 结果反馈 LLM → 循环 → 生成最终回复
```

### API 端点新增

| 端点 | 功能 |
|------|------|
| `GET /api/tools` | 返回当前可用工具列表 |

---

## V4 新增功能 (2026-05-27)

### LLM 代理 + 聊天前端

```
src/llm/
├── llm_client.h/cpp           OpenAI 兼容流式 API 客户端
└── tool_orchestrator.h/cpp    工具调用编排循环

chat/
└── chat.html                  单页聊天界面
```

### 聊天 API 端点

| 端点 | 方法 | 功能 |
|------|------|------|
| `/api/chat` | POST | SSE 流式聊天 (含工具编排) |
| `/api/models` | GET | 模型列表 |
| `/chat.html` | GET | 聊天界面 |

### 工具编排流程

```
用户消息 → LlmClient (流式调用 LLM)
  → LLM 返回 tool_calls → McpHandler.tools/call → 执行工具
  → 工具结果反馈给 LLM → 循环
  → LLM 返回文本 → SSE 流式推送给浏览器
```

### 支持的模型后端

- OpenAI API (gpt-4o, gpt-4o-mini)
- Claude API (通过兼容适配)
- Ollama 本地模型 (qwen2.5, llama3 等)
- 任意 OpenAI 兼容 API

### 使用方式

1. 启动服务器: `./mcp_server` (HTTP 模式)
2. 浏览器打开: `http://localhost:9006/chat.html`
3. 填入 API Key + 选择模型
4. 开始对话: "帮我读取 /etc/hostname 文件"

### 验证结果 (5/5 通过)

```
health check     → OK
chat.html        → 正常返回
/api/models      → 返回模型列表
/api/chat        → 缺少 API key 时正确报错
MCP SSE 兼容     → 原有功能正常
```

---

## 原始项目

TinyWebServer — Linux 下 C++ 轻量级 Web 服务器，基于 epoll + 线程池实现 Reactor/Proactor 并发模型。
