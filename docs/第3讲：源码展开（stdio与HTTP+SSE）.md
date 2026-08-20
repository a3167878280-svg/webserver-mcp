# 第 3 讲：源码展开（stdio 与 HTTP+SSE）

> 对应前置阅读：[第3讲：两种传输方式与会话边界](第3讲：两种传输方式与会话边界.md)。
>
> 目标：理解传输抽象、stdio 的逐字节分帧、HTTP+SSE 的会话队列，以及当前并发实现实际存在的会话归属风险。

## 1. 先读传输抽象：为什么上层只认识字符串

[src/transport/transport.h](../src/transport/transport.h) 定义 `Transport` 接口：

```cpp
virtual void start() = 0;
virtual void stop() = 0;
virtual void send(const std::string& json_message) = 0;
virtual void set_on_message(MessageCallback callback) = 0;
```

`MessageCallback` 的类型是：

```cpp
std::function<void(const std::string&)>
```

这说明传输层与协议层的契约很窄：

```text
入站：传输层拼出一条完整 JSON-RPC 字符串，再回调上层。
出站：上层给一条完整 JSON-RPC 字符串，传输层负责送达。
```

传输层不知道 `method`、请求 ID、插件或工具；协议层也不需要知道字节是从 stdin 还是 HTTP body 来的。

