# field_advanced — Field 模型进阶：变化上报、晚加入同步、扇出、延迟统计

本示例覆盖 Setter/Getter 在生产环境的几项关键能力：

- **变化上报**：`Getter::set_change_reporting(true)` 让回调只在值真正变化时触发，去重连续相同的 set。
- **晚加入同步**：Getter 创建在 Setter 已经 set 之后，仍能通过 `wait_for_value()` 拿到 latest。
- **多 Getter 扇出**：同一 URL 下挂多个 Getter，每次 set 都送给每一个。
- **延迟与丢包统计**：与 Event 模型同名 API，开启后能查询 set→listen 的端到端时延和累计丢包。

这是写配置中心、参数下发、跨模块共享状态时的常用 API 矩阵。`field_basic` 给出了基础形态；本示例把"调一次就够"的高阶 API 集中演示。

## 背景与适用场景

`Getter::listen` 默认在每次 `Setter::set` 调用时都触发回调，即使值没有任何变化。这对"调用方判定变化"场景没问题（订阅者每次都做幂等处理），但当回调有副作用、值是高频心跳、或者订阅者需要精确知道"什么时候真的变了"时，就必须打开 change reporting。

变化的判定是**按字节比较**：`Serializer::serialize<T>(prev) == Serializer::serialize<T>(now)` 时认为相同。对 POD 类型就是 memcmp；对自定义 `operator>>` 序列化的类型由 operator 输出决定。

晚加入同步是 Field 模型的标志性特性：传输层维护 latest，新创建的 Getter 通过 discovery 把 latest 拉过来，调一次 `wait_for_value()` 就能阻塞到这份 latest 到达。

延迟和丢包统计与 Event 模型的同名 API 行为一致，**必须在 `listen()` 之前**调 `set_latency_and_lost_enabled(true)` 才生效。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Getter::set_change_reporting` | `void set_change_reporting(bool enable)` | 开启后只在序列化字节真正变化时触发 `listen` 回调 |
| `Getter::get_change_reporting` | `bool get_change_reporting() const` | 查询当前 change reporting 状态 |
| `Getter::wait_for_value` | `bool wait_for_value(std::chrono::milliseconds timeout)` | 阻塞等到首次拿到值；超时 false |
| `Getter::set_latency_and_lost_enabled` | `void set_latency_and_lost_enabled(bool enable)` | 开启延迟/丢包统计，必须在 listen 前调 |
| `Getter::get_latency` | `int64_t get_latency() const` | 最近一次回调的端到端延迟（微秒） |
| `Getter::get_lost` | `SampleLostInfo get_lost() const` | 累计 `{ total, lost }` |
| `MessageLoop::wait_for_idle` | `bool wait_for_idle(uint32_t timeout_ms)` | 阻塞等到 loop 上待执行任务跑完 |

## 代码导读

### 1. 变化上报：连续相同 set 只触发一次回调

```cpp
Setter<BrightnessConfig> setter("ddsc://display/brightness");
setter.set({50, false});

std::this_thread::sleep_for(50ms);

Getter<BrightnessConfig> getter_cr("ddsc://display/brightness");
getter_cr.attach(&loop);
getter_cr.set_change_reporting(true);  // 关键：开启变化上报

std::atomic<int> cr_count{0};
getter_cr.listen([&cr_count](const BrightnessConfig& cfg) {
  VLOG_I("[cr-getter] changed level=", cfg.level, " auto=", cfg.auto_mode);
  cr_count.fetch_add(1);
});
getter_cr.wait_for_value(1000ms);

setter.set({50, false});   // 与 latest 相同
setter.set({50, false});   // 仍相同
setter.set({75, false});   // 变化 → 触发一次
setter.set({75, false});   // 与新 latest 相同
setter.set({100, true});   // 变化 → 触发一次
```

5 次 set 中只有 2 次值真的变化，因此回调只触发 2 次（再加上 `wait_for_value` 同步过来的初值，总共可能 1 + 2 = 3 次，取决于实现细节；示例预期 ~2 次）。

### 2. 晚加入同步

```cpp
Getter<BrightnessConfig> late_getter("ddsc://display/brightness");
late_getter.attach(&loop);
late_getter.listen([](const BrightnessConfig& cfg) {
  VLOG_I("[late-getter] sync received level=", cfg.level, " auto=", cfg.auto_mode);
});

