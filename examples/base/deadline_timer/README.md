# deadline_timer — `vlink::DeadlineTimer` 绝对截止时间检测

`vlink::DeadlineTimer` 解决"判断当前是否到截止时间"这一类问题。它内部只存一个 64-bit 绝对时间戳，所有读取都是 atomic 无锁，适合在热循环里频繁查询。

读完本示例你能掌握：

- 相对 vs 绝对 deadline 的两种构造方式。
- `has_expired()` / `remaining_time()` / `is_valid()` 的语义。
- 三种典型工程用法：超时检测、重试预算、批处理超时。

## 背景与适用场景

适用场景：

- IO 超时：发请求 + 在 N ms 内轮询响应。
- 重试预算：总时间不超过 X ms，每次重试间隔 Y ms。
- 批处理预算：处理一批任务，超过预算就停。
- 与 `Cancellation` 配合做"协作式截止"。

与 `Timer` 的区别：

- `Timer` 是**主动触发**回调；DeadlineTimer 是**被动检查**是否到点。
- `Timer` 必须 attach loop；DeadlineTimer 不依赖 loop，可在任意线程使用。
- `Timer` 适合"定期做事"；DeadlineTimer 适合"做事直到超时"。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `DeadlineTimer()` | 默认 | 无效 deadline，`is_valid()==false` |
| `DeadlineTimer(rel, Accuracy)` | 构造 | 相对当前的 deadline，Accuracy 默认 `kMilli` |
| `set_deadline` | `void set_deadline(int64_t rel)` | 重新 arm 相对 deadline |
| `set_deadline_abs` | `void set_deadline_abs(int64_t abs_ts)` | arm 绝对时间戳 |
| `has_expired` | `bool has_expired() const` | 是否已过 deadline |
| `remaining_time` | `int64_t remaining_time() const` | 剩余时间（可能为负） |
| `deadline` | `int64_t deadline() const` | 绝对截止时间戳 |
| `is_valid` | `bool is_valid() const` | deadline 是否被 arm 过 |
| `reset` | `void reset()` | 重置为无效 |
| `get_accuracy` | `ElapsedTimer::Accuracy get_accuracy() const` | 构造时锁定 |

## 代码导读

### 1. 默认构造（无效）

```cpp
DeadlineTimer timer;
VLOG_I("  is_valid=", timer.is_valid(), " has_expired=", timer.has_expired(), " deadline=", timer.deadline());
// is_valid=0  has_expired=0（未 arm 视为永不过期）  deadline=0
```

未 arm 的 timer `has_expired()` 返回 false —— 因为 "永远不过期" 是合理默认。

### 2. 相对 deadline

```cpp
DeadlineTimer timer(200, ElapsedTimer::kMilli);  // 200ms 后
VLOG_I("  remaining=", timer.remaining_time(), "ms has_expired=", timer.has_expired());

std::this_thread::sleep_for(50ms);
VLOG_I("  after 50ms: remaining=", timer.remaining_time(), "ms expired=", timer.has_expired());

std::this_thread::sleep_for(200ms);
VLOG_I("  after 250ms total: remaining=", timer.remaining_time(), "ms expired=", timer.has_expired());
// remaining 变负，expired=1
```

### 3. 绝对 deadline

```cpp
DeadlineTimer timer;
uint64_t now_ms = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);
timer.set_deadline_abs(now_ms + 150);
```

适合"截止到某个具体时刻"场景：例如"15:30:00 之前必须完成"。

### 4. reset + re-arm

```cpp
DeadlineTimer timer(100);
timer.reset();                       // 变回无效
timer.set_deadline(300);             // 重新 arm 300ms
```

### 5. 不同精度

```cpp
DeadlineTimer timer(50000, ElapsedTimer::kMicro);    // 50000us = 50ms
VLOG_I("  accuracy=", static_cast<int>(timer.get_accuracy()), " remaining=", timer.remaining_time(), "us");
```

构造时锁定精度，不能后期改。

### 6. 实战：超时检测

```cpp
DeadlineTimer deadline(100);
int iter = 0;
bool received = false;
while (!deadline.has_expired()) {
  ++iter;

  if (iter == 5) {
    received = true;
    break;
  }
  std::this_thread::sleep_for(10ms);
}

if (received) {
  VLOG_I("  got response after ", iter, " iterations, remaining=", deadline.remaining_time(), "ms");
} else {
  VLOG_I("  timeout after ", iter, " iterations");
}
```

经典"轮询直到超时"模式。每次循环判断是否已到 deadline。

### 7. 实战：重试预算

```cpp
DeadlineTimer overall(200);          // 总预算 200ms
int attempt = 0;
while (!overall.has_expired()) {
  ++attempt;
  if (attempt >= 4) {
    VLOG_I("  success on attempt ", attempt);
    break;
  }
  VLOG_I("  attempt ", attempt, " failed, remaining=", overall.remaining_time(), "ms");
  std::this_thread::sleep_for(30ms);
}
```

总预算模型：不管重试几次，总时间不能超 200ms。

## 运行

```bash
./build/output/bin/example_deadline_timer
```

预期输出（节选）：

```
--- Default construction ---
  is_valid=0 has_expired=0 deadline=0
--- Relative deadline 200ms ---
  remaining=200ms has_expired=0
  after 50ms: remaining=150ms expired=0
  after 250ms total: remaining=-50ms expired=1
--- Absolute deadline ---
  now=... deadline_abs=... remaining=150ms
  after 200ms expired=1
--- Reset + reuse ---
  initial valid=1 remaining=100ms
  after reset valid=0
  after set_deadline(300) valid=1 remaining=300ms
--- Practical: timeout detection ---
  got response after 5 iterations, remaining=50ms
--- Practical: retry within 200ms ---
  attempt 1 failed, remaining=170ms
  attempt 2 failed, remaining=140ms
  attempt 3 failed, remaining=110ms
  success on attempt 4
```

## 常见陷阱

1. **默认构造的 `has_expired() == false`**：未 arm 视为永远不过期；调用方要先 `is_valid()` 检查。
2. **`remaining_time()` 可为负**：已过期时返回负值。
3. **多线程并发 set_deadline**：值更新是 atomic 但语义按"最后写赢"；逻辑上要避免多个 setter。
4. **跨平台时钟漂移**：DeadlineTimer 用 monotonic 时钟，单进程内不会跳变；跨机器分别独立。
5. **微秒 timer 期望毫秒精度**：accuracy 控制内部 timestamp 单位；不影响 `clock_gettime` 系统钟精度。

## 设计要点

- 内部用 std::atomic<int64_t> 存绝对时间戳；读取 lock-free。
- 与 `ElapsedTimer::get_cpu_timestamp` 共享时钟源 `CLOCK_MONOTONIC_RAW`。
- 拷贝/赋值就是拷贝那个 atomic 值；线程安全。
- 没有"触发回调"语义；要回调请用 Timer。

## 配图

无专属配图。

## 参考

- `../elapsed_timer/` — 测耗时
- `../timer/` — 周期触发回调
- `../cancellation/` — 协作取消（与 deadline 互补）
- `vlink/include/vlink/base/deadline_timer.h` — DeadlineTimer 接口