`main` 中分别构造 `StdioTransport` 或 `HttpSseTransport`，却都用 `set_on_message` 设置同一个 `on_message`，见 [src/main.cpp:109-142](../src/main.cpp#L109)。

## 2. stdio：`StdioTransport` 的完整实现

相关文件：

```text
src/transport/stdio_transport.h
src/transport/stdio_transport.cpp
```

### 对象状态

头文件中的私有字段解释了实现所需的状态：

```cpp
MessageCallback m_callback;
std::thread m_read_thread;
std::atomic<bool> m_running{false};
std::mutex m_write_mutex;
```

它们分别保存上层回调、读线程、运行标记和 stdout 写锁。读与写不共享同一个锁，因为它们作用在不同方向；写锁的任务是防止两个响应的帧在 stdout 中交叉。

### `start()`：启动后立刻等待读线程结束

`StdioTransport::start()` 位于 [src/transport/stdio_transport.cpp:25-34](../src/transport/stdio_transport.cpp#L25)：

```text
设置 m_running = true
  -> 创建读线程，入口是 read_loop()
  -> 立即 join 读线程
  -> 直到 stdin EOF 或 stop 才返回
```

因此 stdio 模式的 `start()` 是阻塞调用。主线程不是继续做 HTTP 服务，而是在等待整个本地管道生命周期结束。

### `read_loop()`：如何从字节流提取一帧

`read_loop()` 在 [src/transport/stdio_transport.cpp:66](../src/transport/stdio_transport.cpp#L66)，每轮循环处理一条帧。

#### 步骤 A：逐字节读取 header

函数连续调用 `fgetc(stdin)`，把字符追加到 `header_buf`。它通过累计两个 `\r\n` 检测 `\r\n\r\n`，这表示 header 结束。

```text
stdin 字节流
  -> header_buf
  -> 遇到 \r\n\r\n
  -> 停止读 header
```

若 `fgetc` 返回 EOF，函数将 `m_running` 设为 false，外层循环随即结束。若 `ferror(stdin)` 为真，会写入日志。

#### 步骤 B：解析 `Content-Length`

函数去掉 header 尾部四个字节后，查找固定前缀 `Content-Length: `，取到下一个 `\r` 之前的文本，并调用 `std::stoi` 转为 `int`。

```text
Content-Length: 85
  -> "85"
  -> int 85
```

找不到该前缀时，本轮记录错误并跳过，继续读下一轮 header。

#### 步骤 C：`read_exact(N)` 精确读取 body

`read_exact()` 在 [147-164 行](../src/transport/stdio_transport.cpp#L147)。它先分配长度为 `N` 的字符串，再循环用 `fread` 补齐，直到读满、遇到 EOF 或错误。

`read_loop()` 随后比较实际长度与 `Content-Length`；只有完全相等才调用：

```cpp
m_callback(std::move(body));
```

`std::move` 表示把 body 的内容所有权交给回调参数，避免不必要复制。此刻 body 就是第 2 讲入口 `on_message` 的 `raw_json`。

### `send()`：组装并原子化写出一帧

`StdioTransport::send()` 在 [src/transport/stdio_transport.cpp:44-56](../src/transport/stdio_transport.cpp#L44)：

```text
JSON 响应字符串
  -> 计算 json_message.size()
  -> 拼接 "Content-Length: N\r\n\r\n"
  -> m_write_mutex 加锁
  -> fwrite 到 stdout
  -> fflush
```

锁覆盖整帧写入，避免并发响应变成下面这种不可解析输出：

```text
Content-LenContent-Length: ...
```

`fflush(stdout)` 使已写入的帧立刻从用户态缓冲区交给下游客户端。

### 当前实现的输入健壮性边界

`read_loop()` 对不存在的 `Content-Length` 有显式处理，但 `std::stoi(len_str)` 没有被 `try/catch` 包围，也没有检查长度是否为负或是否超过合理上限。

因此下列输入需要在验证或改进时特别检查：

```text
Content-Length: abc   -> stoi 抛异常，读线程可能异常终止。
Content-Length: -1    -> int 转为 size_t 后可能成为极大分配请求。
极大 Content-Length  -> 可能造成过量内存分配或长时间等待。
```

这不是协议主干的语义问题，而是面向不可信字节流时的资源保护缺口。

## 3. HTTP+SSE：对象状态与启动过程

相关文件：

```text
src/transport/http_sse_transport.h
src/transport/http_sse_transport.cpp
```

### `Session`：每个会话持有什么

[http_sse_transport.h:84-89](../src/transport/http_sse_transport.h#L84) 的内部 `Session` 结构有四项：

```cpp
std::string id;
std::queue<std::string> pending;
std::mutex mtx;
std::condition_variable cv;
```

`pending` 中保存的不是 HTTP response 对象，而是已经序列化好的 JSON-RPC 响应字符串。`mtx` 保护队列，`cv` 让 SSE 发送侧在队列为空时睡眠、入队时被唤醒。

传输对象还维护：

```text
m_sessions：session ID -> shared_ptr<Session>
m_sessions_mtx：保护会话 map
m_callback：上层 on_message
m_running：服务运行状态
```

### `start(int port)`：HTTP 服务器在独立线程中监听

`HttpSseTransport::start(int)` 在 [src/transport/http_sse_transport.cpp:83](../src/transport/http_sse_transport.cpp#L83)。它先设置端口和运行标记，再创建一个线程；线程内部创建 HTTP server、注册路由并调用阻塞的 `listen()`。

因此与 stdio 的区别是：

```text
stdio start()：当前线程等待 stdin 生命周期。
HTTP start(port)：后台线程监听；main 线程随后继续等待停止信号。
```

## 4. `/sse`：建立出站长连接

`GET /sse` 的处理逻辑在 [123-188 行](../src/transport/http_sse_transport.cpp#L123)。

### 建立或获取会话

1. 请求带 `session_id` 时，使用这个值。
2. 未带时，`new_session_id()` 生成 UUID，并插入 `m_sessions`。
3. 再调用 `get_session(session_id)` 取得 `shared_ptr<Session>`。

`get_session()` 在 [472-480 行](../src/transport/http_sse_transport.cpp#L472)。若 map 中没有该 ID，它也会新建会话。这意味着客户端可用任意未登记的 ID 发起 `/sse` 并得到一个新会话；而 `/message` 对未知 ID 则会拒绝。

### 首先发出 endpoint 事件

SSE content provider 一启动，先写：

```text
event: endpoint
data: /message?session_id=<当前会话>

```

前端据此知道后续 MCP POST 应提交到哪个 URL。

### 消费 `pending` 队列

provider 随后循环：

```text
锁住 session->mtx
  -> cv.wait_for 最多 500ms，直到队列非空或服务停止
  -> 取出队首 JSON 字符串并 pop
  -> 解锁
  -> sink.write("event: message\ndata: <json>\n\n")
  -> 重新加锁，继续取下一条
```

写入时先解锁，意味着慢客户端不会一直占着队列锁，生产者仍可继续入队。若 `sink.write` 返回 false，provider 返回 false，表示连接已断开。

## 5. `/message`：入站 HTTP 请求如何进入共享主干

`POST /message` 在 [198-243 行](../src/transport/http_sse_transport.cpp#L198)。执行顺序很明确：

```text
读取 query parameter 中的 session_id
  -> 没有：HTTP 400
  -> 在 m_sessions 中查不到：HTTP 404
  -> 设置“当前会话”
  -> m_callback(req.body)
  -> 返回 HTTP 202 和 "Accepted"
```

`req.body` 是完整 JSON-RPC 文本，所以 `m_callback(req.body)` 会进入 [src/main.cpp:90](../src/main.cpp#L90) 的同一个 `on_message`。

注意这里 callback 是同步调用：`on_message` 返回前，`POST /message` 不会写出 202。对于普通、同步的插件调用，`Transport::send` 已在 202 返回前把 JSON 响应放入队列；客户端仍需要从 SSE 流接收真正结果。

## 6. `send()`：当前代码怎样把响应放回队列

`HttpSseTransport::send()` 在 [447-457 行](../src/transport/http_sse_transport.cpp#L447)：

```cpp
sid = current_session();
session = get_session(sid);
lock(session->mtx);
session->pending.push(json_message);
unlock;
session->cv.notify_one();
```

这就是生产者侧：协议主干只调用 `send(json)`，HTTP 实现负责把它转换为“当前会话的待发送消息”。

## 7. 关键实现问题：`m_current_session_id` 不是线程局部变量

源码注释把“当前会话”描述为“线程局部 session ID”，但头文件实际定义是：

```cpp
std::mutex m_current_id_mtx;
std::string m_current_session_id;
```

见 [src/transport/http_sse_transport.h:103-112](../src/transport/http_sse_transport.h#L103)。它是整个 `HttpSseTransport` 对象共享的一条字符串，而不是 `thread_local` 变量。

互斥锁保证了读写这条字符串时不会发生数据竞争，但不能让“设置会话”和“后续 send”成为一个不可打断的事务。一个可能的交错是：

```text
线程 A：POST 会话 A，set_current_session(A)
线程 B：POST 会话 B，set_current_session(B)
线程 A：协议主干调用 send()，current_session() 读到 B
结果：A 的响应被放进 B 的队列
```

HTTP server 可以并发处理请求，因此这是实际的响应串会话风险，不只是注释问题。较稳妥的修正方向是：

```text
将 session ID 作为每次请求的显式上下文参数传入发送路径；或
使用真正的 thread_local 上下文，并严格约束同步调用边界；或
让 send 接收目标会话而不是从共享可变状态读取。
```

在修改前，应先为两个并发 `/message` 请求分别断言“响应只出现在自己的 SSE 流中”的集成测试。

## 8. `stop()` 的实际停止范围

`HttpSseTransport::stop()` 在 [407-426 行](../src/transport/http_sse_transport.cpp#L407)：

```text
m_running = false
  -> 遍历全部 session，notify_all 唤醒等待中的 provider
  -> 若 server thread 可 join，则 detach
```

它没有调用 HTTP server 的显式停止接口，因为 server 对象是启动线程 lambda 中的局部变量。因此 `listen()` 本身如何结束不由 `stop()` 直接控制；`detach` 只是放弃 join，不是向服务线程发出终止命令。

这在“进程马上整体退出”的场景可能足够，但若未来需要在同一进程中反复启动、停止、销毁 transport，就需要重新设计 server 对象所有权和停止信号，避免后台线程继续访问已销毁的 transport 状态。

## 9. 传输代码中不应与 MCP 流混淆的聊天路径

同一 HTTP server 还注册 `POST /api/chat`。该路由也使用 SSE，但它把聊天事件直接写进这次 HTTP response 的 content provider，不经过 `m_sessions` 的 MCP `pending` 队列。

```text
/message + /sse：MCP JSON-RPC 双通道，按 session 队列中转。
/api/chat：一条聊天请求对应一条直接 SSE response 流。
```

阅读第 3 讲时，遇到 `handle_chat_request` 可以先跳过；它属于第 5 讲的 LLM 工具循环。

## 10. 建议你下一步亲自验证的点

1. 在 stdio 下连续发送两条帧，确认服务能按 `Content-Length` 分开处理。
2. 发送不含或非法 `Content-Length` 的帧，观察读线程是否安全失败而非退出。
3. HTTP 客户端先 `GET /sse`，再 `POST /message`，确认 202 与 SSE `message` 事件是两份不同响应。
4. 给 `/message` 省略 `session_id`，确认 HTTP 400；给一个未建立的 ID，确认 HTTP 404。
5. 建立两个 SSE 会话，并发提交不同请求，验证每个响应是否仍返回到对应流。这一项应优先补集成测试。
6. 在服务停止期间观察 SSE 连接是否被唤醒、HTTP listen 线程是否仍存活。

本篇新增的源码级术语已登记在 [运行时词汇表](运行时词汇表.md) 的“第 3 讲源码展开”分组。
