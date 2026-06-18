# event_advanced — Event 模型进阶：连接检测、强制发布、扇出、延迟统计

本示例覆盖 Publisher/Subscriber 在生产场景下的几个关键扩展能力：

- **异步感知订阅者**：通过 `detect_subscribers()` 在订阅者加入/离开时被回调通知，替代轮询 `has_subscribers()`。
- **强制发布**：在 0 个订阅者的情况下也把消息送进传输层（典型用法：把 latest 状态预先广播给后加入的订阅者）。
- **多订阅者扇出**：同一 URL 下挂多个 Subscriber，每条消息都会被复制送给每一个，演示 fan-out 行为。
- **延迟与丢包统计**：开启 `set_latency_and_lost_enabled(true)` 后，订阅端可以查询单条消息的 publisher→subscriber 端到端时延（微秒）和累计丢包计数。

这是写真实模块（感知、规划、监控、健康检查）时必须用到的一组 API。`event_basic` 给出了最小骨架；本示例把所有"调一次就够"的高阶 API 集中在一个文件里。

## 背景与适用场景

`Publisher::publish()` 的默认行为是"没有订阅者就什么都不做"，节省序列化和传输开销。但有两种场景需要打破这个默认：

1. **延迟订阅者**：订阅者比 Publisher 晚启动时，前几条消息会被默默丢弃。
2. **状态/配置广播**：希望把一个状态值"放在那"，谁来订阅都能立即拿到最新值。这种场景 vlink 推荐用 Field 模型（`Setter`/`Getter`），但有时仍需用 Event 强制发布。

`detect_subscribers()` 让 Publisher 知道**自己被谁订阅**，从而可以推送一份初始化数据 / 触发链路状态机变更。`wait_for_subscribers(timeout)` 是它的同步阻塞版本，用于程序入口、单元测试。

延迟/丢包统计是排查链路抖动、网络拥塞、DDS Reliability 配置不当时的核心工具，但**必须在 `listen()` 之前**调用 `set_latency_and_lost_enabled(true)` 才生效（一旦 listen 之后再开，已经走过的数据路径不会回填统计字段）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Publisher::detect_subscribers` | `void detect_subscribers(ConnectCallback&& cb)` | 注册订阅者到达/离开的异步回调，回调入参 `bool has` |
| `Publisher::has_subscribers` | `bool has_subscribers() const` | 当前是否有任何订阅者（非阻塞） |
| `Publisher::wait_for_subscribers` | `bool wait_for_subscribers(std::chrono::milliseconds timeout = kDefaultInterval)` | 同步等到至少一个订阅者；超时返回 false |
| `Publisher::publish` | `bool publish(const T& msg, bool force = false)` | `force=true` 时即使无订阅者也下发 |
| `Subscriber::set_latency_and_lost_enabled` | `void set_latency_and_lost_enabled(bool enable)` | 开启端到端延迟+丢包统计；**必须在 `listen()` 之前调用** |
| `Subscriber::get_latency` | `int64_t get_latency() const` | 上一条消息的延迟（微秒） |
| `Subscriber::get_lost` | `SampleLostInfo get_lost() const` | `{ uint64_t total; uint64_t lost; }` 累计计数 |
| `MessageLoop::wait_for_idle` | `bool wait_for_idle(uint32_t timeout_ms)` | 阻塞等到 loop 中的待执行任务全部跑完 |

## 代码导读

### 1. 启动后台 loop

```cpp
MessageLoop loop;
loop.set_name("main_loop");
loop.async_run();
```

`async_run()` 在内部起一个工作线程驱动 loop，主线程继续往下跑。与 `event_basic` 中的 `loop.run()`（主线程阻塞）相对：这里主线程要继续注册各种 API，所以 loop 必须跑在后台。

### 2. 订阅者连接检测 + 强制发布

```cpp
Publisher<SensorReading> pub(kUrl);
pub.attach(&loop);
pub.detect_subscribers([](bool has) { VLOG_I("[pub] subscribers present: ", has); });
VLOG_I("[pub] has_subscribers (before): ", pub.has_subscribers());

SensorReading forced{0, -1.0};
VLOG_I("[pub] normal publish (no subs): ", pub.publish(forced));        // 返回 false
VLOG_I("[pub] forced publish (no subs): ", pub.publish(forced, true));  // 返回 true
```

`detect_subscribers()` 是异步通知：传输层 discovery 检测到订阅者加入/离开会调一次。回调投递到 `attach()` 指定的 loop 上跑。
`publish(msg, true)` 把"强制下发"语义带进 Event 模型 —— 实际写代码里更常见的是用 `mark_as_setter()` 把 Publisher 临时切换为类 Field 的语义。

### 3. 多订阅者扇出

```cpp
Subscriber<SensorReading> sub1(kUrl);
sub1.attach(&loop);
sub1.listen([&count1](const SensorReading& msg) {
  VLOG_I("[sub1] id=", msg.sensor_id, " value=", msg.value);
  count1.fetch_add(1);
});

