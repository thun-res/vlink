# hello_rpc — VLink 方法模型最小示例

`hello_rpc` 用一份 `intra://` 进程内示例展示 VLink 方法模型（Method Model）的最短可运行形态：Server 在一个 URL 上注册请求处理函数；Client 在同一 URL 上发起同步 RPC 调用，得到反序列化后的响应。读完本示例可以理解 VLink RPC 的"双类型模板（Req/Resp）+ in-place 填充响应 + `invoke()` 同步返回 `std::optional`"这一基础范式。

## 背景与适用场景

Method 模型是 1-对-1 的请求/响应模型，定位类似 gRPC / SOMEIP method / DDS-RPC。它解决的是"我需要拿到对端的明确返回，否则业务无法继续"这类需求：参数查询、控制命令的确认应答、状态切换的同步确认、安全关键路径上的握手等等。

VLink 中方法模型暴露五种调用形态：
1. `invoke(req, resp&, timeout)` —— 同步、用输出引用接收。
2. `invoke(req, timeout) -> std::optional<Resp>` —— 同步、`nullopt` 表示超时或错误。
3. `invoke(req, callback)` —— 异步、回调里处理响应。
4. `async_invoke(req) -> std::future<Resp>` —— 异步、用 `future` 同步等待。
5. `send(req)` —— 单向（fire-and-forget），无响应（要求 `RespT == EmptyType`）。

本示例只演示形态 2（最常用、最简洁）。形态 1/3/4 在 `../../communication/method_sync/` 与 `../../communication/method_async/` 中演示。形态 5 在 `../../communication/method_fire_forget/` 中演示。

## 核心 API

| API | 签名（来自头文件） | 说明 |
|-----|------|------|
| `vlink::Server<ReqT, RespT>` | `explicit Server(const std::string& url_str, InitType type = InitType::kWithInit);` | 服务端节点 |
| `Server::listen(ReqRespCallback&&)` | `bool listen(ReqRespCallback&& callback);`  `using ReqRespCallback = Function<void(const ReqT&, RespT&)>;` | 同步响应：回调内原地填 `resp` |
| `vlink::Client<ReqT, RespT>` | `explicit Client(const std::string& url_str, InitType type = InitType::kWithInit);` | 客户端节点 |
| `Client::wait_for_connected(timeout)` | `bool wait_for_connected(std::chrono::milliseconds timeout = Timeout::kDefaultInterval);` | 阻塞直到 Server 出现 |
| `Client::invoke(req, timeout)` | `[[nodiscard]] std::optional<RespT> invoke(const ReqT& req, std::chrono::milliseconds timeout = Timeout::kDefaultInterval);` | 同步 RPC，返回 `optional<Resp>` |
| `MessageLoop::async_run()` | `bool async_run();` | 后台线程启动循环 |
| `Server::attach(MessageLoop*)` | （继承自 `Node`） | 把服务端回调派发到指定循环 |

## 代码导读

### 1. 请求 / 响应类型

请求和响应都是 POD：

```cpp
struct CalcRequest {
  int a;
  int b;
  char op;
};

struct CalcResponse {
  int result;
};
```

### 2. 启动 Server

服务端 `attach` 到 `loop`，然后 `listen()` 注册同步处理函数。回调签名是 `void(const Req&, Resp&)`——`resp` 是输出引用，框架在回调返回后自动序列化并发回客户端。

```cpp
MessageLoop loop;
loop.async_run();

Server<CalcRequest, CalcResponse> server(kUrl);
server.attach(&loop);
server.listen([](const CalcRequest& req, CalcResponse& resp) {
  switch (req.op) {
    case '+': resp.result = req.a + req.b; break;
    case '-': resp.result = req.a - req.b; break;
    case '*': resp.result = req.a * req.b; break;
    default:  resp.result = 0; break;
  }
  VLOG_I("[server] ", req.a, " ", req.op, " ", req.b, " = ", resp.result);
});
```

### 3. Client 端调用

`wait_for_connected()` 在分布式后端上等价于"等待发现 Server"，这里 `intra://` 几乎立即返回。`invoke(req)` 返回 `std::optional<CalcResponse>`：成功时含值，超时或错误时是 `nullopt`。

```cpp
Client<CalcRequest, CalcResponse> client(kUrl);
client.wait_for_connected();

CalcRequest req{10, 3, '+'};
auto resp = client.invoke(req);

if (resp.has_value()) {
  VLOG_I("[client] 10 + 3 = ", resp->result);
} else {
  VLOG_W("[client] invoke failed");
}
```

### 4. 优雅退出

调用结束后用 `loop.quit()` 触发 server 端循环退出，`wait_for_quit()` 等待后台线程真正退出后再让 `main` 返回。

```cpp
loop.quit();
loop.wait_for_quit();
```

## 运行

```bash
./build/output/bin/example_hello_rpc
```

预期输出：

```
[server] 10 + 3 = 13
[client] 10 + 3 = 13
```

`intra://` 不需要任何环境变量；如果输出 "invoke failed"，多半是 Server 还没注册 listen 就发起调用——下一节有说明。

## 常见陷阱

- **必须在 `client.invoke()` 之前完成 `server.listen()` 注册**。本示例通过先于 client 构造的顺序保证；但 `wait_for_connected()` 只检查传输层连接，不检查 listen 是否注册。多模块编排时建议双向检查。
- **`listen()` 只能调用一次**。重复调用会致命错误退出。
- **`invoke` 的 `[[nodiscard]]`**：返回值必须接收并判定 `has_value()`，否则编译器会给警告。
- **`Server<Req>` （省略 `RespT`）走的是 fire-and-forget 路径**——`invoke()` 在此时会触发 `static_assert`。要单向无响应请用 `Client::send()`，详见 `method_fire_forget` 示例。
- **超时**：默认超时是 `Timeout::kDefaultInterval`（项目级常量，通常为秒级）。传 `0` 会被记 warning 并按"无限等待"处理。

## 设计要点

- **类型安全的 RPC**：`ReqT` / `RespT` 在模板参数中固定，Server 与 Client 的类型不匹配会在编译期或初始化期被检测到。
- **序列化按类型自动选择**：POD 走 `kStandardType`，自定义类型走 `kCustomType`，FlatBuffers/Protobuf 走专属路径。RPC 与 pub/sub 共用同一套序列化机制。
- **线程模型**：Server 回调跑在 `loop` 所在线程；Client 的同步 `invoke()` 阻塞在调用方线程上，因此不需要为 Client 单独 `MessageLoop`。
- **跨传输**：换成 `dds://hello/rpc`、`zenoh://hello/rpc`、`someip://...` 即可跑在真实分布式系统上，业务代码无须改动。

## 参考

- `../../communication/method_sync/` — 三种同步 invoke 形态、`wait_for_connected` 完整使用。
- `../../communication/method_async/` — 回调与 `std::future` 形式的异步调用、`listen_for_reply` + `reply`。
- `../../communication/method_fire_forget/` — 单向无响应的 `Client::send()` + `Server<Req>`。
- `vlink/include/vlink/client.h` — Client 完整 API 与五种调用形态对照表。
- `vlink/include/vlink/server.h` — Server 完整 API。
- 顶层 `doc/04-method-model.md` — 方法模型规范。
