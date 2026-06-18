# timer — `vlink::Timer` 完整用法：周期、单次、动态调整

`vlink::Timer` 是事件驱动的定时器，**必须挂在一个 `MessageLoop` 上**，回调跑在 loop 线程上、与其它 loop 任务串行。一个 MessageLoop 上最多挂 100 个 Timer（`kMaxTimerSize`）。

读完本示例你能掌握：

- 5 种典型 Timer 用法：无限循环、单次触发、有限次循环、跨 loop 迁移、动态修改 interval / loop_count。
- Timer 与 MessageLoop 的绑定与解绑流程。
- `kInfinite` / `call_once` 的工程语义。

## 背景与适用场景

适用场景：

- 周期任务：心跳、轮询、传感器采样驱动、状态机 tick。
- 延迟任务：N 秒后做一件事（用 `call_once` 或 `loop_count=1`）。
- 有限次循环：用于状态机里"重试 N 次"等。

不适合：

- 微秒级精度（vlink Timer 基于 wheel timer + condition_variable，典型抖动 0.1-1ms）。
- 完全异步（独立线程）—— Timer 回调一定跑在挂的 loop 上。

合并自原 `timer_basic` + `timer_advanced`，提供从基础到进阶的完整覆盖。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Timer(MessageLoop*, uint32_t interval_ms, int loop_count, Function<void()>&&)` | 完整构造 | 立刻 attach 到 loop |
| `Timer(uint32_t interval_ms, int loop_count)` | 简化构造 | 不绑定 loop；后续 attach |
| `Timer::start` | `void start()` | 启动定时器 |
| `Timer::stop` | `void stop()` | 停止 |
| `Timer::restart` | `void restart()` | 停 → 启动；常用于 loop_count 或 interval 改后 |
| `Timer::attach` | `void attach(MessageLoop*)` | 绑定到 loop |
| `Timer::detach` | `void detach()` | 解绑（必须停后才能解） |
| `Timer::set_interval` | `void set_interval(uint32_t ms)` | 改 interval；下次 start 生效 |
| `Timer::set_loop_count` | `void set_loop_count(int)` | 改 loop_count；下次 start 生效 |
| `Timer::set_callback` | `void set_callback(Function<void()>&&)` | 改回调 |
| `Timer::is_active` | `bool is_active() const` | 是否在跑 |
| `Timer::get_interval` | `uint32_t get_interval() const` | 当前 interval |
| `Timer::set_strict` | `void set_strict(bool)` | 严格补偿模式（busy 后补 tick） |
| `Timer::call_once` | `static void call_once(MessageLoop*, uint32_t delay_ms, Function<void()>&&)` | 单次延迟触发的便利函数 |
| `Timer::kInfinite` | constexpr int = -1 | 无限循环 |

## 代码导读

### 1. 无限循环

```cpp
MessageLoop loop;
loop.async_run();

std::atomic<int> count{0};
Timer timer(&loop, 100, Timer::kInfinite, [&count]() {
  int c = count.fetch_add(1) + 1;
  MLOG_I("  tick #{}", c);
});
timer.start();

std::this_thread::sleep_for(550ms);
timer.stop();
```

`loop_count = kInfinite`（-1）让 Timer 一直触发直到 stop。`is_active()` 在 start/stop 之间返回 true。

### 2. 单次触发

```cpp
Timer once(&loop, 150, 1, [&fired]() {
  VLOG_I("  single-fire callback");
  fired.store(true);
});
once.start();
std::this_thread::sleep_for(250ms);
MLOG_I("  fired={} active={}", fired.load(), once.is_active());

Timer::call_once(&loop, 100, []() { VLOG_I("  call_once fired"); });
```

`loop_count=1` 触发一次后 active 变 false。`call_once` 是更紧凑的写法，不需要持有 Timer 对象。

### 3. 有限次循环 + restart

```cpp
Timer timer(&loop, 80, 3, [&count]() { count.fetch_add(1); });
timer.start();              // 触发 3 次后停
std::this_thread::sleep_for(400ms);
MLOG_I("  after 3 ticks: count={} active={}", count.load(), timer.is_active());

