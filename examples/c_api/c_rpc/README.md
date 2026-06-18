# c_rpc — 纯 C 实现的请求/响应方法调用

本示例用约 80 行 C 代码完成一对 `Server` / `Client` 的创建、回调注册、连接等待、连续调用、应答返回、句柄销毁全流程。核心约束是 **`vlink_reply()` 必须在 `on_request` 回调内同步调用**，回调返回后再次调用将得到 `VLINK_RET_RUNTIME_ERROR`。

URL 采用 `intra://` 进程内传输，单进程即可演示完整链路。换成 DDS / SHM / SOME/IP 只需替换 URL 字符串。

## 背景与适用场景

VLink 的方法模型对应 C++ 模板 `vlink::Server<Req, Resp>` / `vlink::Client<Req, Resp>`。它表达 1-to-1 的请求/响应语义，比事件模型多一个"应答返回值"环节。

在 C FFI 场景下，没有 `std::function` 闭包，也没有异常用作返回路径，所以 VLink 把 reply 显式化：服务端在 `on_request` 回调里拿到请求字节，**同步**调用 `vlink_reply()` 把响应字节写回；客户端的 `vlink_invoke()` 可选传入响应回调以接收对端答复。这种"必须在回调内回复"的设计避免了在 C 端引入 promise/future 抽象，也方便嵌入式工程把 RPC 应答串到现有的事件循环里。

典型用法：嵌入式控制节点对外暴露"读取参数 / 写入参数 / 触发动作"等业务接口；上层 C 应用或脚本语言通过 FFI 发起请求并读响应。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `vlink_create_server` | `int (const char* url, const vlink_schema_info_t* schema, vlink_server_handle_t* handle, vlink_message_callback_t on_request, void* user_data)` | 创建 Server 并注册请求回调 |
| `vlink_create_client` | `int (const char* url, const vlink_schema_info_t* schema, vlink_client_handle_t* handle)` | 创建 Client 句柄 |
| `vlink_reply` | `int (vlink_server_handle_t* handle, const uint8_t* data, size_t size)` | **仅可在 `on_request` 回调内调用**，写回响应字节 |
| `vlink_invoke` | `int (vlink_client_handle_t handle, const uint8_t* req, size_t req_size, vlink_message_callback_t on_response, void* user_data)` | 发起 RPC；`on_response` 可为 NULL 表示 fire-and-forget |
| `vlink_wait_for_server` | `int (vlink_client_handle_t handle, int timeout_ms)` | 阻塞等待 Server 上线 |
| `vlink_destroy_server` / `vlink_destroy_client` | `int (vlink_*_handle_t* handle)` | 释放原生资源 |

回调签名与 PubSub 共用：

```c
typedef void (*vlink_message_callback_t)(const uint8_t* data, size_t size, void* user_data);
```

注意 `vlink_reply` 的第一个参数是 `vlink_server_handle_t*`（指针），其余 handle 类 API 都是按值传 handle，这是因为 reply 需要修改 handle 内部的同步状态。

API 签名核对入口：`/work/vlink/include/vlink/external/c_api.h` 第 470-560 行。

## 代码导读

源码 `c_rpc.c` 共 108 行；分四段说明。

### 1. on_request 回调里同步 reply

`user_data` 在 `vlink_create_server` 时显式传入了 Server handle 的地址，这样回调能拿到 handle 调用 `vlink_reply`：

```c
static void on_request(const uint8_t* data, const size_t size, void* user_data) {
  vlink_server_handle_t* server = (vlink_server_handle_t*)user_data;
  char resp[128];
  int resp_len = snprintf(resp, sizeof(resp), "REPLY: %.*s", (int)size, (const char*)data);
  int ret = vlink_reply(server, (const uint8_t*)resp, (size_t)resp_len);
}
```

若不在此处 reply，请求会以"空响应"结束，客户端 `vlink_invoke` 仍会收到一次回调，但 `data=NULL, size=0`。

### 2. on_response 回调

客户端响应回调；`data/size` 仅在回调内有效：

```c
static void on_response(const uint8_t* data, const size_t size, void* user_data) {
  (void)user_data;
  if (data != NULL && size > 0) {
    printf("[Client] Response: %.*s\n", (int)size, (const char*)data);
  } else {
    printf("[Client] Empty response\n");
  }
  g_resp_received++;
}
```

