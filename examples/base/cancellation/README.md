# cancellation — vlink 协作式取消三件套

vlink 协作式取消机制由四个组件组成：

- `vlink::CancellationSource`：取消源，持有者；`request_cancel()` 翻转状态。
- `vlink::CancellationToken`：可观察句柄；可拷贝、可跨线程共享。
- `vlink::CancellationRegistration`：注册的回调句柄，可 `.reset()` 取消订阅。
- `vlink::Exception::OperationCancelled`：取消异常，用于 RAII 退栈。

"协作式"意味着：vlink 不强制中断线程，而是提供"被取消时应该如何反应"的统一约定。被取消方负责定期 poll 或 throw 来响应。

读完本示例你能掌握：

- 三种典型协作模式（poll / throw / callback）。
- source / token / registration 三者的生命周期关系。
- 与 `TaskHandle::PostTaskOptions::cancellation_token` 的集成。

## 背景与适用场景

适用：

- 长时间任务的中途取消（用户按取消按钮、连接断开、超时）。
- 同一组任务的级联取消（一组 worker 共享一个 token）。
- 与 IO 操作的协作（取消时 wake_up 阻塞的 epoll）。

不适合：

- 强制 kill 线程（C++ 没有标准方式做到，vlink 也不提供）。
- 业务自己用 atomic_bool 就能搞定的简单中断信号（小项目用 atomic_bool 更轻）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `CancellationSource()` | 构造 | 默认未取消 |
| `CancellationSource::request_cancel` | `bool request_cancel()` | 翻转状态；首次 true，后续 false |
| `CancellationSource::token` | `CancellationToken token() const` | 取观察句柄 |
| `CancellationToken::is_cancellation_requested` | `bool` | 轮询 |
| `CancellationToken::throw_if_cancellation_requested` | `void` | 已取消则抛 `OperationCancelled` |
| `CancellationToken::register_callback` | `CancellationRegistration register_callback(Function<void()>&&)` | 注册回调，单次 |
| `CancellationRegistration::reset` | `void reset()` | 触发前取消订阅 |
| `Exception::OperationCancelled` | 类 | 派生自 `std::exception` |

## 代码导读

### 1. Poll 模式

```cpp
CancellationSource source;
auto token = source.token();
std::thread worker([token]() {
  int i = 0;
  while (!token.is_cancellation_requested()) {
    std::this_thread::sleep_for(20ms);
    ++i;
  }
  VLOG_I("  poll: stopped at iter ", i);
});

std::this_thread::sleep_for(100ms);
source.request_cancel();
worker.join();
```

适合 tight loop、栈浅、能频繁 poll 的场景。

### 2. Throw 模式

```cpp
auto deep = [&token]() {
  std::this_thread::sleep_for(50ms);
  token.throw_if_cancellation_requested();   // 抛 OperationCancelled
  // ... 后续逻辑不会执行 ...
};

try {
  deep();
} catch (const vlink::Exception::OperationCancelled&) {
  VLOG_I("  throw: caught at top");
}
```

适合 RAII 资源清理 / 深栈调用：抛异常让上层 try/catch 统一处理。

### 3. Callback 模式

```cpp
auto reg = token.register_callback([]() {
  VLOG_I("  callback fired (wake_io / cleanup hook)");
});

source.request_cancel();   // 立即触发 reg 的回调
```

适合"取消时唤醒阻塞 IO"的场景。

### 4. 注册时序

```cpp
source.request_cancel();
auto reg = token.register_callback([]() { VLOG_I("  fired sync on caller thread"); });
// 已取消的 token register 会立即同步触发回调
```

注册前先取消：回调同步在 register 调用方线程触发。

### 5. 提前 reset

```cpp
auto reg = token.register_callback([]() { VLOG_I("  this won't run"); });
reg.reset();             // 取消订阅
source.request_cancel(); // 不会触发上面那个回调
```

### 6. 多 token 扇出 + 父子级联

```cpp
CancellationSource root;
auto root_token = root.token();

CancellationSource middle;
auto middle_reg = root_token.register_callback([&middle]() { middle.request_cancel(); });

CancellationSource leaf;
auto leaf_reg = middle.token().register_callback([&leaf]() { leaf.request_cancel(); });

root.request_cancel();   // 触发 middle 取消，再触发 leaf 取消
```

7. **TaskHandle 集成**

```cpp
PostTaskOptions opts;
opts.cancellation_token = source.token();
loop.post_task_handle([token = source.token()]() {
  while (!token.is_cancellation_requested()) {
    // ...
  }
}, opts);

source.request_cancel();
```

`PostTaskOptions::cancellation_token` 让 TaskHandle 直接绑定取消语义。详见 `../task_handle/`。

## 运行

```bash
./build/output/bin/example_cancellation
```

预期输出（节选）：

```
poll: stopped at iter ...
throw: caught at top
callback fired (wake_io / cleanup hook)
fired sync on caller thread
reset before cancel: not fired
... fan-out / cascade / sibling / exception swallowed ...
default token: never cancelled
PostTaskOptions::cancellation_token: ok
```

## 常见陷阱

1. **request_cancel 不是 sticky**：首次调用 true、后续 false；适合配合 if 判断。
2. **callback 抛异常**：vlink 吞掉并打日志；后续 callback 继续执行。不要假设异常会传播。
3. **reset 在触发之后**：no-op；不要假设能"取消已经发生的事件"。
4. **CancellationRegistration 析构**：默认会调 `.reset()`；保留 reg 句柄 till 取消触发。
5. **跨进程不可用**：取消机制是进程内的；跨进程取消请用业务级 RPC。

## 设计要点

- source 与 token 通过 control block 共享状态；token 拷贝零开销。
- mutex 在触发回调前释放；callback 内可安全注册新 callback / 触发其它 source。
- `OperationCancelled` 派生自 `std::exception`；可被通用 try/catch 捕获。

## 配图

![Three modes](./images/cancellation-usage-modes.png)

图中对比三种使用模式（poll / throw / callback）的代码骨架和适用场景。

## 参考

- `../task_handle/` — 带 cancellation_token 的 TaskHandle
- `../deadline_timer/` — 与"超时"互补的"取消"
- `vlink/include/vlink/base/cancellation.h` — 完整接口
- `vlink/include/vlink/base/exception.h` — `OperationCancelled`
