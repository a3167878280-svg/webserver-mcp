# TinyMCP 中 JSON-RPC 2.0 与 MCP 请求处理链路详解

这份文档用“老师讲给学生”的方式，从零解释 TinyMCP 这个项目里，你简历上写的这个技术点到底是怎么实现的：

> 基于 JSON-RPC 2.0 实现 MCP 请求处理链路，设计“解析 -> 路由 -> 执行 -> 序列化”四段管道，支持 Tool / Resource / Prompt 调用。

先不要急着背术语。你可以先把整个项目想象成一个“服务台系统”：

```text
客户端发来一张纸条
服务器先看懂纸条内容
再判断纸条要办什么业务
然后找到对应业务员执行
最后把结果写成标准格式还给客户端
```
对应到这个项目，就是：

```text
原始 JSON 字符串
    -> 解析成 JsonRpcRequest
    -> 根据 method 路由到 MCP 处理函数
    -> 调用插件执行 Tool / Resource / Prompt
    -> 包装成 JsonRpcResponse 并序列化成 JSON 字符串
    -> 通过 stdio 或 HTTP+SSE 发回客户端
```

这就是“解析 -> 路由 -> 执行 -> 序列化”四段管道。

---

## 1. 先理解：MCP Server 到底在干什么

TinyMCP 是一个 MCP Server。

MCP Server 的作用是：把本地能力暴露给大模型或 MCP 客户端调用。

比如服务器可以暴露这些能力：

```text
file_read       读取文件
file_list       列目录
grep_file       搜索文件内容
code_review     审查代码
code_stats      统计代码
config://server 读取服务器配置资源
file_analyzer   文件分析 Prompt 模板
```

客户端不会直接调用 C++ 函数，而是发送 JSON-RPC 2.0 请求。

例如，客户端问服务器：“你有哪些工具？”

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/list",
  "params": {}
}
```

客户端要求服务器调用一个工具：

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/call",
  "params": {
    "name": "file_read",
    "arguments": {
      "path": "/tmp/a.txt"
    }
  }
}
```

服务器处理完之后，也会按 JSON-RPC 2.0 的格式返回：

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "文件内容..."
      }
    ],
    "isError": false
  }
}
```

所以你可以先记住一句话：

```text
JSON-RPC 2.0 是通信外壳，MCP 是里面的业务语义。
```

JSON-RPC 负责规定：请求长什么样，响应长什么样，错误怎么表示。

MCP 负责规定：有哪些方法，比如 `tools/list`、`tools/call`、`resources/read`、`prompts/get`。

---

## 2. 项目里主要模块分别负责什么

这个项目和这个技术点相关的核心目录是：

```text
src/main.cpp
src/transport/
src/mcp/
src/plugin/
plugins/
```

可以这样理解它们的分工：

```text
src/transport/  负责收消息和发消息
src/mcp/        负责 JSON-RPC 和 MCP 协议处理
src/plugin/     负责插件加载、插件注册、插件查找
plugins/        具体插件能力，比如文件插件、代码审查插件、天气插件
src/main.cpp    把所有模块组装起来，形成完整请求链路
```

更具体一点：

```text
transport 层：
    StdioTransport      从 stdin 读请求，向 stdout 写响应
    HttpSseTransport    从 HTTP POST 收请求，通过 SSE 推响应

mcp 层：
    JsonRpcParser       把字符串解析成 JsonRpcRequest
    McpHandler          根据 method 路由并处理 MCP 请求
    JsonRpcSerializer   把 JsonRpcResponse 转回 JSON 字符串
    mcp_types.h         定义 Tool / Resource / Prompt 的数据结构

plugin 层：
    IPlugin             插件统一接口
    PluginManager       扫描并加载 .so 插件
    PluginRegistry      建立 Tool / Resource / Prompt 到插件的索引

plugins 目录：
    file_plugin         文件相关工具、资源、Prompt
    review_plugin       代码审查工具和 Prompt
    weather_plugin      天气工具
    command_plugin      命令执行工具
    bilibili_plugin     B 站相关工具
```

---

## 3. 请求从哪里进入项目

TinyMCP 支持两种通信方式：stdio 和 HTTP+SSE。

### 3.1 stdio 模式

代码位置：

```text
src/transport/stdio_transport.cpp
```

stdio 模式下，客户端和服务器通过标准输入输出通信。

MCP stdio 消息不是单纯一行 JSON，而是这种格式：

```text
Content-Length: 85\r\n
\r\n
{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}
```

为什么要有 `Content-Length`？

因为底层是字节流。服务器需要知道这条 JSON 消息到底有多长，否则可能读多或读少。

`StdioTransport::read_loop()` 做了几件事：

```text
1. 一直从 stdin 读字符
2. 先读到 \r\n\r\n，说明 header 结束
3. 从 header 中找到 Content-Length
4. 按 Content-Length 精确读取 JSON body
5. 把完整 JSON 字符串交给上层回调 m_callback
```

关键代码逻辑是：

```cpp
std::string body = read_exact(content_length);

