# logger_advanced — Logger 进阶：自定义 sink、backtrace、Fatal 异常、运行期格式控制

本示例覆盖 vlink Logger 在生产部署中常用的几个进阶能力：

- **自定义控制台 handler**：替换默认控制台输出，把日志路由到自家 ELK / Sentry / 业务监控。
- **Backtrace ring buffer**：低于阈值的日志先攒在环形缓冲，出错时一次性 dump。
- **Fatal 抛异常**：`VLOG_F` / `MLOG_F` 在打印后**抛 `std::runtime_error`**，方便结构化处理。
- **`is_writable` 门控**：避免被过滤掉的高开销日志做无用计算。
- **运行期格式控制**：ANSI 颜色、浮点精度。

读完本示例你能掌握：

- 怎么把 vlink Logger 接入自己的日志后端。
- backtrace 模式的工程价值（"问题发生时回看前 N 条日志"）。
- 编译期级别常量 `kMinimumLevel` / `kDetailLevel` 的用法。
- Fatal 与普通错误日志的差异。

## 背景与适用场景

在生产部署里，往往不能只用 vlink 默认的双 sink：

- 公司有统一日志网关 → 自定义 console handler 把日志重定向到内部协议。
- 调试一个低频偶发 bug → 平时只记 Warn+，但 backtrace 模式让出错时能拿到前 N 条 Debug/Info。
- 高频日志 → 大对象 / 字符串拼接成本高，必须 `is_writable` 检查。
- ANSI 终端不可用 → 关掉颜色避免日志被乱码污染。

vlink Logger 在这几个维度都提供 API。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Logger::register_console_handler` | `static void register_console_handler(Function<void(Level, std::string_view)>&&)` | 把控制台输出替换为回调 |
| `Logger::enable_backtrace` | `static void enable_backtrace(size_t depth)` | 启用 ring buffer，保留最近 depth 条 |
| `Logger::dump_backtrace` | `static void dump_backtrace()` | 把 buffer 内容刷到 sink |
| `Logger::disable_backtrace` | `static void disable_backtrace()` | 关闭 buffer |
| `Logger::is_writable` | `static bool is_writable(Level)` | 当前级别是否会输出（任一 sink） |
| `Logger::set_console_fmt_enable` | `static void set_console_fmt_enable(bool)` | ANSI 颜色开关 |
| `Logger::set_stream_precision` | `static void set_stream_precision(int)` | VLOG_*/SLOG_* 浮点精度 |
| `Logger::kMinimumLevel` | constexpr Level | 编译期最低级别（低于这个的日志被宏直接展开为空） |
| `Logger::kDetailLevel` | constexpr Level | 编译期"详尽日志"级别阈值 |
| `VLOG_F` / `MLOG_F` / `CLOG_F` / `SLOG_F` | 宏 | Fatal；打印后抛 `std::runtime_error` |

## 代码导读

### 1. 自定义 console handler

```cpp
class CustomLogHandler {
 public:
  void install() {
    Logger::register_console_handler(
        [this](Logger::Level level, std::string_view message) { handle(level, message); });
  }

  size_t captured_count() const {
    std::lock_guard lock(mutex_);
    return captured_logs_.size();
  }

 private:
  void handle(Logger::Level level, std::string_view message) {
    {
      std::lock_guard lock(mutex_);
      captured_logs_.emplace_back(message);
    }
    std::cout << "[CustomHandler][" << to_prefix(level) << "] " << message << std::endl;
  }

  mutable std::mutex mutex_;
  std::vector<std::string> captured_logs_;
};

CustomLogHandler handler;
handler.install();
VLOG_I("custom handler installed");
```

注册后，所有原本走控制台的日志改走你的回调。回调在调用线程同步执行 —— 必须快、必须线程安全（vlink 不会加锁）。

### 2. Backtrace ring buffer

```cpp
Logger::enable_backtrace(10);
VLOG_T("bt 1: system initializing");
VLOG_D("bt 2: loading configuration");
VLOG_I("bt 3: configuration loaded");
VLOG_D("bt 4: connecting to database");
VLOG_I("bt 5: database connected");

VLOG_W("about to dump backtrace...");
Logger::dump_backtrace();
Logger::disable_backtrace();
```

`enable_backtrace(10)` 把最近 10 条日志攒在环形缓冲（**不输出到 sink**），直到 `dump_backtrace()` 才一次性输出。
工程上的常见用法：平时只记 Warn+；遇到不可恢复错误前调 `dump_backtrace()` 把前几条 Trace/Debug 都拿出来。

### 3. Fatal 抛异常

```cpp
try {
  VLOG_F("fatal: critical subsystem failure, code=", 0xDEAD);
} catch (const std::runtime_error& e) {
  VLOG_I("caught from VLOG_F: ", e.what());
}

