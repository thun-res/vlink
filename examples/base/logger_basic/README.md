# logger_basic — vlink Logger 入门：四种调用风格 + 双 sink 级别控制

vlink Logger 是线程安全、双 sink（控制台 + 文件）、零分配的日志库。它给 C++ 项目提供四种调用风格，覆盖不同口味的工程组：流式 (`VLOG_*`)、format (`MLOG_*`)、printf (`CLOG_*`)、RAII 流 (`SLOG_*`)。同一进程可以混用所有风格，运行期可分别调控制台和文件的日志级别。

读完本示例你能掌握：

- 四种宏家族 `VLOG_*` / `MLOG_*` / `CLOG_*` / `SLOG_*` 各自的用法与适用场合。
- `Logger::init` 双 sink 初始化。
- 运行期通过 `set_console_level` / `set_file_level` 控制日志输出。
- 6 个级别 (`kTrace`、`kDebug`、`kInfo`、`kWarn`、`kError`、`kFatal`) 的使用约定。

## 背景与适用场景

vlink Logger 的设计目标：

- **零开销**：调用宏在级别被过滤时只做一次原子读，不构造字符串。
- **线程安全**：内部无锁队列 + 单一后台 flushing 线程。
- **双 sink**：控制台输出与文件输出独立级别控制 —— 例如开发期文件记 Info+，控制台记 Trace+。
- **四种风格**：让团队自由选择，不强制统一。

vlink 自己所有的代码（包括传输层、序列化、扩展模块）都通过 Logger 输出日志。生产部署时通常把控制台关到 Warn 级别、文件保留 Info 级别。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Logger::init` | `static void init(const std::string& app_name, const std::string& file_path = "")` | 初始化；file_path 为空则不写文件 |
| `Logger::set_console_level` | `static void set_console_level(Level)` | 控制台级别（kTrace ~ kFatal） |
| `Logger::set_file_level` | `static void set_file_level(Level)` | 文件级别 |
| `Logger::flush` | `static void flush()` | 强制把缓冲写到 sink |
| `Logger::Level` | enum | kTrace / kDebug / kInfo / kWarn / kError / kFatal |
| `VLOG_T/D/I/W/E/F` | 宏 | 流式（operator<<），零分配拼接 |
| `MLOG_T/D/I/W/E/F` | 宏 | `{}` 占位符，类似 fmt |
| `CLOG_T/D/I/W/E/F` | 宏 | printf 风格 `%d` `%s` |
| `SLOG_T/D/I/W/E/F` | 宏 | RAII iostream 风格，dtor flush |

## 代码导读

### 1. 初始化双 sink

```cpp
Logger::init("logger_basic_demo", "/tmp/vlink_logger_basic.log");
Logger::set_console_level(Logger::kTrace);
Logger::set_file_level(Logger::kInfo);
```

`init` 第一个参数是 app 名（写到每条日志的前缀），第二个是文件路径（可省略，省时只输出到控制台）。

控制台级别 Trace = 全部打印；文件级别 Info = Trace/Debug 不写文件。

### 2. 四种风格示例

```cpp
// 流式：用 << 拼接，零分配
VLOG_I("stream [I]: application started");
VLOG_W("stream [W]: disk usage=", 91, "%");

// format：{} 占位
MLOG_I("format [I]: connected to host={}, port={}", "192.168.1.1", 8080);
MLOG_W("format [W]: retry {}/{}", 3, 5);

// printf：移植旧代码方便
CLOG_I("c [I]: PID=%d started", 12345);
CLOG_E("c [E]: errno=%d (%s)", 2, "No such file or directory");

// RAII iostream：链式 << 直到分号
SLOG_I << "raii [I]: init complete";
SLOG_D << "raii [D]: x=" << 1.5 << " y=" << 2.5;
```

### 3. 运行期调整级别

```cpp
VLOG_I("--- raising console level to kWarn ---");
Logger::set_console_level(Logger::kWarn);
VLOG_D("debug suppressed on console");
VLOG_I("info suppressed on console");
VLOG_W("warn still shown on console");
VLOG_E("error still shown on console");

Logger::set_console_level(Logger::kTrace);

Logger::set_file_level(Logger::kError);
VLOG_I("info -> console only, not file");
VLOG_E("error -> both console and file");
```

`set_console_level` 和 `set_file_level` 完全独立。运行期可以根据负载、SIGUSR1 等热切。

### 4. flush

```cpp
Logger::flush();
```

`flush()` 同步把所有缓冲刷盘；通常在 `quit()` / `abort()` 之前必调，避免日志丢失。

## 运行

```bash
./build/output/bin/example_logger_basic
# 日志文件: /tmp/vlink_logger_basic.log
```

预期输出（节选）：

```
[I][logger_basic_demo] stream [I]: application started
[W][logger_basic_demo] stream [W]: disk usage=91%
[E][logger_basic_demo] stream [E]: failed to open config
[T][logger_basic_demo] format [T]: value=42, label=beta
[I][logger_basic_demo] format [I]: connected to host=192.168.1.1, port=8080
[I][logger_basic_demo] c [I]: PID=12345 started
[I][logger_basic_demo] raii [I]: init complete
--- raising console level to kWarn ---
[W][logger_basic_demo] warn still shown on console
[E][logger_basic_demo] error still shown on console
[I][logger_basic_demo] info -> console only, not file
Logger basic example finished.
```

## 常见陷阱

1. **未 `init` 就用宏**：默认 sink 配置只输出到控制台；级别 Info。不一定符合需求。
2. **fmt `{}` 占位符数量与参数不匹配**：MLOG_* 用 std::format / fmt 风格；不匹配会编译失败或运行时格式化异常。
3. **CLOG 用 std::string**：CLOG 是 printf 风格，传 std::string 必须 `.c_str()`。
4. **SLOG_* 没有 flush** 直到分号：长链 `SLOG_I << a << b << ...;` 在分号前不刷出，崩溃时这条丢。
5. **跨进程不共享 sink**：每个进程独立 init，独立写文件。多进程要同一份日志请走集中式 Logger 后端或自己写文件路径区分。

## 设计要点

- VLOG_ 系列基于 vlink 内置 `FastStream`，零分配（用 thread_local buffer）。
- 后台 flushing 线程异步写文件 sink；不阻塞业务。
- 6 级别中 `kFatal` 输出后会调 `std::abort()`（典型用于不可恢复错误）。

## 配图

无专属配图。

## 参考

- `../logger_advanced/` — 自定义后端、backtrace、Fatal 行为、编译期级别过滤
- `vlink/include/vlink/base/logger.h` — Logger 接口
- `vlink/include/vlink/base/logger_plugin_interface.h` — 自定义 sink 接口