if (m_callback) {
    m_callback(std::move(body));
}
```

这里的 `body` 就是完整 JSON-RPC 请求字符串。

### 3.2 HTTP+SSE 模式

代码位置：

```text
src/transport/http_sse_transport.cpp
```

HTTP 模式下，客户端通过：

```text
POST /message?session_id=xxx
```

发送 JSON-RPC 请求。

服务器不直接在 POST response 里返回 JSON-RPC 结果，而是返回：

```text
202 Accepted
```

真正的结果通过 SSE 长连接推送回客户端。

你可以理解成两条通道：

```text
POST /message  客户端 -> 服务器，负责发请求
GET /sse       服务器 -> 客户端，负责推响应
```

但是注意：无论 stdio 还是 HTTP，它们最后都会调用同一个 `on_message`。

也就是说：

```text
不同传输方式只影响“消息怎么进来、怎么出去”
不影响 MCP 请求本身怎么处理
```

这是一个比较好的分层设计。

---

## 4. 核心管道在 main.cpp 里

代码位置：

```text
src/main.cpp
```

最关键的是这个 `on_message` 回调：

```cpp
auto on_message = [&](transport::Transport& t, const std::string& raw_json) {
    auto maybe_req = mcp::JsonRpcParser::parse(raw_json);
    if (!maybe_req.has_value()) {
        auto err = mcp::JsonRpcParser::make_parse_error(nlohmann::json(nullptr));
        t.send(mcp::JsonRpcSerializer::serialize(err));
        return;
    }

    auto maybe_resp = mcp_handler.handle(maybe_req.value());
    if (maybe_resp.has_value()) {
        t.send(mcp::JsonRpcSerializer::serialize(maybe_resp.value()));
    }
};
```

如果你是小白，可以把它翻译成中文：

```text
客户端发来一个 raw_json 字符串

第一步：JsonRpcParser::parse(raw_json)
把字符串解析成 JsonRpcRequest

第二步：mcp_handler.handle(request)
根据 request.method 找到对应处理函数并执行

第三步：JsonRpcSerializer::serialize(response)
把响应对象转换成 JSON 字符串

第四步：t.send(json)
通过当前传输层发回去
```

这就是你简历里的四段管道。

不过严格说，`main.cpp` 里注释写的是三步，因为它把“路由”和“执行”合在 `McpHandler::handle()` 里了。

如果从设计上拆开，就是：

```text
解析：JsonRpcParser::parse
路由：McpHandler 根据 method 查 m_routes
执行：调用具体 handler，最终调用插件
序列化：JsonRpcSerializer::serialize
```

---

## 5. 第一段：解析 JSON-RPC 请求

代码位置：

```text
src/mcp/jsonrpc_parser.cpp
src/mcp/jsonrpc.h
```

### 5.1 JSON-RPC 请求长什么样

JSON-RPC 2.0 请求通常长这样：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/list",
  "params": {}
}
```

字段含义：

```text
jsonrpc  固定为 "2.0"
id       请求 ID，响应时要原样带回去
method   要调用的方法名，比如 tools/list
params   方法参数
```

在代码里，对应这个结构体：

```cpp
struct JsonRpcRequest {
    std::string jsonrpc;
    std::string method;
    nlohmann::json params;
    nlohmann::json id;

    bool is_notification() const { return id.is_null(); }
};
```

这里有一个细节：如果 `id` 是 null，就表示这是 notification。

notification 的意思是：客户端只是通知服务器一件事，不需要服务器回复。

例如：

```json
{
  "jsonrpc": "2.0",
  "method": "notifications/initialized"
}
```

这个请求没有 `id`，所以服务器处理完不需要返回响应。

### 5.2 JsonRpcParser::parse 做什么

`JsonRpcParser::parse()` 的输入是字符串：

```cpp
std::optional<JsonRpcRequest> JsonRpcParser::parse(const std::string& raw_json)
```

它做这些检查：

```text
1. raw_json 必须是合法 JSON
2. JSON 必须是 object
3. 必须包含 jsonrpc 字段
4. jsonrpc 必须等于 "2.0"
5. 必须包含 method 字段
6. method 必须是字符串
7. params 可选
8. id 可选
```

比如收到：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/list",
  "params": {}
}
```

解析后得到：

```text
JsonRpcRequest
    jsonrpc = "2.0"
    id = 1
    method = "tools/list"
    params = {}