try {
  MLOG_F("fatal: op {} status {}", "connect", -1);
} catch (const std::runtime_error& e) {
  VLOG_I("caught from MLOG_F: ", e.what());
}
```

`VLOG_F` 在打印后抛 `std::runtime_error`。这个设计的好处是把"打日志"和"结构化错误传播"统一在一个点：日志一定会出，调用栈可以靠 catch 处理。

### 4. 编译期级别常量

```cpp
MLOG_I("kMinimumLevel={} kDetailLevel={}", static_cast<int>(Logger::kMinimumLevel),
       static_cast<int>(Logger::kDetailLevel));
VLOG_W("warn includes {file:line} automatically");
VLOG_E("error includes {file:line} automatically");
```

`kMinimumLevel` 是编译期常量，低于它的 `VLOG_*` 宏在预处理阶段被消除（零成本）。release 构建可以把 kMinimumLevel 设为 kInfo 来彻底删除 Trace/Debug 输出代码。

### 5. is_writable 门控

```cpp
if (Logger::is_writable(Logger::kDebug)) {
  std::string expensive = "computed-only-when-debug-is-active";
  VLOG_D("expensive: ", expensive);
}
```

构造 `expensive` 字符串如果只用于 Debug 输出，当 Debug 被过滤时这一段不该跑。`is_writable(kDebug)` 在运行期返回当前 kDebug 是否会输出（任一 sink）。

### 6. 格式控制

```cpp
Logger::set_console_fmt_enable(false);
VLOG_I("ANSI color disabled");
Logger::set_console_fmt_enable(true);

Logger::set_stream_precision(4);
VLOG_I("precision 4: pi=", 3.14159265);   // 输出 3.1416
Logger::set_stream_precision(2);
VLOG_I("precision 2: pi=", 3.14159265);   // 输出 3.14
```

ANSI 颜色用于让控制台输出按级别染色（Debug 蓝、Warn 黄、Error 红）；写文件时建议关闭以免日志聚合工具误读。stream precision 控制 VLOG/SLOG 的浮点输出小数位（不影响 MLOG 的 `{:.4f}` 格式说明）。

## 运行

```bash
./build/output/bin/example_logger_advanced
```

预期输出（节选）：

```
[CustomHandler][INFO ] custom handler installed
[CustomHandler][DEBUG] captured so far: 1
[CustomHandler][WARN ] about to dump backtrace...
... (10 backtrace entries dumped here)
[CustomHandler][INFO ] caught from VLOG_F: fatal: critical subsystem failure, code=...
[CustomHandler][INFO ] caught from MLOG_F: fatal: op connect status -1
[CustomHandler][INFO ] kMinimumLevel=0 kDetailLevel=1
[CustomHandler][INFO ] precision 4: pi=3.1416
[CustomHandler][INFO ] precision 2: pi=3.14
total captured by custom handler: ...
Logger advanced example finished.
```

## 常见陷阱

1. **register_console_handler 阻塞**：handler 同步执行；慢回调会阻塞业务线程。建议把数据扔到 lock-free queue 异步处理。
2. **backtrace 没 dump 就 disable**：buffer 内容会被丢弃；`disable_backtrace` 之前一定 `dump_backtrace`。
3. **Fatal 不抛**：在某些编译模式下（如禁用异常）Fatal 行为变为 `std::abort()`；catch 是否能成功取决于编译选项。
4. **is_writable 仍然格式化字符串**：是的 —— 检查只是绕过宏调用本身；如果你 `VLOG_D("...", expensive_call())` 仍会先 expensive_call。所以正确做法是 `if (Logger::is_writable(kDebug)) { ... }` 包外面。
5. **`set_stream_precision` 全局生效**：影响所有 VLOG_* 调用，不只是当前线程。

## 设计要点

- backtrace 是 thread-local ring buffer（vlink 默认实现）；多线程各自独立。
- `kMinimumLevel` 通常通过 CMake 选项 `VLINK_LOG_MIN_LEVEL` 控制；release 构建编译期消除冗余日志。
- ANSI 颜色用 ESC 序列；终端不支持时显示为乱码 —— 用 `set_console_fmt_enable(false)` 关闭。

## 配图

无专属配图。

## 参考

- `../logger_basic/` — 初始化、四种风格、级别控制
- `vlink/include/vlink/base/logger.h` — Logger 接口
- `vlink/include/vlink/base/logger_plugin_interface.h` — 自定义 sink 接口
