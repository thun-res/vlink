# 🗂️ hello_field — VLink 字段模型最小示例

用 `Setter<T>` / `Getter<T>` 演示状态同步：Getter 缓存其已接收的最近值，并可用 `get()` 拉取。示例基于 `intra://` 进程内传输；该后端不补送 Getter 创建前的写入，因此先创建 Getter 再写入。

## 🧩 核心 API

| API | 用途 |
|-----|------|
| `vlink::Setter<T>` | 字段写者（构造即可用） |
| `Setter::set(value)` | 写入新值；是否向晚加入者补送取决于后端与 durability/QoS |
| `vlink::Getter<T>` | 字段读者 |
| `Getter::wait_for_value(timeout)` | 阻塞直到收到首个值 |
| `Getter::get()` | 非阻塞拉取最近值，返回 `std::optional<T>` |

## ⚙️ 最小示例

```cpp
vlink::Getter<SensorConfig> getter("intra://hello/field");
vlink::Setter<SensorConfig> setter("intra://hello/field");
setter.set({100, 25.0F});                  // Getter 已就绪，再写入
getter.wait_for_value(1000ms);             // 等待首个已接收值
auto value = getter.get();                 // -> std::optional<SensorConfig>
```

完整代码（含详尽注释）见 [`hello_field.cc`](hello_field.cc)。

## ▶️ 运行

```bash
./build/output/bin/example_hello_field
# [setter] rate=100 threshold=25.0
# [getter] rate=100 threshold=25
# [getter] rate=500 threshold=30.5
```

## 🔀 何时用 / 换哪个模型

- **字段模型**：只关心「最新值」的状态类数据（档位、传感器配置、节点健康度）；若要求晚加入者立即取得当前状态，须选用支持的后端/durability 并以 `wait_for_value()` 确认。
- 需要每次写入都被通知 → 用 `Getter::listen(cb)`，见 [`field_advanced`](../../communication/field_advanced/)。
- 需要「按时序逐条收消息」→ 改用 Event 模型（[`hello_pubsub`](../hello_pubsub/)），晚加入者收不到历史消息。
- 在 `dds://`、`shm://` 等地址兼容的后端间切换时可只改 scheme；专用寻址后端须改完整 URL。

## 🔗 参考

- [`field_advanced`](../../communication/field_advanced/) — 变化上报、延迟统计、多 Getter 扇出。
- 顶层 [`doc/02-communication.md`](../../../doc/02-communication.md) — 字段模型规范。