if (late_getter.wait_for_value(2000ms)) {
  auto val = late_getter.get();
  if (val.has_value()) {
    VLOG_I("[late-getter] get(): level=", val->level, " auto=", val->auto_mode);
  }
}
```

`late_getter` 在 Setter 已经 set 过五次之后才创建，仍然能通过 `wait_for_value` 拿到 latest（此时是 `{100, true}`）。

### 3. 多 Getter 扇出 + 延迟统计

```cpp
Setter<int> multi_setter("ddsc://config/volume");

Getter<int> g1("ddsc://config/volume");
g1.attach(&loop);
g1.listen([&c1](const int& v) {
  VLOG_I("[g1] volume=", v);
  c1.fetch_add(1);
});

Getter<int> g2("ddsc://config/volume");
g2.attach(&loop);
g2.listen([&c2](const int&) { c2.fetch_add(1); });

Getter<int> g3("ddsc://config/volume");
g3.attach(&loop);
g3.set_latency_and_lost_enabled(true);   // 必须在 listen 前
g3.listen([&g3, &c3](const int& v) {
  c3.fetch_add(1);
  VLOG_I("[g3] volume=", v, " latency=", g3.get_latency(), "us");
});
```

三个 Getter 挂在同一 URL `ddsc://config/volume`；接下来 Setter 发 5 个值，三个 Getter 各应收到 5 次回调。`g3` 在每次回调时打印延迟。

### 4. 批量 set 后查询统计

```cpp
for (int vol = 0; vol <= 100; vol += 25) {
  multi_setter.set(vol);
  std::this_thread::sleep_for(50ms);
}

loop.wait_for_idle(1000);

SampleLostInfo lost = g3.get_lost();
VLOG_I("g1=", c1.load(), " g2=", c2.load(), " g3=", c3.load(),
       " (g3 total=", lost.total, " lost=", lost.lost, ")");
```

等 loop 把回调跑完，查询丢包计数。理想状态 `lost.lost == 0`。

## 运行

```bash
./build/output/bin/example_field_advanced
```

预期输出（节选）：

```
[cr-getter] changed level=50 auto=0
[cr-getter] changed level=75 auto=0
[cr-getter] changed level=100 auto=1
[cr-getter] change_reporting=1 callbacks=3 (expect ~2)
[late-getter] sync received level=100 auto=1
[late-getter] get(): level=100 auto=1
[g1] volume=0
[g3] volume=0 latency=85us
[g1] volume=25
[g3] volume=25 latency=72us
...
g1=5 g2=5 g3=5 (g3 total=5 lost=0)
```

URL 用 `ddsc://`（CycloneDDS）；切到 `intra://`、`dds://` 也都能跑。

## 常见陷阱

1. **`set_change_reporting(true)` 在 listen 之后调用** —— 不生效；vlink 在 listen 时锁定 reporting 模式。
2. **`set_latency_and_lost_enabled(true)` 在 listen 之后调用** —— 同上，无效。
3. **change reporting 用 byte 比较** —— 对带 padding 的 POD 结构，未初始化 padding 可能让两次"逻辑相同"的值字节不同；建议用 `T value{}` zero-init 或自定义序列化。
4. **`wait_for_value` 超时不一定意味着 Setter 没在线** —— 也可能是 discovery 还没完成；适当延长 timeout 或加上 `detect_subscribers/connected` 配合。
5. **g3 latency 单位是微秒** —— 跨机时钟不同步时可能为负或异常大。

## 设计要点

- change reporting 在框架内部仍然会反序列化每条到达的消息（要做比较），只是抑制了 listener 回调；高频心跳关闭 reporting 节省 CPU、开启 reporting 节省业务 CPU，按需选择。
- Field 模型的 latest 缓存在传输层；Setter 析构不一定会清掉 latest（取决于 DurabilityKind 和后端实现），跨进程升级时要确认期望行为。
- 三个 Getter attach 到同一 loop，回调串行；要并行就分散到不同 loop。

## 配图

无专属配图。Field 在三种模型中的角色见 `../event_basic/images/communication-models-overview.png`。

## 参考

- `../field_basic/` — Field 基础用法
- `../event_advanced/` — Event 模型的 detect_subscribers / 延迟统计 对照
- `../../qos/` — Field 后端可靠性与 Durability 配置
- `vlink/include/vlink/getter.h`、`setter.h` — 接口定义
- 顶层 `doc/05-field-model.md` — Field 模型规范