```

如果解析失败，就返回 `std::nullopt`。

`main.cpp` 收到 `std::nullopt` 后，会构造 JSON-RPC 标准错误：

```cpp
auto err = mcp::JsonRpcParser::make_parse_error(nlohmann::json(nullptr));
```

这个错误码是：

```text
-32700 Parse error
```

项目还定义了这些 JSON-RPC 标准错误码：

```text
-32700  Parse error
-32600  Invalid Request
-32601  Method not found
-32602  Invalid params
-32603  Internal error
```

这些定义在 `src/mcp/jsonrpc.h` 里。

---

## 6. 第二段：根据 method 路由

代码位置：

```text
src/mcp/mcp_handler.cpp
src/mcp/mcp_handler.h
```

解析完成后，请求已经变成了 `JsonRpcRequest`。

下一步就要看：

```text
request.method 到底是什么？
```

如果是：

```text
tools/list
```

就执行列出工具。

如果是：

```text
tools/call
```

就执行工具调用。

如果是：

```text
resources/read
```

就读取资源。

这个“根据 method 找处理函数”的过程，就是路由。

### 6.1 路由表 m_routes

`McpHandler::register_handlers()` 里注册了路由表：

```cpp
m_routes["initialize"] = [this](const nlohmann::json& p) {
    return handle_initialize(p);
};

m_routes["tools/list"] = [this](const nlohmann::json& p) {
    return handle_tools_list(p);
};

m_routes["tools/call"] = [this](const nlohmann::json& p) {
    return handle_tools_call(p);
};

m_routes["resources/list"] = [this](const nlohmann::json& p) {
    return handle_resources_list(p);
};

m_routes["resources/read"] = [this](const nlohmann::json& p) {
    return handle_resources_read(p);
};

m_routes["prompts/list"] = [this](const nlohmann::json& p) {
    return handle_prompts_list(p);
};

m_routes["prompts/get"] = [this](const nlohmann::json& p) {
    return handle_prompts_get(p);
};
```

这就是一张“方法名 -> 处理函数”的表。

你可以把它想象成餐厅菜单：

```text
method = "tools/list"      点的是“列出工具”这道菜
method = "resources/read"  点的是“读取资源”这道菜
method = "prompts/get"     点的是“获取 Prompt”这道菜
```

### 6.2 McpHandler::handle 怎么查表

核心逻辑是：

```cpp
auto it = m_routes.find(request.method);
if (it == m_routes.end()) {
    auto err = JsonRpcParser::make_method_not_found(request.id, request.method);
    return err;
}

result = it->second(request.params);
```

翻译成中文：

```text
用 request.method 去 m_routes 里找
如果找不到，返回 Method not found
如果找到了，就调用对应处理函数，把 params 传进去
```

例如：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/list",
  "params": {}
}
```

会走到：

```cpp
handle_tools_list(params)
```

例如：

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "prompts/get",
  "params": {
    "name": "code_review",
    "arguments": {
      "file_path": "/tmp/a.cpp"
    }
  }
}
```

会走到：

```cpp
handle_prompts_get(params)
```

这就是“路由”。

---

## 7. 第三段：执行 Tool / Resource / Prompt

路由找到处理函数之后，就要真正执行业务。

TinyMCP 支持三类 MCP 能力：

```text
Tool      工具，表示“做一件事”
Resource  资源，表示“读一个东西”
Prompt    模板，表示“生成一段预设对话”
```

它们的底层执行都依赖插件系统。

---

## 8. 插件系统：为什么需要它

如果所有工具都写死在 `McpHandler` 里，代码会变得很乱。

比如：

```text
文件工具写在 McpHandler
天气工具写在 McpHandler
代码审查工具写在 McpHandler
命令执行工具写在 McpHandler
B 站工具写在 McpHandler
```

这样 `McpHandler` 会越来越大，而且每加一个工具都要改核心代码。

所以项目设计了插件系统。

插件系统的思路是：

```text
McpHandler 不关心具体工具怎么实现
McpHandler 只负责根据请求找到插件
插件自己负责真正执行功能
```

插件接口定义在：

```text
src/plugin/plugin_interface.h
```

核心接口是：

```cpp
class IPlugin {
public:
    virtual std::vector<mcp::ToolDef> get_tools() const = 0;

    virtual mcp::ToolCallResult call_tool(
        const std::string& tool_name,
        const nlohmann::json& arguments) = 0;

    virtual std::vector<mcp::ResourceDef> get_resources() const {
        return {};
    }

    virtual mcp::ResourceReadResult read_resource(const std::string& uri) {
        return {};
    }

    virtual std::vector<mcp::PromptDef> get_prompts() const {
        return {};
    }

