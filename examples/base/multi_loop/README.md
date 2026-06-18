# multi_loop — `vlink::MultiLoop` N 线程共享队列

`vlink::MultiLoop` 把 `MessageLoop` 扩展为 N 个 worker 线程共享同一任务队列。它继承 `MessageLoop` 的全部 API（post_task / exec_task / invoke_task / 生命周期），区别是任务**并行**执行而不是串行。

读完本示例你能掌握：

- `MultiLoop(N)` 与 `MessageLoop` 的接口共性与语义差异。
- 与 `ThreadPool` 的对比和选择。
- 优先级队列 + 多 worker 的行为。
- 共享状态需要应用层加锁保护。

## 背景与适用场景

适用：

- 想要 MessageLoop 的 API（包括 `exec_task`、生命周期 hook、`invoke_task`），但需要并行执行的场景。
- vlink 内部许多组件（如某些插件）需要"看起来像 loop"的执行环境，但又能 scale 到 N 核。
- 集中管理一组同质 worker 的统一入口。

不适合：

- 任务严格 FIFO（用 MessageLoop）。
- 极简的"并行执行 N 个 callable"（直接 ThreadPool 更简单）。

与 ThreadPool 的区别：

- MultiLoop 继承 MessageLoop 的所有 API（特别是 `exec_task` + Schedule::Config 链式）。
- MultiLoop 适合需要"loop 抽象"的代码复用；ThreadPool 是更纯粹的执行器。
- 性能差异通常不大；按 API 喜好选。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `MultiLoop(uint32_t threads, QueueType = kNormalType)` | 构造 | N worker 共享队列 |
| `MultiLoop::kNormalType` / `kLockfreeType` / `kPriorityType` | enum | 同 MessageLoop |
| 继承 `MessageLoop` | `async_run` / `run` / `quit` / `wait_for_quit` / `wait_for_idle` / `post_task` 等全部 | 行为一致 |
| `post_task_with_priority` | 同 MessageLoop | 仅在 kPriorityType 下生效 |
| `invoke_task` / `invoke_task_with_priority` | 同 MessageLoop | 返回 future |
| `exec_task` | 同 MessageLoop | 配合 Schedule::Config |
| `is_in_same_thread` | `bool is_in_same_thread() const` | 当前线程是否是任一 worker |

## 代码导读

### 1. 基础多线程派发

```cpp
MultiLoop loop(4);
loop.set_name("multi_4");
loop.async_run();

std::atomic<int> completed{0};
for (int i = 0; i < 20; ++i) {
  loop.post_task([i, &completed]() {
    MLOG_I("  task {} executing", i);
    std::this_thread::sleep_for(20ms);
    completed.fetch_add(1);
  });
}

loop.wait_for_idle();
```

4 worker 并行处理 20 个任务；每个任务模拟 20ms 工作量，总耗时约 100-130ms（约 5 轮）。

### 2. 线程身份

```cpp
MultiLoop loop(2);
loop.async_run();
MLOG_I("  main is_in_same_thread={}", loop.is_in_same_thread());     // 0
loop.post_task([&loop]() { MLOG_I("  worker is_in_same_thread={}", loop.is_in_same_thread()); });   // 1
```

`is_in_same_thread()` 返回当前线程是否是该 MultiLoop 的 worker 之一。

### 3. 优先级队列

```cpp
MultiLoop loop(2, MultiLoop::kPriorityType);
loop.async_run();

loop.post_task_with_priority([]() { VLOG_I("  [LOW]"); }, MultiLoop::kLowestPriority);
loop.post_task_with_priority([]() { VLOG_I("  [HIGH]"); }, MultiLoop::kHighestPriority);
loop.post_task_with_priority([]() { VLOG_I("  [NORMAL]"); }, MultiLoop::kNormalPriority);
```

优先级队列上 high → normal → low 顺序出队；2 worker 并行执行不会绝对保证打印顺序，但出队顺序受优先级控制。

### 4. invoke_task

```cpp
auto future = loop.invoke_task([]() -> int {
  std::this_thread::sleep_for(30ms);
  return 99;
});
MLOG_I("  result={}", future.get());
```

与 MessageLoop 一致。

### 5. exec_task + Schedule

```cpp
loop.exec_task(Schedule::Config{0}, []() { VLOG_I("  immediate exec_task"); })
    .on_catch([](std::exception& e) { MLOG_E("  exception: {}", e.what()); });

loop.exec_task(Schedule::Config{100}, []() { VLOG_I("  delayed 100ms exec_task"); });
```

`exec_task` 在 MultiLoop 上同样可用，但**任务执行不串行** —— `on_then` 等回调依然按 promise 链触发，但是其它任务可能在中间穿插。

### 6. 并行累加

```cpp
std::atomic<int64_t> sum{0};
static constexpr int kTasks = 100;
for (int i = 1; i <= kTasks; ++i) {
  loop.post_task([i, &sum]() { sum.fetch_add(i); });
}

loop.wait_for_idle();
MLOG_I("  sum 1..{} = {} (expected {})", kTasks, sum.load(), kTasks * (kTasks + 1) / 2);
```

最终 sum=5050；atomic 保证累加正确。

## 运行

```bash
./build/output/bin/example_multi_loop
```

预期输出（节选）：

```
=== Basic dispatch (4 workers) ===
  task 0 executing
  task 1 executing
  ...
  completed=20/20
=== Thread identity ===
  main is_in_same_thread=0
  worker is_in_same_thread=1
=== Priority queue ===
  [HIGH]
  [NORMAL]
  [LOW]
=== invoke_task ===
  result=99
=== exec_task with Schedule ===
  immediate exec_task
  delayed 100ms exec_task
=== Parallel sum ===
  sum 1..100 = 5050 (expected 5050)
MultiLoop example finished.
```

## 常见陷阱

1. **任务跨 worker 共享可变状态**：MultiLoop 不串行执行任务；必须自己加锁或用 atomic。
2. **`exec_task` 假定串行**：`on_then` 链按 promise 触发，但中间任务可能并行；不要假设依赖。
3. **优先级在多 worker 下不严格**：高优先级任务先出队但不一定先完成（取决于 worker 调度）。
4. **future.get() 在 worker 内调用**：可能死锁。`is_in_same_thread()` 检查。
5. **wait_for_idle 不等正在执行的任务**：等的是"队列空"；正在执行的任务还在跑。要等所有执行完用其它同步。

## 设计要点

- MultiLoop 内部就是 N 个 worker 共享 MessageLoop 队列；继承自 MessageLoop。
- 适合"想要 MessageLoop API + 并行执行"的场景；性能与 ThreadPool 相近。
- 与 `exec_task` 配合时 timeout / on_catch 都按任务粒度生效。

## 配图

无专属配图。

## 参考

- `../message_loop_basic/` — 单线程 baseline
- `../message_loop_advanced/` — MessageLoop 进阶 API（也都适用 MultiLoop）
- `../thread_pool/` — 另一种 N 线程执行器
- `../graph_task/` — 基于 MultiLoop 的 DAG
- `vlink/include/vlink/base/multi_loop.h` — MultiLoop 接口
