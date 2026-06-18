# status_monitoring — `register_status_handler` 监听 DDS 事件 + `get_cpu_usage`

本示例演示 vlink 节点的状态监听能力：通过 `register_status_handler` 注册回调，框架在连接建立 / QoS 违约 / 样本丢失等关键事件时调用。同时演示 `get_cpu_usage()` 拿到当前节点的 CPU 占用（前提是开启 vlink profiler）。

读完本示例你能掌握：

- 6 种 DDS 标准事件类型的语义。
- 怎么注册节点级状态回调。
- profiler 的开启方式（`VLINK_PROFILER_ENABLE=1`）。
- 监控埋点的工程模式。

## 背景与适用场景

适用：

- 生产环境的健康监控、SLA 告警。
- 与 metric 系统（Prometheus / 自家系统）集成。
- 调试 QoS 设置：通过事件类型确认是不是配错 reliability / deadline。

不适合：

- 内部数据路径调试（用 ProxyServer 实时观察）。
- 高频每条消息的延迟统计（用 `set_latency_and_lost_enabled` + `get_latency`）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Node::register_status_handler` | `void register_status_handler(Function<void(Status::BasePtr)>&&)` | 注册状态回调 |
| `Status::BasePtr` | `std::shared_ptr<Status::Base>` | 状态基类指针 |
| `Status::Base::get_type` | `StatusType get_type() const` | 事件类型 |
| `Status::Base::get_detail` | `StatusDetail` | 子类型详细信息（`StatusDetail` 联合体） |
| `Node::get_cpu_usage` | `double get_cpu_usage() const` | 0..100；profiler 未开返回 -1 |

## DDS 标准事件类型

| StatusType | 含义 |
|-----------|------|
| `kPublicationMatched` | Publisher 端：有新的 Subscriber 加入 |
| `kSubscriptionMatched` | Subscriber 端：发现匹配的 Publisher |
| `kOfferedDeadlineMissed` | Publisher：deadline 期内没有发送 |
| `kRequestedDeadlineMissed` | Subscriber：deadline 期内没有收到 |
| `kLivelinessLost` | Publisher 心跳停了 |
| `kLivelinessChanged` | Subscriber：发布者活跃状态变化 |
| `kSampleRejected` | Subscriber 拒收（队列满 + Reliable QoS） |
| `kSampleLost` | 样本传输中丢失 |

不同传输后端实际支持的事件子集不同（DDS-family 最全；shm / fdbus / mqtt 等只支持 matched 类）。

## 代码导读

### 1. 注册 status handler

```cpp
vlink::Publisher<std::string> pub("dds://topic");
pub.register_status_handler([](vlink::Status::BasePtr s) {
  VLOG_I("status type=", static_cast<int>(s->get_type()));
});

vlink::Subscriber<std::string> sub("dds://topic");
sub.register_status_handler([](vlink::Status::BasePtr s) {
  VLOG_I("subscriber status type=", static_cast<int>(s->get_type()));
});

pub.wait_for_subscribers();
// 此时双方都收到 kPublicationMatched / kSubscriptionMatched 事件
```

### 2. 读取详细信息

```cpp
pub.register_status_handler([](vlink::Status::BasePtr s) {
  auto type = s->get_type();
  auto detail = s->get_detail();
  switch (type) {
    case vlink::StatusType::kPublicationMatched:
      VLOG_I("matched: current_count=", detail.matched.current_count);
      break;
    case vlink::StatusType::kSampleLost:
      VLOG_W("lost: total_count=", detail.sample_lost.total_count);
      break;
    default:
      break;
  }
});
```

### 3. CPU usage

```cpp
double pct = pub.get_cpu_usage();
if (pct < 0) {
  VLOG_W("profiler disabled");
} else {
  VLOG_I("CPU%=", pct);
}
```

开启 profiler：

```bash
export VLINK_PROFILER_ENABLE=1
./build/output/bin/example_status_monitoring
```

未开启时 `get_cpu_usage` 返回 -1。

## 运行

```bash
./build/output/bin/example_status_monitoring

# 启用 CPU profiler
export VLINK_PROFILER_ENABLE=1
./build/output/bin/example_status_monitoring
```

预期输出（节选）：

```
[pub] status type=1 (kPublicationMatched)
[sub] status type=2 (kSubscriptionMatched)
CPU%=1.2          # 或 "profiler disabled"
```

## 常见陷阱

1. **回调在节点的事件线程跑**：可能是 transport 内部线程；阻塞会卡心跳。考虑 attach loop 控制回调线程。
2. **不同后端事件支持不同**：shm 不支持 deadline / liveliness 等 DDS 特有事件；监听了但不会触发。
3. **回调里抛异常**：vlink 不会处理；try/catch。
4. **VLINK_PROFILER_ENABLE 仅在 main 启动前生效**：运行时设无效。
5. **get_cpu_usage 是进程级**：所有 vlink 线程的 CPU 之和；不是节点单独的。

## 设计要点

- StatusType 与 DDS DataReader/DataWriter Status 对齐；vlink 抽象层映射到各后端。
- profiler 启用后内部用 thread-local 累计 CPU 时间。
- handler 注册不带 unregister API；按 node 析构自动解绑。

## 配图

![Status detail events](./images/status-detail-events.png)

图中展示 8 种 DDS 标准事件 + StatusDetail 字段。

## 参考

- `../message_loop_binding/` — 控制 status 回调线程
- `../../communication/event_advanced/` — set_latency_and_lost_enabled 细粒度延迟统计
- `vlink/include/vlink/extension/status.h` / `status_detail.h` — 状态结构
- `vlink/include/vlink/node.h` — Node 接口
- 顶层 `doc/21-environment-vars.md` — `VLINK_PROFILER_ENABLE`