    virtual mcp::PromptGetResult get_prompt(
        const std::string& name,
        const nlohmann::json& arguments) {
        return {};
    }
};
```

这段代码是什么意思？

```text
每个插件必须告诉服务器：我有哪些工具
每个插件必须能执行自己的工具
每个插件可以选择性暴露资源
每个插件可以选择性暴露 Prompt
```

`get_tools()` 和 `call_tool()` 是纯虚函数，必须实现。

`get_resources()`、`read_resource()`、`get_prompts()`、`get_prompt()` 有默认空实现，不需要的插件可以不重写。

---

## 9. 插件如何被注册到系统里

插件加载和注册分两个模块：

```text
PluginManager   负责把 .so 插件加载进来
PluginRegistry  负责记录插件提供了哪些 Tool / Resource / Prompt
```

### 9.1 PluginManager 做什么

代码位置：

```text
src/plugin/plugin_manager.cpp
```

它会扫描插件目录，找到 `.so` 文件，然后：

```text
1. dlopen 加载动态库
2. dlsym 找 create_plugin 函数
3. 调用 create_plugin 创建插件对象
4. 把插件对象交给 PluginRegistry 注册
```

也就是说，插件不是静态写死到主程序里的，而是运行时动态加载的。

### 9.2 PluginRegistry 做什么

代码位置：

```text
src/plugin/plugin_registry.cpp
```

`PluginRegistry::register_plugin()` 会问插件三件事：

```cpp
auto tools = plugin->get_tools();
auto resources = plugin->get_resources();
auto prompts = plugin->get_prompts();
```

然后建立三张表：

```text
m_tools      工具名 -> 插件对象
m_resources  资源 URI -> 插件对象
m_prompts    Prompt 名称 -> 插件对象
```

例如 `FilePlugin` 注册之后，可能是：

```text
m_tools["file_read"]       = FilePlugin
m_tools["file_list"]       = FilePlugin
m_tools["grep_file"]       = FilePlugin

m_resources["config://server"] = FilePlugin

m_prompts["file_analyzer"] = FilePlugin
```

例如 `ReviewPlugin` 注册之后，可能是：

```text
m_tools["code_review"] = ReviewPlugin
m_tools["code_stats"]  = ReviewPlugin

m_prompts["code_review"]  = ReviewPlugin
m_prompts["code_explain"] = ReviewPlugin
```

有了这些表，后面执行时就很快：

```text
调用工具时，根据工具名找到插件
读取资源时，根据 URI 找到插件
获取 Prompt 时，根据 Prompt 名称找到插件
```

---

## 10. Tool 是怎么实现的

Tool 表示“执行一个动作”。

比如：

```text
读取文件
列目录
搜索文件
查天气
审查代码
```

### 10.1 Tool 的数据结构

定义在：

```text
src/mcp/mcp_types.h
```

工具定义：

```cpp
struct ToolDef {
    std::string name;
    std::string description;
    nlohmann::json inputSchema;
};
```

字段含义：

```text
name         工具名，比如 file_read
description  工具说明，告诉客户端这个工具干什么
inputSchema  参数 schema，告诉客户端这个工具需要什么参数
```

工具执行结果：

```cpp
struct ToolCallResult {
    std::vector<TextContent> content;
    bool isError = false;
};
```

字段含义：

```text
content  返回内容数组
isError  这次工具调用是否失败
```

### 10.2 tools/list 怎么执行

客户端请求：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/list",
  "params": {}
}
```

路由到：

```cpp
McpHandler::handle_tools_list()
```

代码逻辑：

```cpp
auto all = m_registry->get_all_tools();
for (auto& tool : all) {
    tools_arr.push_back(tool.to_json());
}
```

翻译成中文：

```text
去 PluginRegistry 拿到所有已注册工具
把每个工具转成 JSON
组成 tools 数组返回
```

返回大概是：

```json
{
  "tools": [
    {
      "name": "file_read",
      "description": "Read the contents of a file",
      "inputSchema": {
        "type": "object",
        "properties": {
          "path": {
            "type": "string"
          }
        },
        "required": ["path"]
      }
    }
  ]
}
```

### 10.3 tools/call 怎么执行

客户端请求：

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/call",
  "params": {
    "name": "file_read",
    "arguments": {
      "path": "/tmp/a.txt"
    }
  }
}
```

路由到：

```cpp
McpHandler::handle_tools_call()
```

核心逻辑：

```cpp
ToolCallParams p = ToolCallParams::from_json(params);

