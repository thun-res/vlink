# schedule — `Schedule::Config` 链式任务调度

`Schedule::Config` 是 vlink 任务调度的统一配置载体，用于 `MessageLoop` / `MultiLoop` / `ThreadPool::exec_task`。它把"延迟、优先级、入队超时、执行超时"四个维度封装成一个结构，配合链式 `on_then` / `on_else` / `on_catch` / `on_*_timeout` 实现"任务 + 后续动作"的声明式写法。

本示例覆盖：

- `Schedule::Config` 三种典型构造方式。
- 返回 void 的任务 + `Status` 链式（异常 / 超时）。
- 返回 bool 的任务 + `RetStatus` 链式（on_then / on_else 短路）。
- 异常拦截、优先级、status 有效性。

## 背景与适用场景

适用：

- 需要把"任务 + 失败处理 + 超时处理"声明式组合在一处的场景。
- 类 promise/future 编程风格，但更轻量。
- 想给单个任务设置 schedule_timeout / execution_timeout 的场景。

不适合：

- 不需要任何后续动作的简单任务（直接 `post_task` 即可，更便宜）。
- 严格的 Future composition（用 `std::future` + `invoke_task`）。

## 核心 API

| API | 签名/形式 | 说明 |
|-----|----------|------|
| `Schedule::Config` | `{ delay_ms=0, priority=0, schedule_timeout_ms=0, execution_timeout_ms=0 }` | 调度参数 |
| `Schedule::Config(delay)` | 单参 | 仅设 delay_ms |
| `Schedule::Config(delay, prio, sched_to, exec_to)` | 全参 | |
| `MessageLoop::exec_task` | `template <typename Callable> Schedule::RetStatus exec_task(const Config&, Callable&&)` | 返回链式状态 |
| `Status::on_catch` | `Status& on_catch(Function<void(std::exception&)>&&)` | 异常处理 |
| `Status::on_schedule_timeout` | `Status&` | 入队超时回调 |
| `Status::on_execution_timeout` | `Status&` | 执行超时回调 |
| `Status::is_valid` | `bool is_valid() const` | 句柄是否有效 |
| `RetStatus::on_then` | `RetStatus& on_then(Function<bool()>&&)` | 上一步返回 true 时调 |
| `RetStatus::on_else` | `RetStatus& on_else(Function<void()>&&)` | 上一步返回 false 时调（短路） |

## 代码导读

### 1. Schedule::Config 构造

```cpp
Schedule::Config def;                                 // 全 0：立刻执行、默认优先级、无超时
Schedule::Config full(100, 200, 5000, 3000);          // delay=100, prio=200, sched_to=5s, exec_to=3s
Schedule::Config delay_only(50);                      // 只设 delay
```

### 2. void 任务 + Status

```cpp
MessageLoop loop;
loop.async_run();

loop.exec_task(Schedule::Config{}, []() { VLOG_I("  immediate void task"); })
    .on_catch([](std::exception& e) { MLOG_E("  exception: {}", e.what()); });

loop.exec_task(Schedule::Config{100, 0, 500, 500}, []() { VLOG_I("  delayed 100ms void task"); })
    .on_schedule_timeout([]() { VLOG_W("  schedule timeout"); })
    .on_execution_timeout([]() { VLOG_W("  execution timeout"); })
    .on_catch([](std::exception& e) { MLOG_E("  caught: {}", e.what()); });
```

void 任务返回 `Status`，可链式注册 `on_catch` / `on_*_timeout`。

### 3. bool 任务 + RetStatus

```cpp
loop.exec_task(Schedule::Config{},
               []() -> bool {
                 VLOG_I("  bool task -> true");
                 return true;
               })
    .on_then([]() -> bool {
      VLOG_I("  on_then(1)");
      return true;
    })
    .on_then([]() -> bool {
      VLOG_I("  on_then(2) -> false stops chain");
      return false;
    })
    .on_then([]() -> bool {
      VLOG_I("  on_then(3) NOT called");
      return true;
    })
    .on_else([]() { VLOG_I("  on_else: triggered after on_then(2) false"); });
```

链路语义：任务/每个 on_then 都返回 bool。true 继续下一个 on_then；false 跳到第一个 on_else 并终止链。

### 4. 异常拦截

```cpp
loop.exec_task(Schedule::Config{}, []() { throw std::runtime_error("simulated"); })
    .on_catch([](std::exception& e) { MLOG_I("  on_catch caught: '{}'", e.what()); });
```

任务里抛 `std::exception` 派生异常被 `on_catch` 接住；未注册时异常被吞掉并打 Error 日志。

### 5. 优先级 + Status 有效性

```cpp
MessageLoop loop(MessageLoop::kPriorityType);
loop.async_run();
loop.exec_task(Schedule::Config{0, MessageLoop::kLowestPriority}, []() { VLOG_I("  [LOW]"); });
loop.exec_task(Schedule::Config{0, MessageLoop::kHighestPriority}, []() { VLOG_I("  [HIGH]"); });
loop.exec_task(Schedule::Config{0, MessageLoop::kNormalPriority}, []() { VLOG_I("  [NORMAL]"); });

auto status = loop.exec_task(Schedule::Config{}, []() { VLOG_I("  task body"); });
MLOG_I("  is_valid={}", status.is_valid());
```

`is_valid()` 用于判断 status 句柄是否对应一个真正入队的任务（队满或其它原因可能拒绝）。

## 运行

```bash
./build/output/bin/example_schedule
```

预期输出（节选）：

```
=== Schedule::Config ===
  default: delay=0ms prio=0 sched_to=0ms exec_to=0ms
  full: delay=100ms prio=200 sched_to=5000ms exec_to=3000ms
=== Void callback (Status) ===
  immediate void task
  delayed 100ms void task
=== Bool callback (RetStatus) ===
  --- returns true ---
  bool task -> true
  on_then(1)
  on_then(2) -> false stops chain
  on_else: triggered after on_then(2) false
  --- returns false ---
  bool task -> false
  on_else: failure path
=== Exception handling ===
  on_catch caught: 'simulated'
=== Priority scheduling ===
  [HIGH]
  [NORMAL]
  [LOW]
=== Status validity ===
  task body
  is_valid=1
```

## 常见陷阱

1. **`on_catch` 不注册**：异常被吞掉只打日志；要传播必须显式 rethrow。
2. **`execution_timeout` 不强制中断**：vlink 不杀任务线程，只触发回调。
3. **bool task 返回 false 时 on_then 全跳过**：是按设计；不是 bug。
4. **RetStatus 必须立即链式调**：保存到变量后 `.on_then` 仍可链式，但跨线程调用注册需要确保 Status 还有效。
5. **`Schedule::Config{0, 0, 0, 0}` 与 `{}` 等价**：所有字段默认 0；不会做超时控制。

## 设计要点

- Status / RetStatus 内部用引用计数管理回调注册；过早析构会让回调被丢弃。
- `on_then` 与 `on_else` 是 mutually exclusive 链：true 路径走 on_then 链；首次 false 跳 on_else 终止。
- 超时是软超时；触发回调但不中断任务执行。

## 配图

无专属配图。

## 参考

- `../message_loop_basic/` — MessageLoop 入门
- `../message_loop_advanced/` — 多种 queue 类型 + 调度策略
- `../task_handle/` — 带 cancellation_token 的 TaskHandle
- `vlink/include/vlink/base/schedule.h` — Schedule::Config / Status / RetStatus 接口
