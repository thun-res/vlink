# event_basic — Event 模型基础：Publisher / Subscriber + Timer 周期发布

本示例演示 VLink 三种通信模型中最常用的一种 —— **Event（发布订阅）**。Publisher 把消息推送到 URL，所有挂在同一 URL 上的 Subscriber 都会以回调形式收到。示例用一个 `Timer` 周期性地驱动发布，并通过 `MessageLoop` 把发布和接收都放在同一线程里完成，这种"单线程 + 事件驱动"是 vlink 应用层最典型的运行模式。

读完本示例你能掌握：

- `Publisher<T>` / `Subscriber<T>` 的最小用法，以及它们与 `MessageLoop`、`Timer` 的关系。
- `wait_for_subscribers()` 的语义和它解决的"消息丢前几条"问题。
- `register_terminate_signal()` + `loop.quit()` 的优雅退出模式，这套模式在所有 base/communication 示例里反复出现。

## 背景与适用场景

Event 模型适合所有"一对多、单向、按时间顺序产生"的数据流：

- 传感器读数（IMU、雷达、激光、相机帧）。
- 感知/预测/规划等模块输出的中间结果。
- 业务事件、状态变化日志、运行指标。

不适合：

- 请求响应（用 Method 模型）。
- "只关心最新值"的状态共享，订阅者频繁加入退出（用 Field 模型，自动维护 latest）。
- 必须保证每条消息被持久化的场景（应叠加 `recording/` 或外部消息队列）。

在 VLink 内部，`Publisher<T>` / `Subscriber<T>` 是模板类，通过模板参数 `T` 决定消息类型；序列化策略由 `Serializer::get_type_of<T>()` 在编译期推导。本示例 `SensorData` 是一个 POD 结构体，会被推导为 `kStandardType`，按 `memcpy` 编解码。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `vlink::Publisher<T>` | `explicit Publisher(const std::string& url, InitType type = kWithInit)` | 构造发布者，绑定 URL；默认构造即完成 `init()` |
| `vlink::Publisher<T>::publish` | `bool publish(const T& msg, bool force = false)` | 把消息推送到 URL；`force=true` 时即使没有订阅者也会推 |
| `vlink::Publisher<T>::wait_for_subscribers` | `bool wait_for_subscribers(std::chrono::milliseconds timeout)` | 阻塞等待至少一个订阅者就绪 |
| `vlink::Subscriber<T>` | `explicit Subscriber(const std::string& url, InitType type = kWithInit)` | 构造订阅者 |
| `vlink::Subscriber<T>::attach` | `void attach(MessageLoop* loop)` | 把回调投递到指定 loop 上执行 |
| `vlink::Subscriber<T>::listen` | `bool listen(MsgCallback&& cb)` | 注册收消息回调 |
| `vlink::MessageLoop` | 默认构造 | 单线程任务/事件分发器 |
| `vlink::MessageLoop::async_run` | `void async_run()` | 在后台线程启动 loop |
| `vlink::MessageLoop::run` | `void run()` | 当前线程阻塞跑 loop（本示例使用） |
| `vlink::MessageLoop::quit` | `void quit()` | 请求退出 |
| `vlink::Timer` | `Timer(MessageLoop* loop, uint32_t interval_ms, int loop_count, Function<void()>&& cb)` | 周期定时器；`kInfinite` 表示无限循环 |
| `vlink::Timer::start` | `void start()` | 启动定时器 |
| `vlink::Utils::register_terminate_signal` | `void register_terminate_signal(Function<void(int)>&& cb)` | 注册 SIGINT/SIGTERM 处理器 |

## 代码导读

### 1. 启动 MessageLoop 并注册信号处理

`MessageLoop` 是 vlink 应用层的"单线程事件循环"，所有 Subscriber 回调、Timer 回调都会被序列化到这个 loop 的执行线程上。`run()` 会阻塞当前线程直到 `quit()` 被调用。

```cpp
MessageLoop loop;
loop.set_name("main_loop");

Utils::register_terminate_signal([&loop](int) { loop.quit(); });
```

`set_name()` 把线程名写进进程信息，便于在 top / htop / 调度器 dump 里识别。`register_terminate_signal` 在收到 SIGINT（Ctrl-C）或 SIGTERM 时把 `quit()` 投递到 loop，主流程从而干净退出。

### 2. 创建 Subscriber 并 attach 到 loop

```cpp
Subscriber<SensorData> sub(kUrl);
sub.attach(&loop);

std::atomic<int> received{0};
sub.listen([&received](const SensorData& data) {
  VLOG_I("[sub] id=", data.id, " value=", data.value, " ts=", data.timestamp);
  received.fetch_add(1);
});
```

