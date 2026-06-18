# task_handle — 带追踪/取消能力的任务提交

`vlink::TaskHandle` 是 `post_task` 的"带追踪"版本。`post_task_handle()` 返回 TaskHandle，让调用方可以 `wait()` 等任务完成、`cancel()` 取消、查询 `state()` 状态、传入 cancellation_token。

读完本示例你能掌握：

- 何时该用 `post_task_handle` 而非 `post_task`。
- 8 种 `TaskExecutionState` 的转换条件。
- `PostTaskOptions` 的 cancellation_token、overflow_policy、drop_policy 三组配置。
- 在 MessageLoop / MultiLoop / ThreadPool 上的一致接口。

## 背景与适用场景

适用：

- 需要等任务完成 → `wait()` / `wait(timeout_ms)`。
- 任务可能要取消 → `cancel()` + `cancellation_token`。
- 监控任务执行状态、统计成功率。
- 多个任务共享同一取消源（批量取消）。

不适合：

- 简单"投递就完事"（用 post_task，省一次堆分配）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `post_task_handle` | `TaskHandle post_task_handle(Function<void()>&&, const PostTaskOptions& = {})` | 返回带 handle 的任务 |
| `post_task_with_priority_handle` | 同上 + Priority | 优先级版本 |
| `PostTaskOptions::cancellation_token` | `CancellationToken` | 父 token；触发时任务被 cancel |
| `PostTaskOptions::overflow_policy` | enum | 队列满时的策略 |
| `PostTaskOptions::drop_policy` | enum | `kDroppable` vs `kProtected` |
| `TaskHandle::wait` | `bool wait(std::chrono::milliseconds = -1)` | -1 等无限；返回 true=完成 |
| `TaskHandle::cancel` | `bool cancel()` | 申请取消 |
| `TaskHandle::state` | `TaskExecutionState state() const` | 当前状态 |
| `TaskHandle::is_done` | `bool is_done() const` | 是否终态 |
| `TaskHandle::cancellation_token` | `CancellationToken cancellation_token() const` | 任务自己的 token，供回调内 poll |

## TaskExecutionState 状态转换

| 状态 | 终态？ | 进入条件 |
|------|------|---------|
| `kInvalid`   | 否 | 默认构造 |
| `kQueued`    | 否 | dispatcher 接受 |
| `kRunning`   | 否 | callback 开始执行 |
| `kCompleted` | 是 | callback 正常返回 |
| `kCancelled` | 是 | `cancel()` 在 queued 期间 / 父 token 触发 |
| `kDropped`   | 是 | overflow drop-oldest 选中 |
| `kRejected`  | 是 | dispatcher 拒绝（quit / 队满 + kReject） |
| `kFailed`    | 是 | callback 抛异常 |

## 代码导读

### 1. 基础 post_task_handle + wait

```cpp
MessageLoop loop;
loop.async_run();

auto h = loop.post_task_handle([]() {
  std::this_thread::sleep_for(50ms);
  VLOG_I("  task body done");
});

h.wait();
MLOG_I("  state={} done={}", static_cast<int>(h.state()), h.is_done());
```

`wait()` 阻塞到任务终态。

### 2. 父 cancellation_token 级联

```cpp
CancellationSource src;
PostTaskOptions opts;
opts.cancellation_token = src.token();

auto h = loop.post_task_handle([token = src.token()]() {
  while (!token.is_cancellation_requested()) {
    std::this_thread::sleep_for(20ms);
  }
}, opts);

std::this_thread::sleep_for(50ms);
src.request_cancel();
h.wait();
MLOG_I("  state after cancel={}", static_cast<int>(h.state()));
```

`PostTaskOptions::cancellation_token` 把外部 token 绑定到任务；触发时任务被 cancel。同时回调内通过 `h.cancellation_token().is_cancellation_requested()` poll。

### 3. cancel() 队列中 vs 执行中