auto result = m_registry->call_tool(p.name, p.arguments);
if (!result.has_value()) {
    throw std::runtime_error("Unknown tool: " + p.name);
}
return result->to_json();
```

翻译成中文：

```text
从 params 里取出工具名 name
从 params 里取出参数 arguments
去 PluginRegistry 里根据工具名找到插件
调用插件的 call_tool(name, arguments)
把插件返回结果转成 JSON
```

再往下看 `PluginRegistry::call_tool()`：

```cpp
auto it = m_tools.find(tool_name);
if (it == m_tools.end()) {
    return std::nullopt;
}
return it->second.plugin->call_tool(tool_name, arguments);
```

也就是：

```text
m_tools 里查工具名
查到了就调用对应插件的 call_tool
查不到就返回空
```

### 10.4 FilePlugin 的 Tool 例子

代码位置：

```text
plugins/file_plugin/file_plugin.cpp
```

`FilePlugin::get_tools()` 返回三个工具：

```cpp
return {
    make_tool("file_read", "Read the contents of a file", read_schema),
    make_tool("file_list", "List files and directories", list_schema),
    make_tool("grep_file", "Search for a pattern in a file", grep_schema)
};
```

这一步是在告诉服务器：

```text
我 FilePlugin 提供 file_read、file_list、grep_file 这三个工具
```

然后 `FilePlugin::call_tool()` 根据工具名分发：

```cpp
if (tool_name == "file_read") {
    return handle_file_read(args);
} else if (tool_name == "file_list") {
    return handle_file_list(args);
} else if (tool_name == "grep_file") {
    return handle_grep_file(args);
}
```

这就是插件内部的小路由。

完整调用链是：

```text
客户端 tools/call
    -> McpHandler::handle_tools_call
    -> PluginRegistry::call_tool("file_read", args)
    -> FilePlugin::call_tool("file_read", args)
    -> FilePlugin::handle_file_read(args)
    -> 返回 ToolCallResult
    -> 序列化成 JSON-RPC response
```

---

## 11. Resource 是怎么实现的

Resource 表示“服务器暴露出来，可以被客户端读取的数据”。

Tool 和 Resource 的区别可以这样理解：

```text
Tool 是动作：请你帮我做一件事
Resource 是数据：你这里有什么东西可以让我读
```

例如：

```text
file_read 是 Tool，因为它需要客户端主动传 path，然后执行读取动作
config://server 是 Resource，因为服务器提前声明“我有这个配置资源”，客户端可以浏览后读取
```

### 11.1 Resource 的数据结构

定义在：

```text
src/mcp/mcp_types.h
```

资源定义：

```cpp
struct ResourceDef {
    std::string uri;
    std::string name;
    std::string description;
    std::string mimeType = "text/plain";
};
```

字段含义：

```text
uri          资源唯一标识，比如 config://server
name         资源名称
description  资源说明
mimeType     内容类型，比如 text/plain 或 application/json
```

资源读取结果：

```cpp
struct ResourceReadResult {
    std::vector<ResourceContent> contents;
};
```

### 11.2 resources/list 怎么执行

客户端请求：

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "resources/list",
  "params": {}
}
```

路由到：

```cpp
McpHandler::handle_resources_list()
```

核心逻辑：

```cpp
auto all = m_registry->get_all_resources();
for (auto& r : all) {
    arr.push_back(r.to_json());
}
return {{"resources", std::move(arr)}};
```

翻译成中文：

```text
从注册表拿到所有资源定义
转成 JSON 数组
返回给客户端
```

返回示例：

```json
{
  "resources": [
    {
      "uri": "config://server",
      "name": "服务器配置",
      "description": "当前 MCP 服务器运行配置",
      "mimeType": "application/json"
    }
  ]
}
```

### 11.3 resources/read 怎么执行

客户端请求：

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "resources/read",
  "params": {
    "uri": "config://server"
  }
}
```

路由到：

```cpp
McpHandler::handle_resources_read()
```

核心逻辑：

```cpp
ResourceReadParams p = ResourceReadParams::from_json(params);

auto result = m_registry->read_resource(p.uri);
if (!result.has_value()) {
    throw std::runtime_error("Unknown resource: " + p.uri);
}
return result->to_json();
```

翻译成中文：

```text
从 params 里取出 uri
去 PluginRegistry 里根据 uri 找插件
调用插件的 read_resource(uri)
把读取结果返回
```

### 11.4 FilePlugin 的 Resource 例子

`FilePlugin::get_resources()` 暴露了一个资源：

```cpp
mcp::ResourceDef config_res;
config_res.uri = "config://server";
config_res.name = "服务器配置";
config_res.description = "当前 MCP 服务器运行配置 (config.json 内容)";
config_res.mimeType = "application/json";
```

这一步的意思是：

```text
FilePlugin 告诉服务器：我这里有一个资源，URI 是 config://server
```

然后读取资源时：

```cpp
if (uri == "config://server") {
    return read_file_resource("config.json", "application/json");
}
```

完整调用链是：

```text
客户端 resources/read
    -> McpHandler::handle_resources_read
    -> PluginRegistry::read_resource("config://server")
    -> FilePlugin::read_resource("config://server")
    -> read_file_resource("config.json")
    -> 返回 ResourceReadResult
    -> 序列化成 JSON-RPC response
