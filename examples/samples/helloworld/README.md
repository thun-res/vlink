# 👋 helloworld —— Method + Event 综合入门示例

同一份代码可由环境变量选择 dds / ddsc / shm / someip / fdbus / qnx 后端，无需重新编译；topic 型后端复用 path，SOME/IP 等由示例提供符合协议的完整 URL。示例同时演示 Method（RPC）与 Event（pub/sub）两种通信模型，消息采用 Protobuf 序列化。为 samples 类目首推示例。

![samples 关系图](../images/samples-relationship.png)

## 🧩 核心 API

| API | 用途 |
|-----|------|
| `Server<Req, Resp>` / `listen(cb)` | RPC 服务端，回调中填充 `Resp` 应答请求 |
| `Client<Req, Resp>` / `invoke(req, resp, timeout)` | RPC 客户端，同步发起调用 |
| `Client::wait_for_connected(timeout)` | 首次 invoke 前等待服务端就绪，避免与发现过程竞态 |
| `Publisher<Msg>` / `publish(msg)` | 事件发布 |
| `Subscriber<Msg>` / `listen(cb)` | 事件订阅 |
| `Timer` + `MessageLoop` | 周期触发 `publish`，loop 驱动主循环 |
| `Utils::check_singleton(name)` | 同名进程互斥，防止重复启动 |
| `Utils::register_terminate_signal(cb)` | SIGINT / SIGTERM 优雅退出 |

## ⚡ 最小片段

服务端（同一进程承载 Server + Publisher + Timer）：

```cpp
vlink::Server<Helloworld::Request, Helloworld::Response> server(Common::get_method_url());
server.listen([](const Helloworld::Request& req, Helloworld::Response& resp) {
  resp.set_sum(req.left() + req.right());
});

vlink::Publisher<Helloworld::Message> pub(Common::get_event_url());
vlink::Timer timer;
timer.attach(&message_loop);
timer.set_interval(100);
timer.set_loop_count(vlink::Timer::kInfinite);
timer.start([&pub] { pub.publish(msg); });
message_loop.run();
```

客户端发起一次 RPC：

```cpp
vlink::Client<Helloworld::Request, Helloworld::Response> client(Common::get_method_url());
client.wait_for_connected(1s);
client.invoke(req, resp, 3s);  // resp.sum() 即结果
```

## 🚀 运行

```bash
./build/output/bin/sample_helloworld_server          # 终端 1：服务端
./build/output/bin/sample_helloworld_client sub      # 终端 2：订阅事件
./build/output/bin/sample_helloworld_client set 10 20  # 终端 3：RPC，输出 sum=30
```

## 🔀 切换后端

设置环境变量即切换传输，默认 `dds`：

| 后端 | 环境变量 | 前置条件 |
|------|----------|----------|
| `ddsc` | `METHOD_TRANSPORT=ddsc EVENT_TRANSPORT=ddsc` | — |
| `shm` | `METHOD_TRANSPORT=shm EVENT_TRANSPORT=shm` | `iox-roudi &` |
| `someip` | `METHOD_TRANSPORT=someip EVENT_TRANSPORT=someip` | vsomeip routing manager |
| `fdbus` | `METHOD_TRANSPORT=fdbus EVENT_TRANSPORT=fdbus` | `fdb_name_server &` |
| `qnx` | `METHOD_TRANSPORT=qnx EVENT_TRANSPORT=qnx` | QNX target |

或用 `METHOD_URL` / `EVENT_URL` 直接传入完整 URL，覆盖 `*_TRANSPORT`。

## 🧭 模型选择

| 场景 | 模型 |
|------|------|
| 一问一答、需要返回值 | Method（`Client` / `Server`） |
| 一对多广播、无需应答 | Event（`Publisher` / `Subscriber`） |
| 仅同步最新状态值 | Field（`Setter` / `Getter`），见 `../shm_raw/` |

## 🔗 参考

- `../ping_pong/` —— Bytes 原始数据 + 延迟测量
- `../shm_raw/` —— 单进程内全部六原语 + Security
- 顶层 [`examples/README.md`](../../README.md) —— samples 索引与阅读顺序
