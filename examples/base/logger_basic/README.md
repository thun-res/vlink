# 📝 logger_basic — vlink Logger 入门

vlink Logger 是线程安全、双 sink（控制台 + 文件）的日志库，提供四种调用风格，控制台与文件级别可分别控制、运行期热切。vlink 自身所有模块的日志都走它。

## 🧩 核心 API

| API | 用途 |
|-----|------|
| `Logger::init(app_name, log_path = "")` | 初始化；空路径会从 `VLINK_LOG_DIR` 或临时目录选择默认日志目录，是否写文件由 file level 控制 |
| `Logger::set_console_level(Level)` | 设置控制台输出级别 |
| `Logger::set_file_level(Level)` | 设置文件输出级别（与控制台独立） |
| `Logger::flush()` | 强制刷盘；退出/abort 前调用避免丢日志 |
| `VLOG_T/D/I/W/E/F` | variadic 参数拼接：`VLOG_I("x=", x)` |
| `VLOG_{T,D,I,W,E}_EVERY_MS` | 调用点级周期限频；首次立即输出，抑制时不求值日志参数 |
| `MLOG_T/D/I/W/E/F` | format：`{}` 占位符 |
| `CLOG_T/D/I/W/E/F` | printf：`%d %s` 风格 |
| `SLOG_T/D/I/W/E/F` | RAII iostream：链式 `<<` 到分号为止 |

级别由低到高：`kTrace / kDebug / kInfo / kWarn / kError / kFatal`，只有不低于所设级别的消息才输出到对应 sink。

## 🚀 最小可运行片段

```cpp
vlink::Logger::init("demo", "/tmp/vlink_logger_basic.log");
vlink::Logger::set_console_level(vlink::Logger::kTrace);  // 控制台全量
vlink::Logger::set_file_level(vlink::Logger::kInfo);      // 文件只记 Info+

VLOG_I("stream [I]: started, usage=", 91, "%");           // 流式
MLOG_I("format [I]: host={}, port={}", "192.168.1.1", 8080);  // {} 占位
CLOG_I("c [I]: PID=%d started", 12345);                   // printf
SLOG_I << "raii [I]: x=" << 1.5 << " y=" << 2.5;          // iostream

vlink::Logger::set_console_level(vlink::Logger::kWarn);   // 运行期热切
VLOG_I("info 被控制台过滤");                                // 不再上控制台
VLOG_W("warn 仍然显示");

vlink::Logger::flush();                                   // 退出前刷盘
```

四种宏只是写法不同，走同一后端，按调用处可读性自由选用，同进程可混用。

## 🎯 何时用

任何 C++ 工程需要分级日志时都可用。流式/format/printf/RAII 四风格按团队口味选；需要控制台安静而文件保留审计日志时，用 `set_console_level` 与 `set_file_level` 分别控制。

## 🔗 参考

- `doc/08-base-library.md` — base 库（含 Logger 自定义 handler、backtrace、Fatal 行为）
- `../README.md` — base 示例总览与阅读顺序
- `include/vlink/base/logger.h` — Logger 接口
