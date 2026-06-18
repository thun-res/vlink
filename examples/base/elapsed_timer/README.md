# elapsed_timer — `vlink::ElapsedTimer` 高精度耗时测量

`vlink::ElapsedTimer` 是 vlink 内置的耗时测量工具，覆盖：

- 两种时钟源 —— **`kCpuTimestamp`**（壁钟）和 **`kCpuActiveTime`**（仅 CPU 实际执行时间，排除阻塞 / 睡眠）。
- 三种精度 —— `kMilli` / `kMicro` / `kNano`。
- 起停 / restart 圈计 / 静态时间戳查询。

注意：`ElapsedTimer` 不触发回调，**只用于测量**。如果要"N 秒后触发"，用 `vlink::Timer`；如果要"判断当前时间是否到截止"，用 `vlink::DeadlineTimer`。

读完本示例你能掌握：

- 两种 method 各自的适用场合。
- 三种精度的取值范围与开销。
- `restart()` 圈计与 stop 的语义差异。
- 几个静态时间戳函数何时直接拿来用。

## 背景与适用场景

适用场景：

- 单段代码耗时测量（profiling、benchmark）。
- 监控埋点：业务函数执行时长上报到 metric 系统。
- 圈计：多段耗时 lap1 / lap2 / lap3。
- 拿当前时间戳：日志、消息时间戳、TTL 判断。

不适合：

- 跨进程时间同步（用 PTP / NTP / chrony）。
- 高精度（< 100ns）测量：调用 `get()` 本身有几十纳秒开销。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `ElapsedTimer()` | 默认 | `kCpuTimestamp` + `kMilli` |
| `ElapsedTimer(Method, Accuracy)` | 构造 | 显式选时钟源 + 精度 |
| `ElapsedTimer::Method` | enum | `kCpuTimestamp` / `kCpuActiveTime` |
| `ElapsedTimer::Accuracy` | enum | `kMilli` / `kMicro` / `kNano` |
| `start` / `stop` | `void` | 起停 |
| `is_active` | `bool is_active() const` | 是否在跑 |
| `get` | `int64_t get() const` | 当前已经累计的时间；未 start 返回 -1 |
| `restart` | `int64_t restart()` | 原子读出 + 重置 |
| `get_method` / `get_accuracy` | const | 配置查询 |
| `ElapsedTimer::get_sys_timestamp` | `static int64_t (Accuracy)` | 系统钟（受 NTP 影响） |
| `ElapsedTimer::get_cpu_timestamp` | `static int64_t (Accuracy)` | CPU 钟（monotonic） |
| `ElapsedTimer::get_cpu_active_time` | `static int64_t (Accuracy)` | 当前进程 CPU 累计时间 |

## 代码导读

### 1. 默认 ms 测量

```cpp
ElapsedTimer timer;
timer.start();
std::this_thread::sleep_for(100ms);
MLOG_I("  elapsed={}ms (expect ~100)", timer.get());
timer.stop();
```

默认 `kCpuTimestamp + kMilli`。`get()` 在 start 之前返回 -1。

### 2. Micro 和 Nano 精度

```cpp
ElapsedTimer timer(ElapsedTimer::kCpuTimestamp, ElapsedTimer::kMicro);
timer.start();
std::this_thread::sleep_for(50ms);
MLOG_I("  elapsed={}us (expect ~50000)", timer.get());

ElapsedTimer ns_timer(ElapsedTimer::kCpuTimestamp, ElapsedTimer::kNano);
ns_timer.start();
// ... CPU work ...
MLOG_I("  CPU work elapsed={}ns", ns_timer.get());
```

精度越高 int64 范围越短：kMilli ~292 兆年，kMicro ~292 千年，kNano ~292 年；实际工程都够用。

### 3. CPU active time

