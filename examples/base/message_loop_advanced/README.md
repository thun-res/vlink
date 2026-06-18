# message_loop_advanced — MessageLoop 进阶：队列类型、调度策略、Schedule::Config、`exec_task`

本示例覆盖 MessageLoop 在生产环境常用的几组进阶能力：

- **三种队列类型**：`kNormalType` / `kLockfreeType` / `kPriorityType`，按场景选最合适的内部数据结构。
- **两种 dispatch 策略**：`kBlockStrategy`（条件变量阻塞）vs `kPopStrategy`（自旋轮询）。
- **`Schedule::Config` 驱动的高阶任务**：延迟执行、优先级、超时回调、异常捕获。
- **链式 `on_then` / `on_else`**：根据 bool 返回值串联回调。
- **`invoke_task` / `invoke_task_with_priority`**：返回 `std::future<R>`，在 loop 线程上执行后把结果同步给调用方。

读完本示例你能掌握：

- 三种队列类型对吞吐 / 公平性 / 延迟的影响。
- `Schedule::Config` 比 `post_task` 多了哪些可调维度。
- 何时该用 `invoke_task` 而非 `post_task`。

## 背景与适用场景

`message_loop_basic` 演示了 MessageLoop 的"经典"形态。但 vlink 在底层提供了更细的可调维度，覆盖几类工程需求：

- **高吞吐 + 低争抢**：用 `kLockfreeType` 队列（基于 MPMC lockfree 实现），消除 mutex 争抢。
- **任务有优先级**：紧急任务（命令、错误处理）需要插队，用 `kPriorityType`。
- **任务有截止时间**：需要"如果 X 毫秒内未开始执行就放弃" / "执行超过 Y 毫秒打日志"，用 `exec_task` + `Schedule::Config`。
- **任务有结果**：业务想拿到任务返回值（int / std::string / 自定义类型），用 `invoke_task` 拿 future。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `MessageLoop(MessageLoopType)` | `enum { kNormalType, kLockfreeType, kPriorityType }` | 选队列类型 |
| `post_task_with_priority` | `bool post_task_with_priority(Function<void()>&&, Priority)` | 优先级队列上投递 |
| `set_strategy` / `get_strategy` | `void set_strategy(Strategy)` | `kBlockStrategy` 或 `kPopStrategy` |
| `exec_task` | `template <typename Callable> Schedule::RetStatus exec_task(const Schedule::Config&, Callable&&)` | 带 Config 的任务投递 |
| `Schedule::Config` | `{ delay_ms, priority, schedule_timeout_ms, execution_timeout_ms }` | 任务执行约束 |
| `RetStatus::on_schedule_timeout` | 链式 | 入队后超过 schedule_timeout 仍未开始执行 |
| `RetStatus::on_execution_timeout` | 链式 | 执行超时 |
| `RetStatus::on_catch` | 链式 | 捕获异常 |
| `RetStatus::on_then` | `on_then(Function<bool()>)` | 上一步返回 true 时调用 |
| `RetStatus::on_else` | `on_else(Function<void()>)` | 上一步返回 false 时调用 |
| `invoke_task` | `template <typename Callable> std::future<R> invoke_task(Callable&&)` | 同步等任务返回值 |
| `invoke_task_with_priority` | 同上 + Priority | 优先级队列上等返回值 |

## 代码导读

### 1. 三种队列类型

```cpp
MessageLoop normal_loop(MessageLoop::kNormalType);
normal_loop.async_run();
normal_loop.post_task([]() { VLOG_I("  [Normal] task"); });

MessageLoop lockfree_loop(MessageLoop::kLockfreeType);
lockfree_loop.async_run();
lockfree_loop.post_task([]() { VLOG_I("  [Lockfree] task"); });

MessageLoop priority_loop(MessageLoop::kPriorityType);
priority_loop.async_run();
priority_loop.post_task_with_priority([]() { VLOG_I("  [Priority] low"); }, MessageLoop::kLowestPriority);
priority_loop.post_task_with_priority([]() { VLOG_I("  [Priority] high"); }, MessageLoop::kHighestPriority);
priority_loop.post_task_with_priority([]() { VLOG_I("  [Priority] normal"); }, MessageLoop::kNormalPriority);
```

- `kNormalType`：mutex + condition_variable，公平 FIFO；默认。
- `kLockfreeType`：MPMC lockfree 队列；高并发投递场景延迟稳定。
- `kPriorityType`：堆排序队列，允许优先级插队。

priority loop 上的任务**按优先级出队**：上面三次 post 后顺序是 high → normal → low。

### 2. dispatch 策略

```cpp
loop.set_strategy(MessageLoop::kBlockStrategy);   // mutex+cv 阻塞等
loop.set_strategy(MessageLoop::kPopStrategy);     // 自旋轮询，亚微秒唤醒
```

- `kBlockStrategy`：队列空时线程 condition_variable 阻塞等待；唤醒延迟约 1-10us。
- `kPopStrategy`：队列空时主动 yield 自旋检查；亚微秒唤醒但 CPU 占用高。

