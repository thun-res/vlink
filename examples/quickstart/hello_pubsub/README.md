# 📡 hello_pubsub — VLink 事件模型最小示例

VLink 事件通信模型（Event Model）的最小可运行示例：基于 `intra://` 进程内传输，演示 `Publisher<T>` 发布、`Subscriber<T>` 回调接收的完整流程。无外部依赖。

## 🧩 核心 API

| 类 / 方法 | 用途 |
|-----------|------|
| `vlink::Publisher<T>` | 发布者，构造传入 URL |
| `Publisher::wait_for_subscribers()` | 阻塞至订阅者就绪，避免首条消息丢失 |
| `Publisher::publish(msg)` | 序列化并发送一条消息 |
| `vlink::Subscriber<T>` | 订阅者，构造传入 URL |
| `Subscriber::attach(loop)` | 回调派发绑定到 `MessageLoop` 线程 |
| `Subscriber::listen(cb)` | 注册接收回调（仅可调用一次） |
| `vlink::MessageLoop` | 回调派发线程，`async_run()` 启动 / `wait_for_idle()` 排空 |

## ⚙️ 最小片段

```cpp
vlink::MessageLoop loop;
loop.async_run();
vlink::Subscriber<SensorReading> sub("intra://hello/pubsub");
sub.attach(&loop);
sub.listen([](const SensorReading& msg) {
  VLOG_I("[sub] seq=", msg.sequence, " temp=", msg.temperature);
});
vlink::Publisher<SensorReading> pub("intra://hello/pubsub");
pub.wait_for_subscribers();              // 握手，避免首条丢失
pub.publish(SensorReading{1, 22.8F});
loop.wait_for_idle(500);                 // 排空回调队列
```

完整代码见 [`hello_pubsub.cc`](hello_pubsub.cc)，已含详尽注释。`SensorReading` 是 POD 结构体，框架按类型自动选择编解码，无需 IDL。运行 `./build/output/bin/example_hello_pubsub`，预期每条 `[pub] seq=N` 后紧跟 `[sub] seq=N`，末尾 `published=5 received=5`；`intra://` 无需任何环境变量。

## 🔀 何时用 / 换哪个模型

- **事件模型**：1-对-多发布/订阅，无最近值缓存，晚加入者只收新消息。适合传感器流、心跳、指令广播。
- 需要「晚加入也能取当前状态」→ **Field 模型**（[`hello_field`](../hello_field/)）；「请求-响应同步调用」→ **Method 模型**（[`hello_rpc`](../hello_rpc/)）。
- 换后端只需改 URL 前缀（`dds://` / `shm://` / `zenoh://` …），业务代码不变。

## ⚠️ 常见陷阱

- `listen()` 仅可调用一次；需切换回调请重建 `Subscriber`。
- 省略 `wait_for_subscribers()` 直接 `publish()`，首条消息可能因对端未就绪被丢弃。
- 回调形参引用出作用域即失效，需保留请按值拷贝。

## 🔗 参考

- [`../../communication/event_advanced/`](../../communication/event_advanced/) — `detect_subscribers`、多订阅者扇出、延迟统计等进阶用法。
- 顶层 [`doc/02-communication.md`](../../../doc/02-communication.md) — 事件模型规范。