```

---

## 12. Prompt 是怎么实现的

Prompt 表示“预设对话模板”。

它不是执行工具，也不是读取资源，而是生成一段可以交给大模型的消息。

比如代码审查 Prompt：

```text
你是一位资深代码审查专家。请审查以下代码...
```

Prompt 可以带参数。

例如：

```text
file_path = /tmp/a.cpp
language = cpp
focus = security
```

服务器根据这些参数生成完整 Prompt 消息。

### 12.1 Prompt 的数据结构

定义在：

```text
src/mcp/mcp_types.h
```

Prompt 定义：

```cpp
struct PromptDef {
    std::string name;
    std::string description;
    std::vector<PromptArgument> arguments;
};
```

字段含义：

```text
name         Prompt 名称，比如 code_review
description  Prompt 说明
arguments    Prompt 支持哪些参数
```

Prompt 获取结果：

```cpp
struct PromptGetResult {
    std::string description;
    std::vector<PromptMessage> messages;
};
```

其中 `messages` 就是要交给大模型的消息数组。

### 12.2 prompts/list 怎么执行

客户端请求：

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "method": "prompts/list",
  "params": {}
}
```

路由到：

```cpp
McpHandler::handle_prompts_list()
```

核心逻辑：

```cpp
auto all = m_registry->get_all_prompts();
for (auto& p : all) {
    arr.push_back(p.to_json());
}
return {{"prompts", std::move(arr)}};
```

翻译成中文：

```text
从注册表拿所有 Prompt 定义
转成 JSON 数组
返回给客户端
```

返回示例：

```json
{
  "prompts": [
    {
      "name": "code_review",
      "description": "按专业标准审查代码",
      "arguments": [
        {
          "name": "file_path",
          "description": "要审查的代码文件路径",
          "required": true
        }
      ]
    }
  ]
}
```

### 12.3 prompts/get 怎么执行

客户端请求：

```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "method": "prompts/get",
  "params": {
    "name": "code_review",
    "arguments": {
      "file_path": "/tmp/a.cpp",
      "language": "cpp",
      "focus": "security"
    }
  }
}
```

路由到：

```cpp
McpHandler::handle_prompts_get()
```

核心逻辑：

```cpp
PromptGetParams p = PromptGetParams::from_json(params);

auto result = m_registry->get_prompt(p.name, p.arguments);
if (!result.has_value()) {
    throw std::runtime_error("Unknown prompt: " + p.name);
}
return result->to_json();
```

翻译成中文：

```text
从 params 里取出 Prompt 名称 name
从 params 里取出参数 arguments
去 PluginRegistry 里根据 name 找插件
调用插件的 get_prompt(name, arguments)
插件动态生成 Prompt 消息
返回给客户端
```

### 12.4 ReviewPlugin 的 Prompt 例子

代码位置：

```text
plugins/review_plugin/review_plugin.cpp
```

`ReviewPlugin::get_prompts()` 返回两个 Prompt：

```cpp
p1.name = "code_review";
p1.description = "按专业标准审查代码，检查 bug、安全漏洞、性能问题和风格";

p2.name = "code_explain";
p2.description = "逐行解释代码逻辑，适合学习或 code review 前理解代码";
```

`ReviewPlugin::get_prompt()` 会根据参数生成消息。

比如 `code_review` 会生成类似这样的内容：

```text
你是一位资深代码审查专家。请审查以下代码。

审查要求:
1. 安全检查
2. 性能分析
3. 代码风格
4. 逻辑错误

请给出: 问题列表 + 严重程度 + 修复建议
文件: /tmp/a.cpp
请用 file_read 工具读取该文件后进行审查。
```

完整调用链是：

```text
客户端 prompts/get
    -> McpHandler::handle_prompts_get
    -> PluginRegistry::get_prompt("code_review", args)
    -> ReviewPlugin::get_prompt("code_review", args)
    -> 生成 PromptGetResult
    -> 序列化成 JSON-RPC response
```

---

## 13. 第四段：序列化响应

代码位置：

```text
src/mcp/jsonrpc.h
src/mcp/jsonrpc_serializer.cpp
```

业务执行完成后，服务器不能直接把 C++ 对象发给客户端。

它必须转换成 JSON-RPC 2.0 响应格式。

响应结构体是：

```cpp
struct JsonRpcResponse {
    std::string jsonrpc = "2.0";
    nlohmann::json result;
    JsonRpcError error;
    nlohmann::json id;
};
```

成功响应长这样：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "tools": []
  }
}
```

失败响应长这样：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -32601,
    "message": "Method not found: xxx"
  }
}
```

`JsonRpcResponse::to_json()` 负责决定返回 `result` 还是 `error`：

```cpp
if (is_error()) {
    j["error"] = error.to_json();
} else {
    j["result"] = result;
}
```

最后 `JsonRpcSerializer::serialize()` 做一件非常简单的事：

```cpp
std::string JsonRpcSerializer::serialize(const JsonRpcResponse& response) {
    return response.to_json().dump();
}
```

也就是：

```text
C++ 响应对象 -> nlohmann::json -> JSON 字符串
```

然后 `main.cpp` 调用：

```cpp
t.send(mcp::JsonRpcSerializer::serialize(maybe_resp.value()));
```

把字符串交给传输层发送。

如果是 stdio，`StdioTransport::send()` 会加上：