### 3. exec_task + Schedule::Config

```cpp
loop.exec_task(Schedule::Config{0, 100}, []() { VLOG_I("  immediate, priority=100"); });

loop.exec_task(Schedule::Config{200}, []() { VLOG_I("  delayed 200ms"); })
    .on_schedule_timeout([]() { VLOG_W("  schedule timeout"); })
    .on_execution_timeout([]() { VLOG_W("  execution timeout"); })
    .on_catch([](std::exception& e) { MLOG_E("  caught: {}", e.what()); });
```

`Schedule::Config{delay_ms, priority, schedule_timeout_ms, execution_timeout_ms}` 控制：

- `delay_ms`：投递后延迟多久才能开始执行。
- `priority`：在 priority loop 上的优先级。
- `schedule_timeout_ms`：入队后最长等待多久；超时 `on_schedule_timeout` 被调。
- `execution_timeout_ms`：执行最长时长；超时 `on_execution_timeout` 被调（**不会强行中断任务**）。

链式 `.on_*` 同步注册，按各自时机被调。

### 4. on_then / on_else 链

```cpp
loop.exec_task(Schedule::Config{},
               []() -> bool {
                 VLOG_I("  bool task -> true");
                 return true;
               })
    .on_then([]() -> bool { VLOG_I("  on_then(1)"); return true; })
    .on_then([]() -> bool { VLOG_I("  on_then(2)"); return true; })
    .on_else([]() { VLOG_I("  on_else (NOT called)"); });

loop.exec_task(Schedule::Config{},
               []() -> bool {
                 return false;
               })
    .on_then([]() -> bool { return true; })
    .on_else([]() { VLOG_I("  on_else: failure path"); });
```

callable 返回 bool 时，`on_then` 在 true 时被调（可链式继续返回 bool 串接），`on_else` 在 false 时被调（短路）。类似简化版 promise/future 链。

### 5. invoke_task 拿返回值

```cpp
auto fut_int = loop.invoke_task([]() -> int {
  VLOG_I("  computing on loop thread...");
  return 42;
});
MLOG_I("  invoke result: {}", fut_int.get());

auto fut_str = loop.invoke_task([]() -> std::string { return "hello from loop"; });
MLOG_I("  invoke string: {}", fut_str.get());
```

`invoke_task` 内部用 `std::promise/std::future` 把任务结果回传调用线程。`.get()` 阻塞等待任务在 loop 上完成。

### 6. invoke_task_with_priority

```cpp
auto high = loop.invoke_task_with_priority([]() -> int { return 1; }, MessageLoop::kHighestPriority);
auto low  = loop.invoke_task_with_priority([]() -> int { return 2; }, MessageLoop::kLowestPriority);

MLOG_I("  high={} low={}", high.get(), low.get());
```

优先级队列 + future 返回值的组合。

## 运行

```bash
./build/output/bin/example_message_loop_advanced
```

预期输出（节选）：

```
=== Queue types ===
  [Normal] task
  [Lockfree] task
  [Priority] high
  [Priority] normal
  [Priority] low
=== Dispatch strategies ===
  kBlockStrategy
  kPopStrategy
=== exec_task with Schedule::Config ===
  immediate, priority=100
  delayed 200ms
=== on_then / on_else chaining ===
  bool task -> true
  on_then(1)
  on_then(2)
  bool task -> false
  on_else: failure path
=== invoke_task ===
  computing on loop thread...
  invoke result: 42
  invoke string: hello from loop
=== invoke_task_with_priority ===
  high=1 low=2
MessageLoop advanced example finished.
```

## 常见陷阱

1. **`post_task_with_priority` 在非 priority loop 上调**：行为退化为忽略优先级；要么换 priority loop，要么用普通 `post_task`。
2. **`kLockfreeType` + `invoke_task`**：fut.get() 在 loop 线程内调会死锁。
3. **on_then 链很长**：每一环都是同步调用，串行执行；任务多就拆分。
4. **`execution_timeout` 不真的中断任务**：vlink 不会 kill 任务，只触发回调。长任务还是会跑完。
5. **`Schedule::Config{200}` 是 delay_ms=200**：单参形态；其它字段保持默认。

## 设计要点

- 三种队列类型在内存布局、缓存友好度、并发争抢上各有取舍；切换前先用 microbench 比较。
- `Schedule::Config` 的 timeout 是软超时；vlink 不强制中断已开始的任务。
- `invoke_task` 内部要分配 promise/future（堆分配）；微秒级热路径用 atomic + `post_task` 更便宜。

## 配图

无专属配图。MessageLoop 内部结构见 `../message_loop_basic/images/message-loop-architecture.png`。

## 参考

- `../message_loop_basic/` — MessageLoop 入门
- `../schedule/` — `Schedule::Config` 在 Timer 等 API 上的更多用法
- `../multi_loop/` — 多 loop 并行
- `../thread_pool/` — ThreadPool 与 MessageLoop 的取舍
- `vlink/include/vlink/base/message_loop.h` — MessageLoop 完整接口
