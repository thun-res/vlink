# hello_pubsub — VLink 事件模型 30 行最小示例

`hello_pubsub` 是 VLink 事件通信模型（Event Model）最简版本，所有代码集中在一个 `.cc` 文件中，使用 `intra://` 进程内传输，无任何外部依赖。它展示创建一个 `Publisher<T>`、一个 `Subscriber<T>`、注册回调、发布若干条消息、等待回调全部处理完毕后退出的完整生命周期。读完这个示例可以理解 VLink 的"URL + 类型 + 回调"的核心使用范式。

## 背景与适用场景

事件模型（Event Model）是 VLink 中覆盖面最广的通信范式。语义上属于 1-对-多的发布/订阅：一个 `Publisher<T>` 把消息发出去，所有在同一 URL 上的 `Subscriber<T>` 都会收到一份反序列化后的副本。它没有"最近一次值缓存"，晚加入的订阅者只能收到加入后的新消息——这一点和后面的 Field 模型最大区别。

事件模型在自动驾驶/机器人系统中典型的应用是：传感器数据流（IMU、雷达、相机帧）、状态心跳、控制指令广播、日志聚合。如果消息允许偶尔丢失（best-effort）或希望尽量低延迟，事件模型是首选。如果需要"晚加入也能取到当前状态"，应改用 Field 模型；如果是请求-响应的同步调用，应改用 Method 模型。

本示例使用 `intra://` 是为了演示无需外部依赖的运行。只需把 URL 协议前缀换成 `dds://`、`shm://`、`zenoh://` 等，业务代码完全不变即可切换到对应的传输后端——这就是 VLink 的"URL Scheme"设计核心价值。

## 核心 API

| API | 签名（来自头文件） | 说明 |
|-----|------|------|
| `vlink::MessageLoop` | `MessageLoop();` | 单线程任务循环，订阅回调将派发到此线程上执行 |
| `MessageLoop::async_run()` | `bool async_run();` | 在后台线程上启动循环，调用立即返回 |
| `MessageLoop::wait_for_idle(int ms, bool check = true)` | `virtual bool wait_for_idle(int ms = Timer::kInfinite, bool check = true);` | 等待循环空闲（队列清空），用于让所有回调跑完 |
| `MessageLoop::quit(bool force = false)` | `bool quit(bool force = false);` | 请求退出循环 |
| `MessageLoop::wait_for_quit(int ms, bool check)` | `bool wait_for_quit(int ms = Timer::kInfinite, bool check = true);` | 阻塞直到后台线程真正退出 |
| `vlink::Subscriber<MsgT>` | `explicit Subscriber(const std::string& url_str, InitType type = InitType::kWithInit);` | 创建订阅者，构造即初始化传输 |
| `Subscriber::attach(MessageLoop*)` | （继承自 `Node`） | 把回调派发线程绑定到指定循环 |
| `Subscriber::listen(MsgCallback&&)` | `bool listen(MsgCallback&& callback);` | 注册一次回调，只能调用一次 |
| `vlink::Publisher<MsgT>` | `explicit Publisher(const std::string& url_str, InitType type = InitType::kWithInit);` | 创建发布者 |
| `Publisher::wait_for_subscribers(timeout)` | `bool wait_for_subscribers(std::chrono::milliseconds timeout = Timeout::kDefaultInterval);` | 阻塞直至至少一个订阅者出现 |
| `Publisher::publish(const MsgT&, bool force = false)` | `bool publish(const MsgT& msg, bool force = false);` | 序列化并发送 |

## 代码导读

### 1. 消息类型定义

`SensorReading` 是 POD 结构体（int + float），VLink 的序列化器会自动选择 `kStandardType`（直接按 `sizeof(T)` 拷贝）。无需任何 `operator>>` / `<<` 重载或 IDL 文件。

```cpp
struct SensorReading {
  int sequence;
  float temperature;
};
```

### 2. 启动消息循环

`MessageLoop` 是 VLink 中所有节点回调的派发载体。`async_run()` 会在后台启动一个工作线程，立即返回。订阅回调最终会在这个线程上被调用，从而避免阻塞传输层。

```cpp
MessageLoop loop;
loop.async_run();
```

### 3. 创建并绑定订阅者

`Subscriber<SensorReading>` 在构造时即完成传输初始化（默认 `InitType::kWithInit`）。`attach(&loop)` 把回调派发到 `loop` 所在线程；`listen()` 注册一次性回调，重复调用会触发致命错误。

