# method_async — Method 模型异步调用：延迟回复 + 回调 / future

本示例演示 Method 模型的异步形态。服务端用 `listen_for_reply(cb)` 拿到 `req_id`，把处理工作派出去后再调 `reply(req_id, resp)` 把响应送回；客户端有两种异步发起方式：

- **回调式**：`invoke(req, callback)` 非阻塞，响应到达后回调被调度执行。
- **future 式**：`async_invoke(req)` 返回 `std::future<Resp>`，调用方按需 `.get()` 或 `.wait()`。

读完本示例你能掌握：

- 服务端"延迟回复"语义 —— 在收到请求和真正回 reply 之间可以经历任意线程切换、IO 等待、外部依赖调用。
- 客户端回调式 / future 式调用，以及它们与同步 invoke 的混用规则。
- `detect_connected` 异步通知客户端连接状态变化。

## 背景与适用场景

适用场景：

- 服务端处理本身是异步的：要去查数据库、调外部 HTTP 服务、跑长任务才能给出结果。
- 客户端需要并发发起多个请求并集中等待（fan-out / scatter-gather）。
- 客户端线程不能阻塞（UI、单线程事件循环、协程）。

不适合：

- 简单的"调用即等待"场景（用 `method_sync`）。
- 不关心响应（用 `method_fire_forget` 的 `Server<Req>` / `Client::send`）。

延迟回复在工程上很关键：同步 listen 回调里一返回就视为 reply 完成；如果你需要把 reply 推到另一个线程、攒一批一起回、或等外部异步资源，必须用 `listen_for_reply`。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Server::listen_for_reply` | `bool listen_for_reply(ReqAsyncRespCallback&& cb)` | 回调入参 `(uint64_t req_id, const Req&)`；不在回调里直接给 resp，而是后续调 `reply()` |
| `Server::reply` | `bool reply(uint64_t req_id, const Resp&)` | 用之前拿到的 `req_id` 投递响应 |
| `Client::detect_connected` | `void detect_connected(ConnectCallback&& cb)` | 注册连接状态变化的异步回调 |
| `Client::invoke` (回调) | `bool invoke(const Req&, RespCallback&& cb)` | 非阻塞 invoke，响应到达后调 `cb(const Resp&)` |
| `Client::async_invoke` | `std::future<Resp> async_invoke(const Req&)` | 非阻塞 invoke，返回 future |

## 代码导读

### 1. 服务端 listen_for_reply

```cpp
Server<TranslateRequest, TranslateResponse> server(kUrl);
server.attach(&server_loop);
server.listen_for_reply([&server](uint64_t req_id, const TranslateRequest& req) {
  VLOG_I("[server] word_id=", req.word_id, " lang=", req.target_lang, " req_id=", req_id);

  TranslateResponse resp{};
  resp.word_id = req.word_id;
  resp.target_lang = req.target_lang;
  resp.result_code = (req.word_id > 0 && req.word_id <= 100) ? 0 : 1;

  bool ok = server.reply(req_id, resp);
  VLOG_I("[server] reply req_id=", req_id, " ok=", ok);
});
```

`req_id` 是 vlink 内部分配的唯一标识；`reply(req_id, resp)` 可以**在任意线程、任意时刻**调用，框架按 id 把响应路由回正确的客户端。

工程上 `listen_for_reply` 回调里通常不会立刻 `reply`，而是把 `req_id` + `req` 投递到工作线程池：

```cpp
server.listen_for_reply([&server, &pool](uint64_t req_id, const Req& req) {
  pool.post_task([&server, req_id, req = req]() {
    Resp resp = slow_business_logic(req);   // 在 worker 上跑
    server.reply(req_id, resp);
  });
});
```

### 2. 客户端连接检测

```cpp
Client<TranslateRequest, TranslateResponse> client(kUrl);
client.detect_connected([](bool connected) { VLOG_I("[client] connected=", connected); });
client.wait_for_connected(2000ms);
```

`detect_connected` 异步注册：服务端 discovery 状态变化时回调被调。本示例同时用 `wait_for_connected(2000ms)` 阻塞等到第一次连上，免得后续 invoke 跑得太快。

### 3. 回调式 invoke

```cpp
std::atomic<int> cb_done{0};

