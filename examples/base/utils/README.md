# utils — `vlink::Utils` 跨平台系统工具集

`vlink::Utils` 是 vlink 的"杂物抽屉"，提供与具体业务无关的跨平台系统能力：进程信息、环境变量、网络枚举、单实例校验、线程管理、资源监控、信号处理等。底层适配 Linux / macOS / Windows / QNX / Android。

读完本示例你能掌握：

- vlink 应用启动时常用的几个查询（PID / 路径 / hostname / 网络接口）。
- 环境变量的读写。
- 信号处理（终止信号 + 崩溃信号）。
- 线程级 API（名字、优先级、亲和性、CPU yield）。
- 资源监控埋点（CPU / 内存）。

## 背景与适用场景

适用：

- 应用启动期初始化（注册信号、确定数据目录、检查单实例）。
- 运行期 metric 埋点（CPU 占用、内存 RSS）。
- 跨平台代码减少 `#ifdef` 分支。

不适合：

- 高频热路径（这些 API 大多含系统调用）。
- 涉及具体业务的工具（自己写或用其它库）。

## 核心 API（按类别）

### 进程

| API | 说明 |
|-----|------|
| `Utils::get_pid` / `get_pid_str` | 当前 PID |
| `Utils::get_app_path` | 可执行文件完整路径 |
| `Utils::get_app_dir` / `get_app_name` | 目录 / 名字 |
| `Utils::get_host_name` | hostname |
| `Utils::get_tmp_dir` | 临时目录（/tmp 或对应） |
| `Utils::get_machine_id` | 机器唯一标识 |

### 环境变量

| API | 说明 |
|-----|------|
| `Utils::get_env(key, default)` | 读 |
| `Utils::set_env(key, value, overwrite)` | 写 |
| `Utils::unset_env(key)` | 删 |

### 网络

| API | 说明 |
|-----|------|
| `Utils::get_all_ipv4_address(only_up)` | 枚举 IPv4 |
| `Utils::get_interface_name_by_ipv4(addr)` | 反查接口名 |
| `Utils::get_dds_default_address(prefer_up, n)` | DDS 默认绑定的最优接口 |

### 单实例

| API | 说明 |
|-----|------|
| `Utils::check_singleton(name)` | 跨进程单实例锁；同名再启返回 false |

### 线程

| API | 说明 |
|-----|------|
| `Utils::set_thread_name(s)` | top/htop 可见 |
| `Utils::get_native_thread_id` | 平台 native tid |
| `Utils::set_thread_priority(policy, prio, cpu_set)` | 优先级 + 亲和性 |
| `Utils::yield_cpu` | `pause` 指令（自旋等待中用） |

### 资源监控

| API | 说明 |
|-----|------|
| `Utils::get_cpu_usage` | 当前进程 CPU% |
| `Utils::get_memory_usage` | RSS 字节 |
| `Utils::is_process_running(name)` | 进程是否存在 |

### 杂项

| API | 说明 |
|-----|------|
| `Utils::get_timezone_diff` | 与 UTC 偏移（秒） |
| `Utils::get_terminal_size` | tty 大小 |
| `Utils::register_terminate_signal(fn, dump_stack=false, dump_thread_info=false)` | SIGTERM/SIGINT |
| `Utils::register_crash_signal(fn)` | SIGSEGV/SIGABRT/SIGFPE/SIGBUS |
| `Utils::set_console_utf8_output` | Windows 控制台 UTF-8 |
| `Utils::try_release_sys_memory` | malloc_trim 类似 |

## 代码导读

### 1. 进程信息

```cpp
MLOG_I("  pid={} app={} dir={}", Utils::get_pid(), Utils::get_app_path(), Utils::get_app_dir());
MLOG_I("  host={} machine_id={}", Utils::get_host_name(), Utils::get_machine_id());
```

### 2. 环境变量

```cpp
auto v = Utils::get_env("PATH", "");
Utils::set_env("MY_KEY", "value", /*overwrite=*/true);
Utils::unset_env("MY_KEY");
```

### 3. 网络枚举

```cpp
auto ipv4s = Utils::get_all_ipv4_address(/*only_up=*/true);
for (const auto& addr : ipv4s) {
  MLOG_I("  ip={} iface={}", addr, Utils::get_interface_name_by_ipv4(addr));
}

std::string default_ip = Utils::get_dds_default_address(true, 0);
MLOG_I("  dds default: {}", default_ip);
```

### 4. 单实例

```cpp
if (!Utils::check_singleton("my_app")) {
  VLOG_E("another instance is running");
  return 1;
}
```

### 5. 线程控制

```cpp
Utils::set_thread_name("worker_1");
auto tid = Utils::get_native_thread_id();
Utils::set_thread_priority(SCHED_FIFO, 50, {0, 1});   // 绑 CPU 0/1，FIFO 优先级 50
Utils::yield_cpu();                                   // pause 指令
```

### 6. 资源监控

```cpp
MLOG_I("  cpu={}% mem={} bytes", Utils::get_cpu_usage(), Utils::get_memory_usage());
MLOG_I("  self running: {}", Utils::is_process_running(Utils::get_app_name()));
```

### 7. 信号处理

```cpp
Utils::register_terminate_signal([](int sig) {
  VLOG_I("got terminate sig=", sig);
}, /*dump_stack=*/true, /*dump_thread_info=*/true);

Utils::register_crash_signal([](int sig) {
  VLOG_E("crash sig=", sig);
  // 注意：crash handler 内只能调 async-signal-safe 函数
});
```

## 运行

```bash
./build/output/bin/example_utils
```

预期输出（节选）：

```
  pid=12345 app=/work/vlink/build/output/bin/example_utils dir=...
  host=mybox machine_id=...
  PATH=/usr/local/sbin:/usr/local/bin:...
  ip=192.168.1.10 iface=eth0
  dds default: 192.168.1.10
  singleton ok
  thread name=worker_1 tid=...
  cpu=0.5% mem=12345678 bytes
  self running: 1
  timezone diff: 28800 sec (UTC+8)
  terminal size: 80x24
  signal handlers registered
```

## 常见陷阱

1. **`set_thread_priority` 静默失败**：Linux 上需要 `CAP_SYS_NICE`；无权限时返回 false。
2. **`register_crash_signal` 回调内做不安全操作**：crash 时只能 async-signal-safe；不要 malloc/log。
3. **`check_singleton` 进程崩溃后**：vlink 在 /tmp 用文件锁；正常退出会清，但 kill -9 之后下次启动可能误判 —— 用 PID 文件 + 检查进程是否真在跑。
4. **网络枚举包含 lo / docker 虚拟接口**：用 `only_up=true` 过滤；DDS 默认地址选择有专门启发式。
5. **环境变量改动**：fork 出去的子进程会继承当前的环境，但已运行的兄弟进程不会看到。

## 设计要点

- 大部分 API 在编译期分支选择实现（Linux / Windows / macOS）。
- `register_terminate_signal` 内部用 `sigaction` 注册 SIGTERM/SIGINT；可同时注册多个 handler 链式调用。
- `get_dds_default_address` 在跨网卡机器上做"优先 UP、非 loopback、有效路由"启发式选择。

## 配图

无专属配图。

## 参考

- `../process/` — 子进程管理
- `vlink/include/vlink/base/utils.h` — Utils 接口
- `vlink/include/vlink/base/helpers.h` — 字符串、文件大小等纯算法 helper
