# 📡 event_advanced —— Event 模型进阶

在最小 pub/sub 之上演示 Publisher / Subscriber 在感知、监控、健康检查等真实模块中常用的进阶能力：连接检测、强制发布、多订阅者扇出、端到端延迟与丢包统计。

![Event 模型数据流](../../../doc/images/event-dataflow.png)

## ⚡ 进阶能力一览

| 能力 | 机制 |
|------|------|
| 连接检测 | `detect_subscribers()` 在订阅者加入 / 离开时异步回调，替代轮询 `has_subscribers()` |
| 强制发布 | `publish(msg, true)` 在 0 订阅者时仍把消息送入传输层（诊断信标、录制预热） |
| 多订阅者扇出 | 同一 URL 挂多个 Subscriber，每条消息复制送达每一个 |
| 延迟 / 丢包统计 | `set_latency_and_lost_enabled(true)` 后可查询单条端到端时延与累计丢包 |

## 🧩 核心 API

| API | 语义 |
|-----|------|
| `pub.detect_subscribers(cb)` | 注册订阅者到达 / 离开的异步回调，入参 `bool has` |
| `pub.has_subscribers()` | 非阻塞查询当前是否有订阅者 |
| `pub.wait_for_subscribers(timeout)` | 同步等到至少一个订阅者，超时返回 `false` |
| `pub.publish(msg, force)` | `force=true` 时无订阅者也下发 |
| `sub.set_latency_and_lost_enabled(true)` | 开启延迟 + 丢包统计，须在 `listen()` 之前调用 |
| `sub.get_latency()` | 上一条消息的端到端延迟（纳秒） |
| `sub.get_lost()` | 返回 `SampleLostInfo{ total, lost }` 累计计数 |

## 🚀 最小示例

```cpp
vlink::MessageLoop loop;
loop.async_run();

vlink::Publisher<SensorReading> pub("dds://advanced/sensor");
pub.attach(&loop);
pub.detect_subscribers([](bool has) { VLOG_I("subscribers present: ", has); });
pub.publish({0, -1.0}, true);

vlink::Subscriber<SensorReading> sub("dds://advanced/sensor");
sub.attach(&loop);
sub.set_latency_and_lost_enabled(true);
sub.listen([&sub](const SensorReading& msg) {
  VLOG_I("id=", msg.sensor_id, " latency=", sub.get_latency(), "ns");
});

pub.wait_for_subscribers(2000ms);
pub.publish({1, 10.1});

loop.wait_for_idle(2000);
vlink::SampleLostInfo lost = sub.get_lost();
VLOG_I("total=", lost.total, " lost=", lost.lost);
```

同一 URL 上可挂任意多个 Subscriber，每条消息复制送达每一个；attach 到同一 loop 时回调顺序串行，分别 attach 到不同 loop 才并行执行。`dds://` 需启用 FastDDS 组件，改用 `intra://advanced/sensor` 可在无 DDS 环境下运行。

## 🔀 模型选择

| 需求 | 选用模型 |
|------|----------|
| 任意订阅者立即拿到最新状态 / 配置广播 | Field（`Setter` / `Getter`），比强制发布更直接 |
| 请求 / 响应、需要拿到返回值 | Method（`Client` / `Server`） |
| 最小 pub/sub | `../../quickstart/hello_pubsub/` |

## 🔗 参考

- `../../quickstart/hello_pubsub/` —— Event 模型基础（Publisher + Subscriber）。
- `../field_advanced/` —— Field 模型进阶（状态同步、变化上报）。
- `../../qos/qos_basics/` —— QoS 配置可靠性与历史深度，影响丢包统计。
- 顶层 `doc/02-communication.md` —— Event 模型规范。
