# qos_history_depth — `History` 与 `depth` 对消息队列、内存、晚加入语义的影响

本示例聚焦 QoS 的两个最常被调的字段：

- **`history.kind`**：`KeepLast` 表示队列只保留最近 N 条；`KeepAll` 表示全部保留（受 `ResourceLimits` 限制）。
- **`history.depth`**：`KeepLast` 模式下的 N 值。

这两个字段决定了：

1. Publisher 端的发送队列容量。
2. Subscriber 端的接收缓冲容量。
3. 晚加入的订阅者能补送到多少历史消息（叠加 `Durability::TransientLocal` 才能补送）。
4. 进程的内存占用：`depth * sizeof(Msg)`。

读完本示例你能掌握：

- KeepLast 与 KeepAll 的取舍。
- 各 topic 类型的 depth 推荐值。
- URL `?depth=N` 临时覆盖 profile 的 depth。
- ResourceLimits 防 OOM 的兜底配置。

## 背景与适用场景

history 是消息队列容量的核心参数：

- **太小**：晚加入的订阅者补送不全；高吞吐时发布端阻塞（Reliable 模式）或丢消息（BestEffort 模式）。
- **太大**：内存浪费 + 启动后补送延迟变长。

不同 topic 的合理 depth：

| topic 类别 | depth 推荐 | 理由 |
|----------|----------|------|
| 当前状态（速度、位姿） | 1 | 只看最新值 |
| 控制命令 | 5-10 | 缓冲短暂积压 |
| 传感器流（IMU、相机） | 20-50 | 容忍订阅端短暂卡顿 |
| 录制 / 离线分析 | 100+ | 不允许丢 |
| 审计日志 | KeepAll + ResourceLimits | 永不丢但有上限 |

公式：`memory_per_topic ≈ depth × avg_msg_size`。若有 50 个 topic、平均 depth=10、平均消息 200KB，单端内存占用约 100MB。要进一步精细化要看后端实现（DDS 通常把队列分发布端 + 订阅端两份）。

## 核心 API

| API | 签名/字段 | 说明 |
|-----|---------|------|
| `vlink::Qos::history.kind` | `enum { kKeepLast, kKeepAll }` | 队列模式 |
| `vlink::Qos::history.depth` | `uint32_t` | KeepLast 队列长度 |
| `vlink::Qos::resource_limits.max_samples` | `int` | 总样本上限 |
| `vlink::Qos::resource_limits.max_samples_per_instance` | `int` | 每个 instance 的样本上限 |
| `vlink::DdsConf::register_qos` | `static void register_qos(std::string name, Qos)` | 注册到 FastDDS |
| URL `?depth=N` | URL 参数 | 在不修改 profile 的前提下按节点覆盖 depth |

## 代码导读

### 1. 注册三个 KeepLast profile

```cpp
vlink::Qos make_keep_last(const char* name, uint32_t depth) {
  vlink::Qos qos;
  std::strncpy(qos.name, name, sizeof(qos.name) - 1);
  qos.valid = true;
  qos.history.kind = vlink::Qos::History::kKeepLast;
  qos.history.depth = depth;
  return qos;
}

auto depth1 = make_keep_last("depth1", 1);
auto depth10 = make_keep_last("depth10", 10);
auto depth100 = make_keep_last("depth100", 100);

vlink::DdsConf::register_qos("depth1", depth1);
vlink::DdsConf::register_qos("depth10", depth10);
vlink::DdsConf::register_qos("depth100", depth100);
```

三个 profile 只改一个字段：`history.depth`。

### 2. 带 ResourceLimits 的 KeepAll profile

```cpp
vlink::Qos keep_all;
std::strncpy(keep_all.name, "keepall", sizeof(keep_all.name) - 1);
keep_all.valid = true;
keep_all.history.kind = vlink::Qos::History::kKeepAll;
keep_all.resource_limits.max_samples = 10000;
keep_all.resource_limits.max_samples_per_instance = 5000;

vlink::DdsConf::register_qos("keepall", keep_all);
```