count.store(0);
timer.restart();            // 再触发 3 次
std::this_thread::sleep_for(400ms);
MLOG_I("  after restart: count={}", count.load());
```

`restart()` 等价于 stop + start；保留当前 interval/loop_count/callback 配置。

### 4. 跨 loop 迁移

```cpp
MessageLoop loop_a, loop_b;
loop_a.async_run();
loop_b.async_run();

Timer timer(100, Timer::kInfinite);      // 不绑定 loop 的构造
timer.attach(&loop_a);
timer.set_callback([&a_count]() { a_count.fetch_add(1); });
timer.start();
std::this_thread::sleep_for(350ms);
timer.stop();

timer.detach();
timer.attach(&loop_b);
timer.set_callback([&b_count]() { b_count.fetch_add(1); });
timer.start();
```

`detach()` 必须先 `stop()`。迁移到另一 loop 后，回调跑在新 loop 线程上。

### 5. 动态修改 interval

```cpp
Timer timer(&loop, 100, Timer::kInfinite, [&count]() { count.fetch_add(1); });
timer.start();
std::this_thread::sleep_for(250ms);
timer.stop();
MLOG_I("  phase 1 (100ms): {} ticks", count.load());

count.store(0);
timer.set_interval(50);
timer.set_loop_count(6);
timer.restart();
```

`set_interval` / `set_loop_count` 修改下次 start 生效的参数。常见模式：先以慢速跑、达到某个条件后切到快速。

## 运行

```bash
./build/output/bin/example_timer
```

预期输出（节选）：

```
=== Repeating timer (kInfinite) ===
  active=1 interval=100ms
  tick #1
  tick #2
  tick #3
  tick #4
  tick #5
  stopped after 5 ticks
=== Single-fire (loop_count=1) ===
  single-fire callback
  fired=1 active=0
  call_once fired
=== Bounded loop_count + restart ===
  after 3 ticks: count=3 active=0
  after restart: count=3
=== attach / detach across loops ===
  loop_a ticks=3
  loop_b ticks=3
=== Dynamic interval change ===
  phase 1 (100ms): 2 ticks
  phase 2 (50ms x 6): 6 ticks
Timer example finished.
```

## 常见陷阱

1. **未 attach 就 start**：行为 undefined；构造时如果传了 loop 已经 attach；用简化构造时必须显式 attach。
2. **detach 在 active 时调**：行为按实现可能是断言或忽略；务必先 stop。
3. **回调阻塞**：回调跑在 loop 线程，阻塞会推迟所有后续 Timer / Subscriber 回调。
4. **interval=0 或极小值**：会被钳位到 `kMinInterval` （约 10us）；不会真的 busy spin。
5. **跨 loop 时间漂移**：不同 loop 的负载不同，跑同一 interval 的 Timer 实际触发频率可能不同。

## 设计要点

- vlink Timer 基于内部 wheel timer（O(1) 调度），精度约 0.1-1ms。
- `set_strict(true)` 让长时间被阻塞后能"补"漏触发；默认 false（丢掉漏的）。
- 同一 loop 上 Timer 总数受 `kMaxTimerSize=100` 限制；超过会拒绝 attach。
- Timer 是值类型；可以构造在栈上 / 类成员里 / heap 上都行；只需保证它在 stop 前 alive。

## 配图

![Timer Lifecycle](./images/timer-lifecycle.png)

图中展示 Timer 的状态机：Detached → Attached/Stopped → Running → (auto-)Stopped。

## 参考

- `../message_loop_basic/` — 驱动 Timer 的 loop
- `../elapsed_timer/` — 测耗时（不是定时触发）
- `../deadline_timer/` — 绝对截止时间检测
- `../schedule/` — `Schedule::Config` 调度
- `vlink/include/vlink/base/timer.h` — Timer 完整接口
