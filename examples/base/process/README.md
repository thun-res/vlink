# process — `vlink::Process` 跨平台子进程管理

`vlink::Process` 提供跨平台（Linux / macOS / Windows）的子进程启动、I/O 管道、信号、detached 模式等能力，API 风格借鉴 Qt 的 `QProcess`。

读完本示例你能掌握：

- `Process::execute` 同步执行的最简用法。
- 异步启动 + 输出读取的完整流程。
- 三种 ChannelMode（Separate / Forwarded / Merged）的差异。
- 向子进程 stdin 写、按行读 stdout、注册 finished 回调。
- `terminate()` / `kill()` 的区别和升级流程。
- `start_detached` 启动游离子进程。

## 背景与适用场景

适用：

- 业务里需要调外部程序：脚本、CLI 工具、ffmpeg、git 等。
- 启动并管理 daemon 进程（监控、自动重启）。
- 测试场景里 spawn 一个 helper 程序。

不适合：

- C++ 内部并发（用 std::thread / ThreadPool）。
- IPC（用 vlink Publisher/Subscriber 等通信原语）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Process::execute` | `static int execute(const std::string& prog, const std::vector<std::string>& args = {}, uint32_t timeout_ms = 0)` | 同步运行；返回 exit code |
| `Process::start_detached` | `static bool start_detached(const std::string& prog, const std::vector<std::string>& args)` | 游离子进程 |
| `Process` | 默认构造 | 异步管理实例 |
| `set_process_mode` | `void set_process_mode(ChannelMode)` | Separate / Forwarded / Merged |
| `start` | `bool start(const std::string& prog, const std::vector<std::string>& args = {})` | 启动 |
| `wait_for_started` / `wait_for_finished` | `bool (ms)` | 同步等 |
| `write` | `bool write(const std::string&)` | 写 stdin |
| `close_write_channel` | `void` | 关闭 stdin pipe（让 child 看到 EOF） |
| `read_all_output` / `read_all_error` | `bool (std::string&)` | 一次性读全部 |
| `can_read_line_stdout` / `read_line_stdout` | const / `(std::string&)` | 按行读 |
| `terminate` / `kill` | `void` | SIGTERM / SIGKILL |
| `is_running` / `get_exit_code` / `get_pid` | const | 状态查询 |
| `register_finished_callback` | `void register_finished_callback(Function<void(int, ExitStatus)>&&)` | finished 回调 |

## 代码导读

### 1. 同步 execute

```cpp
int rc = Process::execute("/bin/echo", {"hello"});
MLOG_I("  exit code={}", rc);

int rc2 = Process::execute("/bin/ls", {"-la"});
int rc_bad = Process::execute("/no/such/binary");   // 非 0
```

### 2. 异步启动 + 读 stdout

```cpp
Process proc;
proc.set_process_mode(Process::kSeparateMode);
proc.start("/bin/sh", {"-c", "echo line1; echo line2"});
proc.wait_for_finished(1000);

std::string out;
proc.read_all_output(out);
VLOG_I("  stdout:\n", out);
```

### 3. stdout / stderr 分离

```cpp
proc.start("/bin/sh", {"-c", "echo to_out; echo to_err 1>&2"});
proc.wait_for_finished(1000);
std::string out, err;
proc.read_all_output(out);
proc.read_all_error(err);
```

### 4. 写 stdin

```cpp
Process proc;
proc.start("/bin/cat", {});
proc.write("hello\n");
proc.close_write_channel();
proc.wait_for_finished(1000);
std::string out;
proc.read_all_output(out);
```

`close_write_channel()` 让 cat 看到 EOF 然后退出。

### 5. 按行流式读

```cpp
Process proc;
proc.start("/bin/sh", {"-c", "for i in 1 2 3; do echo line$i; sleep 0.05; done"});
while (proc.is_running() || proc.can_read_line_stdout()) {
  std::string line;
  if (proc.read_line_stdout(line)) {
    VLOG_I("  line: ", line);
  }
}
proc.wait_for_finished(1000);
```

### 6. terminate → kill 升级

```cpp
Process proc;
proc.set_process_mode(Process::kForwardedMode);
proc.start("/bin/sleep", {"10"});
std::this_thread::sleep_for(50ms);
proc.terminate();
if (!proc.wait_for_finished(1000)) {
  proc.kill();
}
```

`terminate()` 发 SIGTERM；如果进程不响应再 `kill()` 发 SIGKILL。

### 7. finished 回调 + detached

```cpp
Process proc;
proc.register_finished_callback([](int code, Process::ExitStatus status) {
  MLOG_I("  finished: code={} normal={}", code, status == Process::kNormalExit);
});
proc.start("/bin/true");
proc.wait_for_finished(1000);

Process::start_detached("/bin/sh", {"-c", "echo detached &"});
```

## 运行

```bash
./build/output/bin/example_process
```

预期输出（节选）：

```
=== execute ===
hello
  exit code=0
total ... drwxr-xr-x ...
  exit code=0
  exit code=2     (non-zero for missing binary)
=== async start ===
  stdout:
line1
line2
=== stderr separate ===
  out: to_out
  err: to_err
=== write to stdin ===
  out: hello
=== line stream ===
  line: line1
  line: line2
  line: line3
=== terminate / kill ===
  terminated
=== detached + finished cb ===
  finished: code=0 normal=1
  detached started
```

## 常见陷阱

1. **没 wait_for_started 就 read**：read 可能返回空；等启动完成。
2. **start_detached 后 read**：detached 子进程没有 pipe；read 无效。
3. **跨平台路径**：示例硬编码 `/bin/sh` 等 POSIX 路径；Windows 上得改 `cmd.exe`。
4. **`get_exit_code` 在仍运行时**：返回未定义；先 `is_running()` 判断。
5. **写 stdin 后不 close_write_channel**：child 可能一直等输入；显式关闭 pipe。

## 设计要点

- Linux 用 fork+exec；Windows 用 CreateProcess；macOS 用 posix_spawn。
- pipe 管道异步读取靠 select/poll（Linux）/ epoll/iocp。
- finished 回调在 vlink 内部线程上触发；不要做长任务。

## 配图

无专属配图。

## 参考

- `../utils/` — `get_pid`、`get_app_path` 等
- `vlink/include/vlink/base/process.h` — Process 接口