```cpp
Subscriber<SensorReading> sub(kUrl);
sub.attach(&loop);

std::atomic<int> received{0};
sub.listen([&received](const SensorReading& msg) {
  VLOG_I("[sub] seq=", msg.sequence, " temp=", msg.temperature);
  received.fetch_add(1);
});
```

### 4. 创建发布者并等待订阅者就绪

`Publisher` 构造完后并不保证订阅端已经发现自己——尤其在分布式传输上发现过程是异步的。`wait_for_subscribers()` 阻塞当前线程直到至少一个订阅者在传输层被识别到，这样第一条消息不会丢。`intra://` 传输由于在进程内直接连线，会几乎立即返回。

```cpp
Publisher<SensorReading> pub(kUrl);
pub.wait_for_subscribers();
```

### 5. 循环发布

发布 5 条消息，每条之间睡眠 50ms 避免淹没回调线程。每次 `publish()` 都会触发一次回调，回调中 `received` 自增。

```cpp
for (int i = 1; i <= kMessageCount; ++i) {
  SensorReading msg{i, 22.5F + static_cast<float>(i) * 0.3F};
  pub.publish(msg);
  VLOG_I("[pub] seq=", msg.sequence, " temp=", msg.temperature);
  std::this_thread::sleep_for(50ms);
}
```

### 6. 排空与退出

`wait_for_idle(500)` 等待最多 500ms 让回调队列里的剩余消息全部处理完。然后 `quit()` 设置退出标志，`wait_for_quit()` 阻塞主线程直到后台线程真正退出。这是 VLink 推荐的优雅退出序。

```cpp
loop.wait_for_idle(500);
VLOG_I("published=", kMessageCount, " received=", received.load());
loop.quit();
loop.wait_for_quit();
```

## 运行

```bash
./build/output/bin/example_hello_pubsub
```

预期输出（每行前会有时间戳，省略）：

```
[pub] seq=1 temp=22.8
[sub] seq=1 temp=22.8
[pub] seq=2 temp=23.1
[sub] seq=2 temp=23.1
[pub] seq=3 temp=23.4
[sub] seq=3 temp=23.4
[pub] seq=4 temp=23.7
[sub] seq=4 temp=23.7
[pub] seq=5 temp=24.0
[sub] seq=5 temp=24.0
published=5 received=5
```

`intra://` 不需要任何环境变量。

## 常见陷阱

- **`listen()` 只能调用一次**。如果业务侧需要切换回调，应当销毁 `Subscriber` 重建，而不是再次 `listen()`。
- **构造 `Publisher` 不会等待订阅者**。如果省略 `wait_for_subscribers()` 并立刻 `publish()`，第一条消息可能因为对端未就绪而被丢弃（在分布式传输上尤其明显）。
- **回调形参引用的对象在回调返回后失效**。订阅者内部使用 `thread_local` 缓存反序列化结果，若需要把消息保留下去，请按值拷贝再传出。`Bytes` 和 `IntraData` 是例外（按值/共享指针传递）。
- **`MessageLoop::async_run()` 必须在 `attach()` 前或后都可调用，但回调真正派发需要它在 run 状态**。先 `async_run()` 再 `attach()` 是更安全的次序。
- **退出顺序**。建议先 `wait_for_idle()` 让回调队列清空，再 `quit()`，最后 `wait_for_quit()`；否则可能出现回调还没处理完循环就退出的情况。

## 设计要点

- **回调线程**：本示例所有订阅回调都跑在 `MessageLoop` 的后台线程，与主线程的 `publish` 调用解耦。这是 VLink 推荐的多线程模型：传输层 / 业务层 / 主线程三者解耦。
- **零外部依赖**：`intra://` 全程进程内，无需启动 DDS / Zenoh / FastDDS 守护进程，编译即可跑通。
- **可移植**：只要把 `kUrl` 改成 `dds://hello/pubsub` 或 `shm://hello/pubsub`，代码无需任何其它修改，就能切换到对应后端运行。
- **POD 零编排**：业务结构体只要是 POD（trivial + standard-layout），无需 IDL 也无需手写 `operator>>` / `<<`。

## 参考

- `../../communication/event_basic/` — 用 `Timer` 周期触发发布，演示更接近真实场景的 pub/sub。
- `../../communication/event_advanced/` — `detect_subscribers`、多订阅者扇出、延迟统计、强制发布等高级特性。
- `vlink/include/vlink/publisher.h` — `Publisher<T>` 完整 API。
- `vlink/include/vlink/subscriber.h` — `Subscriber<T>` 完整 API。
- 顶层 `doc/03-event-model.md` — 事件模型规范。