KeepAll 没有 depth 字段（理论上无限）；但**必须**配置 ResourceLimits，否则 Subscriber 跟不上 Publisher 时会 OOM。生产代码强制 KeepAll + ResourceLimits 同时设。

### 3. URL `?depth=` 临时覆盖

```cpp
vlink::Publisher<std::string> pub("dds://qos/depth_demo?depth=5");
vlink::Subscriber<std::string> sub("dds://qos/depth_demo?depth=50");

std::atomic<int> count{0};
sub.listen([&count](const std::string& msg) {
  int c = ++count;

  if (c <= 3) {
    VLOG_I("Received:", msg);
  }
});

pub.wait_for_subscribers(2s);

for (int i = 1; i <= 20; ++i) {
  pub.publish("depth-msg-" + std::to_string(i));
}
```

`?depth=5` 让 Publisher 队列最多 5 条；`?depth=50` 让 Subscriber 缓冲 50 条。这两个值**互相独立**，按各自端的资源约束决定。

发布 20 条但 Publisher 只缓 5 条 —— BestEffort 模式下后到的 15 条会覆盖旧的；如果 Subscriber 还没接到就被覆盖，会真正丢。

### 4. 选型指引

```cpp
VLOG_I("depth=1 -> current state | depth=5-10 -> control | depth=20-50 -> sensor",
       " | depth=100+ -> recording | KeepAll -> audit");
VLOG_I("Memory budget: topics * depth * avg_msg_size (depth=1000 * 1MB = 1GB!)");
```

公式 + 量级估算放进日志，帮调试时定位"内存为什么这么高"。

## 运行

```bash
./build/output/bin/example_qos_history_depth
```

预期输出（节选）：

```
Registered depth1, depth10, depth100 profiles.
KeepAll: max_samples=10000
Received:depth-msg-1
Received:depth-msg-2
Received:depth-msg-3
Published 20, received <= 20
depth=1 -> current state | depth=5-10 -> control | depth=20-50 -> sensor | depth=100+ -> recording | KeepAll -> audit
Memory budget: topics * depth * avg_msg_size (depth=1000 * 1MB = 1GB!)
```

DDS 部分需要 FastDDS 组件；不可用时跳过 DDS 段。

## 常见陷阱

1. **KeepAll 不配 ResourceLimits**：理论上无限缓冲；OOM 风险，强制必配。
2. **Publisher depth 与 Subscriber depth 不匹配**：vlink/DDS 不会自动协商；按 DDS 规则取双方各自的值。Reliable 模式下可能因为对端缓冲不够阻塞。
3. **KeepLast(1) + TransientLocal**：很多人想要"晚加入也能拿到最新值"，但 depth=1 + Volatile 是拿不到的；必须 depth=1 + TransientLocal。
4. **大 depth + 大消息**：内存爆炸不常见但很难定位；上线前用 `topics * depth * msg_size` 估一下。
5. **URL `?depth=N` 写到非 DDS 后端**：被忽略；不会报错，但 depth 没生效。

## 设计要点

- depth 是按 instance 计数还是按 sample 计数取决于后端语义；vlink 抽象层会做映射，应用层不必关心细节。
- `?depth=` 与 `?qos=` 可以同时用：先按 profile 应用，再被 URL 中的具体 key 覆盖。
- KeepAll 在 FastDDS 上对应 `KEEP_ALL_HISTORY`，但实际仍受 ResourceLimits 约束；没有真正"无限"。
- 高频写入 + 低频读取 + Reliable 模式 → 发布端阻塞是常见模式；配合 `reliability.block_time` 控制阻塞上限。

## 配图

![QoS KeepLast depth](./images/qos-keep-last-depth.png)

图中展示不同 depth 下队列的覆盖与补送行为。

## 参考

- `../qos_basics/` — Qos 完整结构、子策略全集
- `../qos_profiles/` — 预设 profile（kEvent / kSensor / kLarge 等）
- 顶层 `doc/08-qos.md` — QoS 完整规范
- `vlink/include/vlink/extension/qos.h` — Qos 结构定义