`attach(&loop)` 让框架把后续 `listen()` 的回调投递到 `loop` 线程；如果不 attach，回调会跑在传输层内部线程（不可控、不安全共享数据）。`listen()` 注册一次回调，可以重复调用切换回调，但通常一个 Subscriber 只 listen 一次。

### 3. 创建 Publisher 并等待订阅者

```cpp
Publisher<SensorData> pub(kUrl);
pub.wait_for_subscribers();
```

不调 `wait_for_subscribers()` 时，发布的前几条消息可能在订阅者 discover 完成前被丢弃（典型 1-50 ms 窗口，与传输和 QoS 配置相关）。在示例和单元测试里几乎总是要先等订阅者，再发；生产代码会换成 `detect_subscribers` 回调式触发。

### 4. 用 Timer 周期发布

```cpp
Timer timer(&loop, 500, Timer::kInfinite, [&pub, &published, &loop]() {
  int seq = published.fetch_add(1) + 1;

  if (seq > kMaxPublish) {
    loop.quit();
    return;
  }

  SensorData data{};
  data.id = seq;
  data.value = 20.0F + static_cast<float>(seq) * 0.5F;
  data.timestamp = ...;

  pub.publish(data);
});
timer.start();
```

Timer 的回调会被 loop 调度，与 Subscriber 回调天然串行 —— 这是单线程 loop 模型最大的好处：业务代码不用加锁。发够 10 条后调 `loop.quit()` 让主流程退出。

### 5. 阻塞跑 loop

```cpp
loop.run();
VLOG_I("published=", published.load(), " received=", received.load());
```

`run()` 阻塞到 `quit()` 触发；之后打印发布/接收计数。

## 运行

```bash
./build/output/bin/example_event_basic
```

预期输出（节选）：

```
[pub] #1 value=20.5
[sub] id=1 value=20.5 ts=...
[pub] #2 value=21
[sub] id=2 value=21 ts=...
...
[pub] #10 value=25
[sub] id=10 value=25 ts=...
published=10 received=10
```

按 Ctrl+C 也能在任意时刻干净退出。

URL 默认是 `dds://sensor/temperature`，需要 vlink 编译时启用 FastDDS 组件（`vlink::dds`）。如果只装了核心库，把 URL 改成 `intra://sensor/temperature` 即可在进程内跑。

## 常见陷阱

1. **忘记 `attach(&loop)`**：Subscriber 回调会在传输层的内部线程上跑；与你预期的"单线程串行"不一致，且与该线程共享的可变状态需要加锁。
2. **不 `wait_for_subscribers()`**：开发期跑示例时表现为"前几条丢了"，看起来像 bug 实际上是 discovery 没完成。生产代码用 `detect_subscribers` 回调监听到达，而不是阻塞等。
3. **Timer 回调里访问跨线程数据**：本示例所有 atomic 访问都安全；如果你换成普通 `int`，多线程读写会 UB。
4. **`force=true` 滥用**：`publish(msg, true)` 强发不检查订阅者，常用于状态广播（如 Field 的初始值）。频繁强发会浪费序列化开销。
5. **Publisher 比 Subscriber 短命**：Publisher 析构后挂在它上面的 send 操作会被丢弃；保持 Publisher 至少和最长一次发布同生命周期。

## 设计要点

- `MessageLoop` 不是线程池：它只跑在一个线程上。如果有 CPU 密集工作要做，用 `ThreadPool` 或 `MultiLoop`（参考 `base/thread_pool/`、`base/multi_loop/`）。
- 模板参数 `T` 决定序列化策略，编译期推导，零运行期开销。POD 结构会走 memcpy；要走自定义序列化必须实现 `operator>>` / `operator<<`（见 `serialization/custom_type/`）。
- `dds://` 默认走 FastDDS，但 vlink 也支持 CycloneDDS（`ddsc://`）、RTI（`ddsr://`）、TravoDDS（`ddst://`）等；同一份代码通过改 URL 前缀就能切换后端。
- `wait_for_subscribers()` 没有超时参数时使用默认超时（`Timeout::kDefaultInterval`，通常 500ms 左右）。

## 配图

![Communication models](./images/communication-models-overview.png)

图中给出 VLink 三种通信模型（Event / Method / Field）的角色对应：Event 是一对多广播、Method 是双向请求响应、Field 是带最新值缓存的状态共享。本示例对应左侧的 Event 列。

## 参考

- `../event_advanced/` — 多订阅者、延迟统计、`detect_subscribers`、强制发布、`std::optional` 返回
- `../../quickstart/hello_pubsub/` — 最短的 pub/sub 示例（30 行）
- `vlink/include/vlink/publisher.h`、`subscriber.h` — Publisher/Subscriber 接口
- `vlink/include/vlink/base/timer.h` — Timer 接口
- 顶层 `doc/03-event-model.md` — Event 模型规范