```cpp
auto h_queued = loop.post_task_handle([]() {
  std::this_thread::sleep_for(100ms);
});
h_queued.cancel();                                  // 在 queued 期间取消
h_queued.wait();
// state == kCancelled

auto h_running = loop.post_task_handle([&](){
  std::this_thread::sleep_for(50ms);                 // 已 Running
  // poll 不到取消信号则继续完成
});
std::this_thread::sleep_for(10ms);
h_running.cancel();
h_running.wait();
// state == kCompleted（任务没主动 poll cancel）
```

`cancel()` 是协作式：vlink 不会强行打断 Running 任务；只在 Queued 期间能"挡掉"。

### 4. 任务抛异常 -> kFailed

```cpp
auto h_fail = loop.post_task_handle([]() {
  throw std::runtime_error("boom");
});
h_fail.wait();
MLOG_I("  state={}", static_cast<int>(h_fail.state()));   // kFailed
```

### 5. 优先级 + handle

```cpp
auto h_low = pri_loop.post_task_with_priority_handle([](){ /*...*/ }, MessageLoop::kLowestPriority);
auto h_high = pri_loop.post_task_with_priority_handle([](){ /*...*/ }, MessageLoop::kHighestPriority);
```

### 6. 拒绝 + 丢弃策略

```cpp
PostTaskOptions opts_reject;
opts_reject.overflow_policy = PostTaskOptions::kReject;

// loop.quit() 之后再投递
auto h = loop.post_task_handle([]() {}, opts_reject);
// state == kRejected
```

### 7. ThreadPool 群组取消

```cpp
ThreadPool pool(8);
CancellationSource src;
PostTaskOptions opts;
opts.cancellation_token = src.token();

std::vector<TaskHandle> handles;
for (int i = 0; i < 8; ++i) {
  handles.push_back(pool.post_task_handle([token = src.token(), i]() {
    while (!token.is_cancellation_requested()) {
      std::this_thread::sleep_for(20ms);
    }
  }, opts));
}

std::this_thread::sleep_for(100ms);
src.request_cancel();
for (auto& h : handles) {
  h.wait();
}
```

一个 source 控制 8 个 worker 任务的取消，典型 fan-out 取消模式。

## 运行

```bash
./build/output/bin/example_task_handle
```

预期输出（节选）：

```
task body done
state=3 done=1                                       # 3=kCompleted
state after cancel=4                                 # 4=kCancelled
state=3                                              # 5=kFailed
[LOW] [NORMAL] [HIGH] —— priority queue order
state=6                                              # 6=kRejected
8 workers cancelled in unison
wait(timeout) returned without completion
default handle state=0                               # 0=kInvalid
```

## 常见陷阱

1. **handle 析构 ≠ 取消**：dispatcher 持有强引用；handle 析构后任务仍会跑。
2. **lockfree 队列 + kProtected**：lockfree 不支持 protected，只是打警告；要保护用 kNormal/kPriority。
3. **wait(timeout) 返回 false**：可能仍在跑；不要假设 timeout 就是失败。
4. **kRejected 与 kDropped 区别**：Rejected 是入队失败；Dropped 是入队后被新任务挤掉。
5. **cancellation_token 不阻断 Running 任务**：必须任务自己 poll。

## 设计要点

- TaskHandle 内部 State 用 mutex 保护；wait 用 cv 通知。
- 状态机一旦进入终态不再变化。
- vlink lock order：`MessageLoop::AliveState::mtx → MessageLoop::Impl::mtx → TaskHandle::State::mtx`；callback 在所有 mtx 释放后触发。

## 配图

![TaskHandle timeline](./images/task-handle-timeline.png)

图中展示从 `post_task_handle` 投递 → Queued → Running → 终态的完整时间线，含 cancel / wait / state 的时序关系。

## 参考

- `../cancellation/` — 取消三件套
- `../schedule/` — `Schedule::Config` 调度（无 handle，链式回调）
- `../message_loop_advanced/` — 队列类型对 handle 行为的影响
- `vlink/include/vlink/base/task_handle.h` — TaskHandle 接口