```text
Content-Length: xxx\r\n\r\n
```

如果是 HTTP+SSE，`HttpSseTransport::send()` 会把响应放进对应 session 的队列，再由 SSE 通道推给客户端。

---

## 14. 用一个完整 tools/call 例子串起来

假设客户端发请求：

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "method": "tools/call",
  "params": {
    "name": "file_read",
    "arguments": {
      "path": "/tmp/a.txt"
    }
  }
}
```

完整过程如下：

```text
1. Transport 收到原始 JSON 字符串

2. main.cpp 的 on_message 被调用
   raw_json = 上面的 JSON 字符串

3. JsonRpcParser::parse(raw_json)
   解析出：
   method = "tools/call"
   id = 10
   params = {"name":"file_read", "arguments":{"path":"/tmp/a.txt"}}

4. McpHandler::handle(request)
   用 method = "tools/call" 去 m_routes 查表
   找到 handle_tools_call

5. handle_tools_call(params)
   从 params 中解析出：
   name = "file_read"
   arguments = {"path":"/tmp/a.txt"}

6. PluginRegistry::call_tool("file_read", arguments)
   在 m_tools 里找到 file_read 属于 FilePlugin

7. FilePlugin::call_tool("file_read", arguments)
   插件内部判断工具名是 file_read
   调用 handle_file_read(arguments)

8. handle_file_read(arguments)
   取出 path = "/tmp/a.txt"
   打开文件
   读取内容
   包装成 ToolCallResult

9. McpHandler 把 ToolCallResult 转成 result JSON

10. JsonRpcResponse 包装响应
    jsonrpc = "2.0"
    id = 10
    result = 工具返回内容

11. JsonRpcSerializer::serialize(response)
    把响应对象 dump 成 JSON 字符串

12. Transport::send(json)
    发回客户端
```

最终响应类似：

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "result": {
    "isError": false,
    "content": [
      {
        "type": "text",
        "text": "文件内容..."
      }
    ]
  }
}
```

---

## 15. 用一个完整 resources/read 例子串起来

客户端发请求：

```json
{
  "jsonrpc": "2.0",
  "id": 11,
  "method": "resources/read",
  "params": {
    "uri": "config://server"
  }
}
```

完整过程：

```text
1. Transport 收到 JSON 字符串
2. JsonRpcParser 解析成 JsonRpcRequest
3. McpHandler 根据 method = resources/read 找到 handle_resources_read
4. handle_resources_read 从 params 取出 uri = config://server
5. PluginRegistry::read_resource("config://server")
6. 注册表发现这个 URI 属于 FilePlugin
7. FilePlugin::read_resource("config://server")
8. FilePlugin 读取 config.json
9. 返回 ResourceReadResult
10. McpHandler 包成 JSON-RPC response
11. JsonRpcSerializer 序列化
12. Transport 发回客户端
```

响应类似：

```json
{
  "jsonrpc": "2.0",
  "id": 11,
  "result": {
    "contents": [
      {
        "uri": "file:///.../config.json",
        "mimeType": "application/json",
        "text": "{...配置内容...}"
      }
    ]
  }
}
```

---

## 16. 用一个完整 prompts/get 例子串起来

客户端发请求：

```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "method": "prompts/get",
  "params": {
    "name": "code_review",
    "arguments": {
      "file_path": "/tmp/a.cpp",
      "language": "cpp",
      "focus": "security"
    }
  }
}
```

完整过程：

```text
1. Transport 收到 JSON 字符串
2. JsonRpcParser 解析成 JsonRpcRequest
3. McpHandler 根据 method = prompts/get 找到 handle_prompts_get
4. handle_prompts_get 解析 name = code_review，arguments = {...}
5. PluginRegistry::get_prompt("code_review", arguments)
6. 注册表找到 ReviewPlugin
7. ReviewPlugin::get_prompt("code_review", arguments)
8. ReviewPlugin 根据 file_path、language、focus 生成 PromptMessage
9. 返回 PromptGetResult
10. McpHandler 包成 JSON-RPC response
11. JsonRpcSerializer 序列化
12. Transport 发回客户端
```

响应类似：

```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "result": {
    "description": "审查代码: /tmp/a.cpp",
    "messages": [
      {
        "role": "user",
        "content": {
          "type": "text",
          "text": "你是一位资深代码审查专家。请审查以下代码..."
        }
      }
    ]
  }
}
```

---

## 17. 模块之间的关系图

可以用这张图记住整个项目：