for (int i = 1; i <= 3; ++i) {
  TranslateRequest req{i, i % 3};
  client.invoke(req, [i, &cb_done](const TranslateResponse& resp) {
    VLOG_I("[client] callback #", i, " word_id=", resp.word_id, " code=", resp.result_code);
    cb_done.fetch_add(1);
  });
}

for (int wait = 0; wait < 50 && cb_done.load() < 3; ++wait) {
  std::this_thread::sleep_for(20ms);
}
```

3 个 invoke 几乎同时发出去，每个回调独立完成。`cb_done` 用 atomic 等所有 3 个回调跑完才往下走。

### 4. future 式 async_invoke

```cpp
std::vector<std::future<TranslateResponse>> futures;
futures.reserve(5);

for (int i = 10; i <= 14; ++i) {
  futures.push_back(client.async_invoke(TranslateRequest{i, 1}));
}

for (size_t i = 0; i < futures.size(); ++i) {
  TranslateResponse resp = futures[i].get();
  VLOG_I("[client] future #", i, " word_id=", resp.word_id, " code=", resp.result_code);
}
```

5 个 async_invoke 全部发出去，再按顺序 `.get()` 收集结果。`.get()` 会阻塞当前线程直到对应 future 完成。

注意 future 收集是顺序的，但发起是并行的 —— 总耗时是单次 RPC 时延，而不是 5 次相加。

### 5. 同步与异步混用

```cpp
auto sync_result = client.invoke(TranslateRequest{50, 2});
if (sync_result.has_value()) {
  VLOG_I("[client] sync word_id=", sync_result->word_id, " code=", sync_result->result_code);
}

TranslateResponse async_resp = client.async_invoke(TranslateRequest{51, 0}).get();
VLOG_I("[client] async word_id=", async_resp.word_id, " code=", async_resp.result_code);
```

同一个 Client 可以同时支持同步 `invoke()` 和异步 `async_invoke()`，互不干扰。每次调用都用新的 request_id，所以也不会有交叉。

## 运行

```bash
./build/output/bin/example_method_async
```

预期输出（节选）：

```
[client] connected=1
[server] word_id=1 lang=1 req_id=...
[server] reply req_id=... ok=1
[client] callback #1 word_id=1 code=0
[server] word_id=2 lang=2 req_id=...
[client] callback #2 word_id=2 code=0
...
[client] future #0 word_id=10 code=0
[client] future #1 word_id=11 code=0
...
[client] sync word_id=50 code=0
[client] async word_id=51 code=0
```

URL `zenoh://translate/service` 需要 vlink 启用 Zenoh 组件（`vlink::zenoh`）；改成 `intra://` 也能在无 Zenoh 环境跑通。

## 常见陷阱

1. **`req_id` 失效**：`req_id` 只在当前 Server 实例内有效；Server 析构后再 `reply()` 会失败。
2. **`reply()` 调用太晚**：客户端默认 invoke 超时 500ms，如果服务端长时间不 reply，客户端早已超时丢弃。需要时 client 端显式传更长 timeout。
3. **回调式 invoke 的回调在哪里跑**：在 Client attach 的 loop 上（如果没 attach 就在传输层线程）。回调里不要做长任务。
4. **future.get() 不能在 client 的 attach loop 线程里调** —— 那是回调线程，会死锁。本示例 main 线程 attach 时没指定 client.attach，所以安全。
5. **listen_for_reply 与 listen 不能共存**：一个 Server 实例只能选一种回调方式。

## 设计要点

- 异步 reply 的 `req_id` 是 vlink 内部分配的；跨进程时也可以序列化转发（典型场景：proxy 转发请求到后端服务）。
- 回调式 `invoke()` 的 callback 通过 `Function` 移动；不可重入。
- `async_invoke` 内部用 `std::promise`/`std::future` 实现；过早析构 Client 会让未完成的 future 抛 `broken_promise`。
- 异步并发请求数受底层传输的 in-flight 窗口限制（DDS 默认无限制；shm 取决于 capacity）。

## 配图

无专属配图。同步形态的 RPC 时序见 `../method_sync/images/method-sync-rpc-sequence.png`，异步形态的差异是 server 端在回调和 reply 之间可经历任意延迟。

## 参考

- `../method_sync/` — 同步 invoke
- `../method_fire_forget/` — 单向 send，无响应
- `../../base/thread_pool/` — 把异步处理派到 worker 池
- `vlink/include/vlink/client.h`、`server.h` — Client/Server 完整接口
- 顶层 `doc/04-method-model.md` — Method 模型规范
