# field_basic — Field 模型基础：Setter / Getter + 最新值缓存

Field 模型解决"状态共享"问题：一方负责维护当前值（`Setter<T>`），任意多个观察方按需读取（`Getter<T>`）。它和 Event 模型最大的区别是**缓存语义**：Setter 写入的最新值会被 vlink 框架保留在传输层，**晚加入的 Getter 也能立刻拿到当前值**，不需要等下一次写入。

读完本示例你能掌握：

- `Setter<T>` 写入和 `Getter<T>` 三种读取方式（`wait_for_value` 同步阻塞、`get()` 取最新值、`listen()` 变化回调）。
- "晚加入也能读到当前状态"的行为在工程上意味着什么 —— 配置中心、状态共享、参数下发都可以靠 Field 模型替代轮询。
- Setter 和 Getter 各自的生命周期约束。

## 背景与适用场景

适用场景：

- 设备工作模式、档位、运行配置（变化频率低、读取频率高）。
- 多模块共享的全局状态（如运行/暂停开关、当前选定算法版本）。
- 标定参数、阈值、运行时可调参数下发。

不适合：

- 流式数据（应该用 Event 模型）。
- 需要请求响应语义（应该用 Method 模型）。
- 每次写入都必须送达每个订阅者、且不能合并（Field 在中间合并相同/连续写入）。

VLink 内部 Field 模型在传输层是"广播 + 最新值缓存"：Setter 的每次 `set()` 都会广播给已经在线的 Getter；同时框架在传输层维护一份 latest，新来的 Getter 通过 discovery 协商把这份 latest 取过来。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `vlink::Setter<T>` | `explicit Setter(const std::string& url, InitType type = kWithInit)` | 写入端 |
| `vlink::Setter<T>::set` | `void set(const T& value)` | 写入新值；默认行为不去重，每次都广播 |
| `vlink::Getter<T>` | `explicit Getter(const std::string& url, InitType type = kWithInit)` | 读取端 |
| `vlink::Getter<T>::attach` | `void attach(MessageLoop* loop)` | 把变化回调投递到指定 loop |
| `vlink::Getter<T>::wait_for_value` | `bool wait_for_value(std::chrono::milliseconds timeout)` | 阻塞等首次拿到值；超时 false |
| `vlink::Getter<T>::get` | `std::optional<T> get() const` | 当前最新值；从未写入则 `std::nullopt` |
| `vlink::Getter<T>::listen` | `bool listen(MsgCallback&& cb)` | 注册变化回调；本基础示例每次 set 都触发 |

## 代码导读

### 1. 启动后台 loop

```cpp
MessageLoop loop;
loop.set_name("getter_loop");
loop.async_run();
```

`async_run()` 让 loop 在后台线程跑；主线程接下来连续做 `set()` / `get()`，需要 loop 不阻塞主线程。

### 2. Setter 写入初始值（先于 Getter 启动）

```cpp
Setter<GearState> setter(kUrl);
setter.set({0, true});       // gear=0 (Park), engaged=true
VLOG_I("[setter] initial gear=0 (Park)");

std::this_thread::sleep_for(50ms);
```

这里**故意**让 Setter 先 set 再让 Getter 上线，演示 Field 的核心卖点：晚加入的 Getter 也能拿到这份初始值。50ms 是 discovery 完成的窗口。

### 3. Getter 晚加入并 wait_for_value

```cpp
Getter<GearState> getter(kUrl);
getter.attach(&loop);

if (getter.wait_for_value(2000ms)) {
  VLOG_I("[getter] wait_for_value succeeded");
}

auto current = getter.get();
if (current.has_value()) {
  VLOG_I("[getter] current gear=", current->gear, " engaged=", current->engaged);
}
```

`wait_for_value(2000ms)` 阻塞当前线程，最多等 2 秒；在那之前 Getter 一旦同步到 latest（不管是新写入还是 discovery 拉取的初始值）就立刻返回 true。

`get()` 返回 `std::optional<T>`：从未拿到值时为 `nullopt`。本示例此时一定能拿到，因为前面 Setter 已经写过。

### 4. 注册变化回调

```cpp
std::atomic<int> change_count{0};
getter.listen([&change_count](const GearState& gear) {
  VLOG_I("[getter] changed gear=", gear.gear, " engaged=", gear.engaged);
  change_count.fetch_add(1);
});
```

`listen()` 注册回调，每次 Setter 调 `set()` 都会触发（默认 `change_reporting=false`：哪怕值没变也触发）。回调跑在 `attach()` 指定的 loop 线程。

### 5. 连续 set 触发变化回调

```cpp
GearState gears[] = {{2, true}, {3, true}, {4, true}, {5, true}, {0, true}};

for (const auto& gear : gears) {
  setter.set(gear);
  VLOG_I("[setter] set gear=", gear.gear);
  std::this_thread::sleep_for(100ms);
}

loop.wait_for_idle(1000);
```

5 次 set 后等 loop 把 5 个回调全跑完。

### 6. 最终查询

```cpp
auto final_val = getter.get();
if (final_val.has_value()) {
  VLOG_I("[getter] final gear=", final_val->gear, " engaged=", final_val->engaged);
}
```

`get()` 永远返回 latest 的同步快照，不阻塞。

## 运行

```bash
./build/output/bin/example_field_basic
```

预期输出：

```
[setter] initial gear=0 (Park)
[getter] wait_for_value succeeded
[getter] current gear=0 engaged=1
[getter] changed gear=0 engaged=1
[setter] set gear=2
[getter] changed gear=2 engaged=1
...
[setter] set gear=0
[getter] changed gear=0 engaged=1
[getter] final gear=0 engaged=1
change_count=6
```

URL `dds://vehicle/gear` 需要启用 FastDDS 组件；切到 `intra://vehicle/gear` 也能跑。

## 常见陷阱

1. **Setter 析构早于 Getter**：本示例里 Setter 和 Getter 在同一 main 作用域。生产代码要保证 Setter 至少与最后一次写入同生命周期，否则后续 Getter 拿不到 latest。
2. **`wait_for_value` 等的不是 discovery 完成，而是真正拿到值**。timeout 内必须收到一份数据，否则返回 false。
3. **`Getter::listen` 默认每次 set 都触发** —— 即便值与上次完全相同，回调仍被调。`field_advanced` 用 `set_change_reporting(true)` 关掉重复触发。
4. **`get()` 返回的是值拷贝**，大对象有开销；高频读取要么自己缓存，要么走 zerocopy 路径。
5. **跨进程时序**：Setter 和 Getter 在不同进程时，必须等 discovery 完成（100-500ms）；用 `wait_for_value` 而非裸 `get()`。

## 设计要点

- Field 模型在传输层有 latest 缓存的语义；不同后端实现方式不同（DDS 用 `DurabilityKind::TransientLocal`，shm 用共享内存中的固定槽位）。
- `set()` 默认不做去重 —— 用 `set_change_reporting(true)` 才能在值没变时跳过回调。
- `Getter<T>::listen` 的回调与 `wait_for_value()` / `get()` 并发安全，可以同时使用。

## 配图

无专属配图。Field 在三种模型中的角色见 `../event_basic/images/communication-models-overview.png`。

## 参考

- `../field_advanced/` — `set_change_reporting`、晚加入同步、多 Getter 扇出、延迟统计
- `../../quickstart/hello_field/` — 最短的 Setter/Getter 示例（30 行）
- `../event_basic/` — Event 模型对照
- `vlink/include/vlink/setter.h`、`getter.h` — Setter/Getter 完整接口
- 顶层 `doc/05-field-model.md` — Field 模型规范