```text
客户端
  |
  | JSON-RPC 请求
  v
Transport 层
  |  StdioTransport / HttpSseTransport
  |  负责收发消息，不理解业务
  v
main.cpp:on_message
  |  统一处理入口
  v
JsonRpcParser
  |  raw_json -> JsonRpcRequest
  v
McpHandler
  |  根据 method 路由
  v
具体 MCP handler
  |  tools/list
  |  tools/call
  |  resources/list
  |  resources/read
  |  prompts/list
  |  prompts/get
  v
PluginRegistry
  |  根据工具名 / URI / Prompt 名称查插件
  v
具体插件
  |  FilePlugin
  |  ReviewPlugin
  |  WeatherPlugin
  |  CommandPlugin
  v
返回 MCP 结果对象
  v
JsonRpcResponse
  v
JsonRpcSerializer
  |  response -> JSON 字符串
  v
Transport::send
  |
  v
客户端收到响应
```

---

## 18. 面试时怎么讲这个技术点

可以这样讲：

```text
这个项目里我基于 JSON-RPC 2.0 实现了一套 MCP 请求处理链路。

传输层支持 stdio 和 HTTP+SSE，但它们最终都会把完整 JSON-RPC 字符串交给 main.cpp 里的统一 on_message 回调。

on_message 里先用 JsonRpcParser 校验并解析 JSON-RPC 2.0 请求，生成 JsonRpcRequest。

然后交给 McpHandler，根据 request.method 在 m_routes 路由表里找到对应 MCP 方法处理函数，比如 tools/list、tools/call、resources/read、prompts/get。

执行阶段没有把能力写死在 Handler 里，而是通过 PluginRegistry 查找插件。工具调用按 tool name 查，资源读取按 URI 查，Prompt 获取按 prompt name 查，最后调用插件的 call_tool、read_resource 或 get_prompt。

执行结果会被包装成 JsonRpcResponse，保留原请求 id，再通过 JsonRpcSerializer 转成 JSON 字符串，由传输层发回客户端。

这样做的好处是协议解析、方法路由、能力执行、响应序列化四层职责清晰。后续新增工具、资源或 Prompt，只需要实现插件接口，不需要改主请求链路。
```

如果面试官继续问“Tool、Resource、Prompt 有什么区别”，可以这样答：

```text
Tool 是动作，比如 file_read、code_review，客户端通过 tools/call 主动调用。

Resource 是服务器暴露的数据，比如 config://server，客户端先 resources/list 发现资源，再 resources/read 读取内容。

Prompt 是对话模板，比如 code_review prompt，客户端通过 prompts/get 传入参数，服务器返回预设 messages。

在代码上三者都通过 IPlugin 接口扩展，但注册索引不同：Tool 用 name 索引，Resource 用 uri 索引，Prompt 用 name 索引。
```

---

## 19. 这个实现的亮点

这个技术点可以拆成几个亮点：

```text
1. 协议层和传输层解耦
   stdio 和 HTTP+SSE 都复用同一个 on_message 处理链路。

2. JSON-RPC 2.0 结构清晰
   请求、响应、错误码都有独立结构体，错误能统一包装。

3. method 路由集中管理
   McpHandler 维护 m_routes，新增 MCP 方法只要注册新 handler。

4. 执行能力插件化
   Tool / Resource / Prompt 都走 PluginRegistry，不写死在核心 Handler。

5. 三类 MCP 能力统一建模
   mcp_types.h 中定义 ToolDef、ResourceDef、PromptDef 等协议类型。

6. 扩展成本低
   新增一个插件，只要实现 IPlugin 接口并编译成 .so，主程序加载后自动注册。
```

---

## 20. 也要知道当前实现的不足

面试时如果能主动说出不足，会显得你真的理解项目。

当前实现有这些可以优化的点：

```text
1. 没有支持 JSON-RPC batch request
   JSON-RPC 2.0 允许一次发送数组形式的多个请求，但当前 parser 只接受 object。

2. 参数校验比较轻量
   很多 params 字段只是 value("xxx", 默认值)，缺字段时不一定返回 -32602，而可能到插件里才报错。

3. initialize 的能力声明和实际支持略有注释不一致
   代码实际注册了 resources 和 prompts 方法，能力结构里也有 resources/prompts，但注释里有“只支持 tools”的旧说法。

4. 插件跨 .so 传 STL 和 nlohmann::json 有 ABI 风险
   要求主程序和插件使用相同编译器、相同 C++ 标准库版本。

5. Resource 当前主要实现了文本内容
   blob 类型资源还没有完整实现。
```

这些不足不是否定项目，而是说明你知道工程边界。

---

## 21. 最后用一句话总结

这个项目的核心设计可以总结成：

```text
用 JSON-RPC 2.0 作为统一通信协议，用 McpHandler 做 method 路由，用 PluginRegistry 做能力分发，把 Tool / Resource / Prompt 三类 MCP 能力插件化，最后统一包装成 JSON-RPC 响应返回。
```

如果你只记一条主线，就记这个：

```text
Transport 收消息
    -> JsonRpcParser 解析
    -> McpHandler 路由
    -> PluginRegistry 找插件
    -> Plugin 执行能力
    -> JsonRpcResponse 包装
    -> JsonRpcSerializer 序列化
    -> Transport 发响应
```
