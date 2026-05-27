# TinyWebServer-MCP

基于 [TinyWebServer](https://github.com/qinguoyi/TinyWebServer) 改造的 **MCP (Model Context Protocol) 服务器**，为 AI 客户端提供标准化的工具调用能力。

## 项目路线图

| 版本 | 内容 | 状态 |
|------|------|------|
| **V1** | CMake 构建、JSON-RPC 2.0 协议、stdio 传输、基础 MCP 方法 | ✅ 已完成 |
| V2 | 插件系统 (dlopen + .so 动态加载) | 计划中 |
| V3 | HTTP+SSE 远程传输 (cpp-httplib) | 计划中 |
| V4 | LLM 代理 + 聊天前端 | 计划中 |
| V5 | 更多插件 (天气/代码审查/文件管理) | 计划中 |

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

## 原始项目

TinyWebServer — Linux 下 C++ 轻量级 Web 服务器，基于 epoll + 线程池实现 Reactor/Proactor 并发模型。