Subscriber<SensorReading> sub2(kUrl);
sub2.attach(&loop);
sub2.listen([&count2](const SensorReading&) { count2.fetch_add(1); });
```

同一 URL 上挂任意多个 Subscriber，每条消息会被框架复制一份送给每一个。本示例三个订阅者 attach 到**同一个 loop**，所以三个回调实际是顺序串行执行的；如果分别 attach 到不同 loop，三个回调将真正并行。

### 4. 启用延迟与丢包统计

```cpp
Subscriber<SensorReading> sub3(kUrl);
sub3.attach(&loop);
sub3.set_latency_and_lost_enabled(true);  // 必须先开，再 listen
sub3.listen([&sub3, &count3](const SensorReading& msg) {
  count3.fetch_add(1);
  VLOG_I("[sub3] id=", msg.sensor_id, " latency=", sub3.get_latency(), "us");
});
```

`get_latency()` 返回最近一条消息的发布到接收延迟，以微秒为单位，使用 `Header::time_pub` 与本地接收时间做差。底层依赖时钟同步（同机进程同步，跨机依赖 PTP/NTP）。

### 5. 等待订阅就绪后批量发布

```cpp
VLOG_I("[pub] wait_for_subscribers: ", pub.wait_for_subscribers(2000ms));

for (int i = 1; i <= 5; ++i) {
  pub.publish({i, 10.0 + i * 0.1});
  std::this_thread::sleep_for(100ms);
}
```

5 条消息以 100ms 间隔顺序下发，loop 上三个订阅者各收到 5 条。`wait_for_subscribers` 带 2s 超时；DDS 后端的 discovery 通常在 100-500ms 内完成。

### 6. 收尾：查询丢包统计

```cpp
loop.wait_for_idle(2000);

SampleLostInfo lost = sub3.get_lost();
VLOG_I("[sub3] total=", lost.total, " lost=", lost.lost);
```

`wait_for_idle()` 等 loop 上待执行任务全跑完，确保最后几条消息的回调已被处理。然后查询丢包计数 —— 理想状态 `lost.lost == 0`。

## 运行

```bash
./build/output/bin/example_event_advanced
```

预期输出（节选）：

```
[pub] subscribers present: 0
[pub] has_subscribers (before): 0
[pub] normal publish (no subs): 0
[pub] forced publish (no subs): 1
[pub] subscribers present: 1
[pub] wait_for_subscribers: 1
[sub1] id=1 value=10.1
[sub3] id=1 latency=80us
[sub1] id=2 value=10.2
...
[sub3] total=5 lost=0
sub1=5 sub2=5 sub3=5
```

URL `dds://advanced/sensor` 需要 vlink 启用了 FastDDS 组件；改成 `intra://advanced/sensor` 可在无 DDS 环境跑通。

## 常见陷阱

1. **`set_latency_and_lost_enabled(true)` 在 `listen()` 之后调用** —— 不生效，`get_latency()` 一直返回 0。
2. **`detect_subscribers` 回调里阻塞** —— 回调跑在 loop 线程，长时间阻塞会推迟所有 Subscriber/Timer 回调。
3. **`publish(msg, true)` 滥用** —— 高频强发会让传输层不断序列化、入队；建议只在初始化广播状态时用。
4. **多订阅者 attach 同一 loop** —— 看起来"并行"，实际是串行执行。CPU 密集型回调需要分散到不同 loop 或 `ThreadPool`。
5. **跨机时钟未同步** —— `get_latency()` 数值不可信，可能出现负数。

## 设计要点

- `detect_subscribers()` 与 `has_subscribers()` 是互补的：前者是被动通知，后者是主动查询，工程上推荐前者。
- `SampleLostInfo` 的 `total` 是 publisher 端编号差累计（订阅者从 publisher 序列号推断丢失），需要 publisher 端正确填充 `Header::seq`。
- `wait_for_subscribers(timeout)` 在 timeout 之前若所有订阅都已就绪会立刻返回；保持 timeout > discovery 时间（DDS 推荐 ≥ 500ms）。
- Latency 计算依赖发送端时钟与接收端时钟的一致性；跨机部署强烈建议启用 PTP（chrony / linuxptp）。

## 配图

无专属配图。Event 模型的整体角色图见 `../event_basic/images/communication-models-overview.png`。

## 参考

- `../event_basic/` — Event 模型基础（Publisher + Subscriber + Timer）
- `../method_async/` — Method 模型的异步回调与连接检测的对照
- `../../qos/` — 通过 QoS 配置可靠性、历史深度从而影响丢包统计
- `vlink/include/vlink/publisher.h`、`subscriber.h` — Publisher/Subscriber 完整接口
- 顶层 `doc/03-event-model.md` — Event 模型规范