```cpp
ElapsedTimer timer(ElapsedTimer::kCpuActiveTime, ElapsedTimer::kMicro);
timer.start();

volatile double sum = 0;
for (int i = 0; i < 1000000; ++i) {
  sum += std::sqrt(static_cast<double>(i));
}

MLOG_I("  CPU active={}us", timer.get());
```

`kCpuActiveTime` 排除被阻塞 / 睡眠的时间，只算 CPU 真的在跑的时间。用于 profiling CPU 密集型函数；典型场景是判断是不是 IO bound（壁钟和 CPU active 差很多）。

### 4. restart 圈计

```cpp
ElapsedTimer timer(ElapsedTimer::kCpuTimestamp, ElapsedTimer::kMilli);
timer.start();
std::this_thread::sleep_for(100ms);
MLOG_I("  lap1={}ms", timer.restart());   // 读出 ~100，重置
std::this_thread::sleep_for(50ms);
MLOG_I("  lap2={}ms", timer.restart());   // 读出 ~50，重置
timer.stop();
```

`restart()` 原子读+重置，避免"读完再清"的竞态。

### 5. 静态时间戳工具

```cpp
MLOG_I("  sys ms     = {}", ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli));
MLOG_I("  cpu ms     = {}", ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli));
MLOG_I("  cpu active = {}us", ElapsedTimer::get_cpu_active_time(ElapsedTimer::kMicro));
MLOG_I("  sys ns     = {}", ElapsedTimer::get_sys_timestamp(ElapsedTimer::kNano));
```

不需要 start/stop 就能拿当前时间戳。

- `get_sys_timestamp`：系统钟（real-time clock）；受 NTP / 用户手动改时间影响。
- `get_cpu_timestamp`：单调钟（monotonic）；从某个固定点累计，不会倒退。
- `get_cpu_active_time`：当前进程 user+kernel 累计 CPU 时间。

## 运行

```bash
./build/output/bin/example_elapsed_timer
```

预期输出（节选）：

```
=== Default (ms) ===
  elapsed=100ms (expect ~100)
  is_active after stop=0
=== Microsecond ===
  elapsed=50012us (expect ~50000)
=== Nanosecond ===
  CPU work elapsed=1234567ns
=== CPU active time ===
  CPU active=23456us
=== restart ===
  lap1=100ms
  lap2=50ms
=== Static utilities ===
  sys ms     = 1716266400000
  cpu ms     = 12345678
  cpu active = 567us
  sys ns     = 1716266400123456789
=== Config query ===
  method=1 (0=CpuTimestamp 1=CpuActiveTime)
  accuracy=2 (0=Milli 1=Micro 2=Nano)
  is_active before start=0 get=-1
```

## 常见陷阱

1. **未 start 直接 get**：返回 -1。
2. **跨线程访问同一 ElapsedTimer**：vlink 不保证 thread-safety；每个线程持有自己的实例。
3. **`kCpuActiveTime` 在 macOS 上不可用**：vlink 在 macOS 用 `mach_thread_info`，行为略不同；详见头文件。
4. **`get_sys_timestamp` 用作消息时间戳**：跨机时不可靠（NTP 可能跳变）；持久化场景请用 `get_cpu_timestamp`。
5. **stop 后再 get**：返回 stop 时的总耗时，不会继续累计；再 start 会从 0 开始。

## 设计要点

- `kCpuTimestamp` 内部用 `clock_gettime(CLOCK_MONOTONIC_RAW)`（Linux）或 `steady_clock`（标准 C++ fallback）。
- `kCpuActiveTime` 在 Linux 上用 `getrusage(RUSAGE_THREAD)`；macOS 上不同。
- `get()` 自身开销几十纳秒到一百纳秒；纳秒级测量的边际误差不可忽略。

## 配图

无专属配图。

## 参考

- `../deadline_timer/` — 绝对截止时间检测
- `../timer/` — 周期触发回调
- `vlink/include/vlink/base/elapsed_timer.h` — ElapsedTimer 接口
- `vlink/include/vlink/base/cached_timestamp.h` — 缓存时间戳（高频读取场景）