### 3. 创建 Server / Client，等待匹配

`schema` 与 PubSub 完全一致；只是把 publisher/subscriber 换成了 server/client：

```c
const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};

vlink_server_handle_t server;
vlink_create_server("intra://c_api/rpc", &schema, &server, on_request, &server);

vlink_client_handle_t client;
vlink_create_client("intra://c_api/rpc", &schema, &client);

vlink_wait_for_server(client, 2000);
```

`vlink_wait_for_server` 在 2 秒内若仍未匹配将返回 `VLINK_RET_UNEXPECTED_ERROR`。

### 4. 连续发起三次 invoke

主循环连发三次请求，每次间隔 100 ms 给 reply 时间送回：

```c
for (int i = 1; i <= 3; ++i) {
  char req[64];
  int req_len = snprintf(req, sizeof(req), "request_%d", i);
  ret = vlink_invoke(client, (const uint8_t*)req, (size_t)req_len, on_response, NULL);
  sleep_ms(100);
}
```

若不需要响应，第四个参数传 NULL 即可（fire-and-forget）。

### 5. 清理

```c
vlink_destroy_client(&client);
vlink_destroy_server(&server);
```

## 运行

```bash
./build/output/bin/example_c_rpc
```

预期输出：

```
wait_for_server: connected
[Server] Request: request_1 (9 bytes)
[Server] Reply sent (ret=0)
invoke("request_1") ret=0
[Client] Response: REPLY: request_1
[Server] Request: request_2 (9 bytes)
[Server] Reply sent (ret=0)
invoke("request_2") ret=0
[Client] Response: REPLY: request_2
[Server] Request: request_3 (9 bytes)
[Server] Reply sent (ret=0)
invoke("request_3") ret=0
[Client] Response: REPLY: request_3
Responses received: 3
```

无任何前置守护进程；`intra://` 跑完即退。

## 常见陷阱

1. **回调外 reply**：`vlink_reply` 出回调即失效，返回 `VLINK_RET_RUNTIME_ERROR`。如需异步业务处理，可在回调里把请求字节拷出排队，但 reply 仍要在回调内完成（或接受"空响应"）。
2. **handle 指针陷阱**：`vlink_reply` 的第一个参数是 `vlink_server_handle_t*`；务必把 server handle 通过 `user_data` 透传到回调，而非每次新建变量。
3. **超时与重试**：`vlink_invoke` 本身是异步派发；C API 暂未暴露 invoke 级超时参数，需配合 `vlink_wait_for_server` 提前确保连接。
4. **response 回调 NULL**：fire-and-forget 模式下传 NULL 是合法的，但服务端仍会跑回调并尝试 reply；reply 结果会被丢弃。
5. **`reserved` 字段**：`vlink_server_handle_t.reserved` 是内部同步原语，不要读写。

## 设计要点

- **Reply 同步化**：保证 RPC 语义在 C FFI 下不需要 future/promise，回调返回即代表 reply 完成。
- **Handle 不变量**：除 `vlink_reply` 外，其它 API 都按值传 handle；这条规则让 C 调用者可以把 handle 自由拷贝。
- **Schema 严谨性**：服务端与客户端必须使用相同 `ser_type` + `schema_type`，否则消息会因 wire 元信息不匹配而被丢弃。
- **零阻塞写回**：服务端回调里 `vlink_reply` 立刻完成；不会在内部排队等待客户端确认。

## 配图

![C API RPC 流程](./images/c-api-rpc-flow.png)

图示了 Client 发起 `vlink_invoke`、Server 在 `on_request` 内调用 `vlink_reply`、响应回到 Client 触发 `on_response` 的完整往返。

## 参考

- `../c_pubsub/` — Event 模型 C 实现，作为本示例的前置阅读
- `../c_field/` — Field 模型 C 实现，对比"最新值"语义
- `../c_security/` — 同套 RPC 接口上叠加加密
- `vlink/include/vlink/external/c_api.h` — 唯一头文件
- 顶层 `doc/18-c-api.md` — C API 参考手册
