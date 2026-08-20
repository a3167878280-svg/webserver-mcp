# 第 2 讲：源码展开（JSON-RPC 请求主干）

> 对应前置阅读：[第2讲：直接MCP请求的一次往返](第2讲：直接MCP请求的一次往返.md)。
>
> 目标：把“JSON 文本 -> 协议请求 -> 路由 -> 插件 -> 响应文本”的抽象流程，映射到实际函数、数据结构和错误分支。

## 1. 建议的阅读顺序

按这一顺序打开源码，视线不会跳跃：

```text
src/main.cpp
  -> src/mcp/jsonrpc.h
  -> src/mcp/jsonrpc_parser.cpp
  -> src/mcp/mcp_handler.cpp
  -> src/plugin/plugin_registry.cpp
  -> src/mcp/jsonrpc_serializer.cpp
```

主调用链是：

```text
main 中的 on_message
  -> JsonRpcParser::parse
  -> McpHandler::handle
  -> 某个 handle_xxx
  -> PluginRegistry 的查询或调用
  -> JsonRpcSerializer::serialize
  -> Transport::send
```

`on_message` 在 [src/main.cpp:90](../src/main.cpp#L90) 被定义一次，并同时交给 stdio 与 HTTP+SSE 两种传输实现。这正是两种入口共享协议主干的落点。

## 2. 先认识管道中的核心类型

### `JsonRpcRequest`

[src/mcp/jsonrpc.h:58](../src/mcp/jsonrpc.h#L58) 定义的 `JsonRpcRequest` 是解析器的输出，也是路由器的输入：

```cpp
struct JsonRpcRequest {
    std::string jsonrpc;
    std::string method;
    nlohmann::json params;
    nlohmann::json id;
};
```

字段含义与前置文档一致，但源码有一个重要实现细节：`nlohmann::json` 默认构造为 `null`。因此解析器未填 `id` 时，`id` 保持 `null`，`is_notification()` 就会返回 `true`。

### `JsonRpcResponse`

同一文件的 [71-90 行](../src/mcp/jsonrpc.h#L71) 定义响应。它同时持有 `result` 和 `error` 字段，但 `to_json()` 根据 `error.code != 0` 二选一输出：

```text
error.code == 0  -> 输出 result
error.code != 0  -> 输出 error
```

这使“结构体里有两个字段”不等于“JSON 响应里同时有 result 与 error”。

### `std::optional`

解析器和路由器都用 `std::optional` 表示“本函数可能没有正常对象可交给下一阶段”：

```text
parse() 返回 nullopt
  -> 原始文本不能形成请求

handle() 返回 nullopt
  -> 已处理的是通知，不应该发送响应
```

这两个 `nullopt` 的语义完全不同，阅读时不能混为一谈。

## 3. 入口：`on_message` 如何组装三段管道

`on_message` 接收两个参数：

```cpp
transport::Transport& t
const std::string& raw_json
```

其中 `raw_json` 已经是传输层拼好的完整 JSON 文本；`t` 是对具体传输方式的抽象引用，后续只用它的 `send()` 返回结果。

源码在 [src/main.cpp:90-107](../src/main.cpp#L90) 依次执行：

1. `JsonRpcParser::parse(raw_json)`：文本转 `JsonRpcRequest`。
2. 若返回空，调用 `make_parse_error(null)` 构造错误，再序列化和发送。
3. 否则把请求交给 `mcp_handler.handle()`。
4. `handle()` 返回响应时，序列化并发送；返回空时什么也不发送。

用伪代码重写这段逻辑：

```cpp
req = parse(raw_json)
if (!req) {
    send(serialize(parse_error(null)))
    return
}

resp = handle(*req)
if (resp) {
    send(serialize(*resp))
}
```

这段 lambda 捕获 `mcp_handler`，但不把传输方式写死。因此 `StdioTransport` 与 `HttpSseTransport` 都能通过各自的回调把文本送进同一段逻辑。

## 4. 解析：`JsonRpcParser::parse` 逐行做了什么

函数在 [src/mcp/jsonrpc_parser.cpp:28](../src/mcp/jsonrpc_parser.cpp#L28)。输入是 `const std::string& raw_json`，输出是 `optional<JsonRpcRequest>`。

### 阶段 A：把文本读成 JSON 值

```cpp
nlohmann::json j;
try {
    j = nlohmann::json::parse(raw_json);
} catch (...) {
    return std::nullopt;
}
```

例如半截 JSON、缺少引号、错误逗号都会在这里返回空。

### 阶段 B：检查 JSON-RPC 最小形状

函数随后依次确认：

```text
j 必须是 object
j["jsonrpc"] 必须是字符串 "2.0"
j["method"] 必须存在且是字符串
```

任一检查失败都返回 `nullopt`。`params` 与 `id` 不属于这里的必填项：

```text
params 缺失 -> 保持 null
id 缺失     -> 保持 null，因此成为通知
```

### 阶段 C：填充 C++ 请求对象

通过校验后，函数创建 `JsonRpcRequest req`，只复制 `method`，并在字段存在时复制 `params` 与 `id`。最后返回 `req`。

这里没有验证 `params` 是否为对象，也没有验证某个 method 的具体参数。这些职责被推迟到后续的 MCP 类型转换和插件业务校验。

### 实际行为差异：无效请求也被返回为 `-32700`

`jsonrpc_parser.cpp` 还定义了 `make_invalid_request()`，其错误码是 `-32600`。但当前主调用点 [src/main.cpp:92-97](../src/main.cpp#L92) 只要 `parse()` 返回空，就一律调用 `make_parse_error()`。

这意味着当前实际输出是：

| 输入问题 | `parse()` 结果 | 实际发送错误 |
| --- | --- | --- |
| JSON 语法错误 | 空 | `-32700 Parse error` |
| JSON 不是 object | 空 | `-32700 Parse error` |
| `jsonrpc` 不是 `2.0` | 空 | `-32700 Parse error` |
| 缺少或错误的 `method` | 空 | `-32700 Parse error` |

因此前置讲义中“解析错误”和“无效请求错误”的概念边界仍然有用，但**当前实现没有把它们对客户端区分开**。若要符合更细的 JSON-RPC 语义，解析器需要返回更具体的失败原因，或入口需要自行重做校验。

## 5. 路由：`McpHandler` 如何选择业务函数

### 构造与注册

`McpHandler` 在 [src/mcp/mcp_handler.cpp:28-31](../src/mcp/mcp_handler.cpp#L28) 保存 `PluginRegistry*`，接着调用 `register_handlers()`。

`register_handlers()` 在 [37-65 行](../src/mcp/mcp_handler.cpp#L37) 向 `m_routes` 这个 `unordered_map<string, HandlerFn>` 写入九项映射：

```text
initialize
ping
tools/list
tools/call
resources/list
resources/read
prompts/list
prompts/get
notifications/initialized
```

每个 value 都是一个捕获 `this` 的 lambda。以 `tools/call` 为例，lambda 只是把 `params` 转交给成员函数 `handle_tools_call(params)`；它不在 lambda 中写业务逻辑。

### 通用入口：`McpHandler::handle`

[src/mcp/mcp_handler.cpp:78-109](../src/mcp/mcp_handler.cpp#L78) 是第二讲最值得逐行阅读的函数。

#### 第一步：查 `m_routes`

```cpp
auto it = m_routes.find(request.method);
if (it == m_routes.end()) {
    return make_method_not_found(request.id, request.method);
}
```

这里返回的是一个带 `-32601` 的 `JsonRpcResponse`，还没有经过序列化。

#### 第二步：调用找到的处理函数

```cpp
try {
    result = it->second(request.params);
} catch (const std::exception& e) {
    return make_internal_error(request.id, e.what());
}
```

所有业务处理函数的返回类型都统一为 `nlohmann::json`。它们抛出的 `std::exception` 会在这里变成 `-32603 Internal error`。

#### 第三步：正常通知不回包

若处理函数正常结束，再检查 `request.is_notification()`：

```cpp
if (request.is_notification()) {
    return std::nullopt;
}
```

这才是入口 `on_message` 不发送内容的原因。

#### 当前代码的边界：失败的通知仍可能收到错误响应

通知判断位于“查路由”和“执行 handler”之后。由此产生两个实际结果：

```text
已知 method 的通知，正常执行后 -> nullopt，不响应。

未知 method 的通知 -> 在通知判断之前直接返回 -32601 错误响应。
handler 抛异常的通知 -> 在通知判断之前直接返回 -32603 错误响应。
```

这与“所有通知都不响应”的简化说法不完全一致。阅读测试或修改这里时，应明确希望采用哪种语义，再将通知判断移动到合适位置或保留当前行为。

## 6. 逐个看关键 `handle_xxx` 函数

### `handle_initialize`

[125-140 行](../src/mcp/mcp_handler.cpp#L125) 读取客户端初始化参数，但当前实现并未利用这些参数协商版本，而是固定返回协议版本 `2024-11-05` 和固定的服务器信息。

`InitializeResult::to_json()` 会序列化 tools、resources、prompts 三组 capability 字段；当前 flags 均为 `false`。这与注释“只支持 tools”的表述不完全一致，也与同一文件中已有 resources/prompts 路由事实不同。应以实际返回 JSON 与客户端兼容性测试为准。

### `handle_tools_list`

[165-177 行](../src/mcp/mcp_handler.cpp#L165) 做的事很小：

```text
registry 不为空
  -> get_all_tools()
  -> 对每个 ToolDef 调 to_json()
  -> 返回 { "tools": [...] }
```

如果 `m_registry` 是空指针，函数不会抛错，而是返回空数组。这与 `tools/call` 的严格行为不同。

### `handle_tools_call`

[192-206 行](../src/mcp/mcp_handler.cpp#L192) 是工具调用分支：

1. `ToolCallParams::from_json(params)` 读取 `name` 与 `arguments`。
2. `m_registry->call_tool(name, arguments)` 查找并调用插件。
3. 未找到时抛出 `runtime_error`。
4. 找到时调用 `ToolCallResult::to_json()` 返回结果。

`ToolCallParams::from_json()` 位于 [src/mcp/mcp_types.h:108-118](../src/mcp/mcp_types.h#L108)。它使用 `value()` 提供默认值：

```text
name 缺失      -> 空字符串
arguments 缺失 -> 空 object
```

没有独立的参数合法性检查。因此 `name` 缺失的请求最终会被当作 `Unknown tool: ` 抛出，继而被 `handle()` 包装成 `-32603 Internal error`，而不是 `-32602 Invalid params`。`resources/read` 与 `prompts/get` 的参数转换也采取同类默认值策略。

### 资源与提示模板函数

`handle_resources_list/read` 在 [222-255 行](../src/mcp/mcp_handler.cpp#L222)，`handle_prompts_list/get` 在 [271-305 行](../src/mcp/mcp_handler.cpp#L271)。它们与工具分支的骨架一致：

```text
list：从 registry 收集定义，逐个 to_json。
read/get：从 params 取 URI 或名字与参数，交给 registry，找不到时抛异常。
```

差别只在注册表索引键和结果类型：

```text
Tool     -> name -> ToolCallResult
Resource -> uri  -> ResourceReadResult
Prompt   -> name -> PromptGetResult
```

## 7. `PluginRegistry` 是如何完成最后一次分发的

源文件是 [src/plugin/plugin_registry.cpp](../src/plugin/plugin_registry.cpp)。

### `get_all_tools`

[96-103 行](../src/plugin/plugin_registry.cpp#L96) 遍历 `m_tools` 这个哈希表，把每个 entry 中保存的 `ToolDef` 复制到 vector 返回。由于底层是 `unordered_map`，返回顺序不保证稳定；客户端不应依赖工具目录排序。

### `call_tool`

[105-112 行](../src/plugin/plugin_registry.cpp#L105) 只有四步：

```cpp
it = m_tools.find(tool_name)
if (it == end) return nullopt
return it->second.plugin->call_tool(tool_name, arguments)
```

注册表不解释 `arguments`，不捕获插件内部异常，也不把失败改写成成功。它是“工具名 -> 插件实例”的薄分发层。

资源与 Prompt 的 `read_resource()`、`get_prompt()` 采用相同模式，分别查 `m_resources`、`m_prompts`。

一个实现注意点：这些 map 在当前源码中没有自身锁。通常它们在启动时登记、正常服务期间只读、停机时注销；若未来支持运行中装卸插件，则必须补充并发保护或明确生命周期隔离。

## 8. 序列化与发送

`JsonRpcSerializer::serialize()` 在 [src/mcp/jsonrpc_serializer.cpp:16-20](../src/mcp/jsonrpc_serializer.cpp#L16)，实现只有一行：

```cpp
return response.to_json().dump();
```

`to_json()` 由 `JsonRpcResponse` 决定输出 `result` 或 `error`，`dump()` 把 JSON 值压缩为字符串。到这里，协议层的工作结束；下一步的 `Transport::send()` 决定是添加 `Content-Length` 写 stdout，还是放进 SSE 会话队列。

## 9. 用一次真实调用把函数串起来

输入：

```json
{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"file_read","arguments":{"path":"/tmp/a.txt"}}}
```

函数轨迹：

```text
on_message(raw_json)
  -> JsonRpcParser::parse
     返回 JsonRpcRequest{id=7, method="tools/call", params=...}
  -> McpHandler::handle
     m_routes.find("tools/call")
     -> handle_tools_call(params)
        ToolCallParams::from_json
        -> PluginRegistry::call_tool("file_read", arguments)
           -> 某插件实例的 call_tool
           -> ToolCallResult
        -> ToolCallResult::to_json
     -> JsonRpcResponse{id=7, result=...}
  -> JsonRpcSerializer::serialize
  -> Transport::send
```

若工具名不存在，轨迹只在注册表处改变：`call_tool` 返回空，`handle_tools_call` 抛异常，`McpHandler::handle` 捕获后构造 `-32603`。这就是当前代码的实际错误语义。

## 10. 建议你下一步亲自验证的点

1. 用核心单元测试观察正常请求、未知 method 和正常通知。
2. 单独提交缺失 `name` 的 `tools/call`，确认它现在返回的是 `-32603` 而非 `-32602`。
3. 单独提交 `jsonrpc` 版本错误的请求，确认它现在返回的是 `-32700`。
4. 提交未知 method 的通知，确认是否收到 `-32601`，从而验证通知判断的位置。
5. 打断点或加临时日志，观察 `ToolCallResult` 到 `JsonRpcResponse.result` 的形状变化。

本篇新增的源码级术语已登记在 [运行时词汇表](运行时词汇表.md) 的“第 2 讲源码展开”分组。
