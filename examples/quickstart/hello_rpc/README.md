# 📞 hello_rpc — VLink 方法模型最小示例

VLink 方法模型（Method Model）的最小可运行示例：Server 在某个 URL 上注册请求处理函数，Client 在同一 URL 上发起同步 RPC 调用并取得响应。适用于需要对端明确返回的场景——参数查询、控制命令的确认应答、状态切换的同步握手等。基于 `intra://` 进程内传输。

## 🧩 核心 API

| API | 用途 |
|-----|------|
| `vlink::Server<Req, Resp>` | 服务端节点，构造时传 URL |
| `Server::listen(cb)` | 注册同步处理回调 `void(const Req&, Resp&)`，原地填 `resp` 即自动回发 |
| `Server::attach(loop)` | 将回调派发到指定 `MessageLoop`（须在 `listen` 之前调用） |
| `vlink::Client<Req, Resp>` | 客户端节点，构造时传 URL |
| `Client::wait_for_connected()` | 阻塞直到 Server 可达 |
| `Client::invoke(req)` | 同步 RPC，返回 `std::optional<Resp>`，超时/失败为 `nullopt` |

## ⚙️ 最小示例

```cpp
vlink::Server<CalcRequest, CalcResponse> server("intra://hello/rpc");
server.attach(&loop);  // 须在 listen() 之前
server.listen([](const CalcRequest& req, CalcResponse& resp) {
  resp.result = req.a + req.b;  // 原地填响应，返回后框架自动回发
});

vlink::Client<CalcRequest, CalcResponse> client("intra://hello/rpc");
client.wait_for_connected();
auto resp = client.invoke(CalcRequest{10, 3, '+'});  // 同步阻塞，返回 optional
if (resp.has_value()) { VLOG_I("10 + 3 = ", resp->result); }
```

完整代码见 [`hello_rpc.cc`](hello_rpc.cc)，含详尽注释。运行：

```bash
./build/output/bin/example_hello_rpc   # 预期输出 [server]/[client] 10 + 3 = 13
```

`intra://` 无需环境变量。若输出 `invoke failed`，多半是 Server 尚未 `listen` 就发起了调用——`wait_for_connected()` 仅检查传输连接，不保证回调已注册。

## 🔀 何时用 / 换哪个模型

- 需要对端**明确返回**才能继续 → 用本模型（Method）。多种 `invoke` 形态见 [`method_sync`](../../communication/method_sync/)。
- 只广播不要应答 → Event 模型（[`hello_pubsub`](../hello_pubsub/)）；只同步「最新值」→ Field 模型（[`hello_field`](../hello_field/)）。
- 换后端只改 URL 前缀：`dds://`、`zenoh://`、`someip://` 等，业务代码不变。

## 🔗 参考

- 顶层 [`doc/02-communication.md`](../../../doc/02-communication.md) — 方法模型规范与各调用形态。
- [`include/vlink/client.h`](../../../include/vlink/client.h) / [`include/vlink/server.h`](../../../include/vlink/server.h) — 完整 API。
