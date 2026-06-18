# hello_field — VLink 字段模型最小示例

`hello_field` 是 VLink 字段模型（Field Model）的最简版本，使用 `intra://` 进程内传输演示 `Setter<T>` / `Getter<T>` 这一对节点：Setter 持久化最近一次写入，Getter 即使在 Setter 写入之后才创建，依然能够通过 `wait_for_value()` + `get()` 拿到当前状态。读完本示例可以理解 VLink 中的"状态同步"语义和 Event 模型的关键差别。

## 背景与适用场景

字段模型对应自动驾驶/机器人系统中的"状态"概念：当前档位、当前传感器配置、当前节点健康度、当前参数表……。这类数据的特点是：
- 总有一个"最新值"，新加入的读者应当立刻拿到该值，而不是等下一次写入。
- 读者数量可以是 0 到多个，写者通常只有 1 个（也支持多个，但一般会引入仲裁逻辑）。
- 旧值通常没有意义，丢失中间过程的写入不影响最终一致性。

与 Event 模型最关键的区别：晚加入的 `Subscriber` 收不到此前的消息；而晚加入的 `Getter` 通过 `wait_for_value()` 可以拿到 Setter 最近一次写入的值——这是字段模型的"latest value retention"语义，由底层传输的 `sync()` 回调机制实现。

字段模型同时支持回调驱动（`Getter::listen()`）和拉取（`Getter::get()`）两种使用方式，本示例只演示拉取；回调形式见 `../../communication/field_basic/`。

## 核心 API

| API | 签名（来自头文件） | 说明 |
|-----|------|------|
| `vlink::Setter<ValueT>` | `explicit Setter(const std::string& url_str, InitType type = InitType::kWithInit);` | 字段写者，构造即 init |
| `Setter::set(const ValueT&)` | `void set(const ValueT& value);` | 写入新值，同时缓存供晚加入 Getter 使用 |
| `vlink::Getter<ValueT>` | `explicit Getter(const std::string& url_str, InitType type = InitType::kWithInit);` | 字段读者 |
| `Getter::wait_for_value(timeout)` | `bool wait_for_value(std::chrono::milliseconds timeout = Timeout::kDefaultInterval);` | 阻塞直到收到首个值 |
| `Getter::get()` | `[[nodiscard]] std::optional<ValueT> get() const;` | 非阻塞读取最近一次值；尚未收到值时返回 `nullopt` |

## 代码导读

### 1. 字段值类型

`SensorConfig` 描述一个传感器的运行配置：

```cpp
struct SensorConfig {
  int sample_rate_hz;
  float threshold;
};
```

POD 结构体走 `kStandardType` 序列化路径，无需任何额外编排。

### 2. Setter 写入初值

注意本示例没有 `MessageLoop`。Setter 不需要消息循环——它只负责写。`set()` 会立即把值序列化、发送，并缓存起来以便后续晚加入的 Getter 同步。

```cpp
Setter<SensorConfig> setter(kUrl);
setter.set({100, 25.0F});
VLOG_I("[setter] rate=100 threshold=25.0");
```

### 3. 晚加入的 Getter 通过 `wait_for_value` 拿到缓存

`Getter` 构造时与 Setter 同一 URL。`wait_for_value(1000ms)` 阻塞当前线程，最多等 1 秒。`intra://` 上传输层会立即把 Setter 的缓存值同步过来，所以这里几乎立即返回。

```cpp
Getter<SensorConfig> getter(kUrl);
getter.wait_for_value(1000ms);

auto value = getter.get();
if (value.has_value()) {
  VLOG_I("[getter] rate=", value->sample_rate_hz, " threshold=", value->threshold);
} else {
  VLOG_W("[getter] no value");
}
```

`get()` 返回 `std::optional<SensorConfig>`，没有收到任何值时为 `nullopt`。

### 4. Setter 更新值，Getter 再次读取

短暂 `sleep` 用于让传输层完成异步派发；之后 `get()` 应返回新值。

```cpp
setter.set({500, 30.5F});
std::this_thread::sleep_for(50ms);

value = getter.get();
if (value.has_value()) {
  VLOG_I("[getter] rate=", value->sample_rate_hz, " threshold=", value->threshold);
}
```

## 运行

```bash
./build/output/bin/example_hello_field
```

预期输出：

```
[setter] rate=100 threshold=25.0
[getter] rate=100 threshold=25
[getter] rate=500 threshold=30.5
```

无需任何环境变量。

## 常见陷阱

- **`get()` 在第一次 `set()` 之前是 `nullopt`**。即使后续传输层立刻送达，`Getter` 必须等回调跑完才会更新内部缓存——建议读之前先 `wait_for_value()`。
- **`wait_for_value(0)` 会被记 warning 并按无限等待处理**。务必传一个正数毫秒。
- **`set()` 不阻塞**。它是写入并立即返回；如果需要确认对端是否收到，应使用 Event 模型 + 应用层 ACK。
- **本示例没有 `listen()` 回调**。若希望每次 `set()` 都被通知，应改用 `Getter::listen(cb)`，详见 `../../communication/field_basic/`。
- **`Setter` / `Getter` 析构时机**：`Getter` 析构会自动调用 `deinit()` 排空在途回调，但用户对象（lambda 捕获）应保证生命周期覆盖整个回调期间。

## 设计要点

- **状态最近值缓存**：`Setter::set()` 内部使用 `std::optional<ValueT>` + `std::mutex` 维护最近值，传输层有 `sync()` 钩子在新 Getter 接入时自动回放最新值。
- **零消息循环开销**：本示例完全不使用 `MessageLoop`，主线程直接 `set` + `get`，适合简单状态查询场景。
- **跨传输**：与 Event/Method 模型一样，仅改 URL 协议前缀即可换后端（`dds://`、`ddsc://`、`shm://` 等）。`dds://` 上 Field 模型对应 DDS 的 `TransientLocal` 配置。
- **与 Subscriber 共享底层**：`Getter` 实际上是 `Subscriber` 的角色变形（`mark_as_getter()`），享受同样的传输 API 与序列化路径。

## 参考

- `../../communication/field_basic/` — `listen()` 回调形式，演示每次值变更的回调触发。
- `../../communication/field_advanced/` — 变化上报（`set_change_reporting`）、延迟统计、多 Getter 扇出。
- `vlink/include/vlink/setter.h` — `Setter<T>` 完整 API 与 Field/Event 模型对比表。
- `vlink/include/vlink/getter.h` — `Getter<T>` 完整 API。
- 顶层 `doc/05-field-model.md` — 字段模型规范。
