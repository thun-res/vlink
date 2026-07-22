# 🗂️ field_advanced —— Field 模型进阶

在 `Setter` / `Getter` 基础之上演示三项常用进阶能力：变化上报（去重连续相同的 `set`）、晚加入同步（后创建的 Getter 仍能拿到 latest）、多 Getter 扇出与延迟 / 丢包统计。适用于配置中心、参数下发、跨模块共享状态。

![Field 模型数据流](../../../doc/images/field-dataflow.png)

## 🧩 核心 API

| API | 语义 |
|-----|------|
| `Getter::set_change_reporting(bool)` | 开启后抑制序列化字节与上一值相同的回调，须在 `listen` 前调用 |
| `Getter::get_change_reporting()` | 查询当前是否开启变化上报 |
| `Getter::wait_for_value(timeout)` | 阻塞等到首次拿到值，超时返回 `false` |
| `Getter::set_latency_and_lost_enabled(bool)` | 开启 / 关闭端到端延迟与丢包统计，可在 `listen` 前后切换 |
| `Getter::get_latency()` | 最近一次回调的端到端延迟（纳秒） |
| `Getter::get_lost()` | 返回 `SampleLostInfo{ total, lost }` 累计统计 |

## 🚀 最小示例

变化上报：连续写入序列化字节相同的值时不重复触发回调。使用 Standard 序列化的结构体应确保 padding / reserved 字节确定。

```cpp
vlink::Setter<BrightnessConfig> setter("ddsc://display/brightness");
setter.set({50, false});

vlink::Getter<BrightnessConfig> getter("ddsc://display/brightness");
getter.attach(&loop);
getter.set_change_reporting(true);
getter.listen([](const BrightnessConfig& cfg) {
  VLOG_I("changed level=", cfg.level);
});
getter.wait_for_value(1000ms);

setter.set({50, false});
setter.set({75, false});
```

晚加入同步：后创建的 Getter 仍能阻塞拿到 latest。

```cpp
vlink::Getter<BrightnessConfig> late("ddsc://display/brightness");
late.attach(&loop);
if (late.wait_for_value(2000ms)) {
  auto val = late.get();
}
```

多 Getter 扇出与统计：同一 URL 挂多个 Getter，每次 `set` 各收到一份。

```cpp
vlink::Getter<int> g3("ddsc://config/volume");
g3.attach(&loop);
g3.set_latency_and_lost_enabled(true);
g3.listen([&g3](const int& v) {
  VLOG_I("volume=", v, " latency=", g3.get_latency(), "ns");
});
vlink::SampleLostInfo lost = g3.get_lost();
```

URL 采用 `ddsc://`（CycloneDDS），切换为 `intra://` / `dds://` 亦可运行。

## 🔀 模型选择

| 需求 | 选用模型 |
|------|----------|
| 共享最新状态、晚加入也要读到上一次的值 | Field（本示例） |
| 仅广播事件流、不缓存 latest | Event（`Publisher` / `Subscriber`） |
| 请求 / 响应或单向命令 | Method（`Client` / `Server`） |

## 🔗 参考

- `../../quickstart/hello_field/` —— Field 基础用法（Setter + Getter）。
- `../event_advanced/` —— Event 模型的检测 / 延迟统计对照。
- `include/vlink/getter.h`、`setter.h` —— 接口定义。
- 顶层 `doc/02-communication.md` —— Field 模型规范。
