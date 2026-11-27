# 11. Launcher 编排与进程守护

VLink Launcher 是位于 `src/external/launcher_api/` 与 `include/vlink/external/` 的轻量化进程与组件编排子系统。其核心设计理念是 **Write Once, Run In-Process or Standalone**：同一份 `vlink::LauncherComponent` 节点代码，既可编译为动态库在 `LauncherServer` 同进程内零拷贝运行，也可仅链接 `vlink::launcher_api` 提供的极简启动器作为独立进程运行（或通过 `vlink-launcher exec` 直接拉起插件），底层统一通过 `LauncherNotify` 协议完成就绪、健康与看门狗心跳上报。

---

## 11.1 架构与目标分层

| CMake 目标 | 公开头文件 | 职责 |
| --- | --- | --- |
| `vlink::launcher_api` | `launcher_component.h`、`launcher_notify.h`、`launcher_api.h` | 组件接口与独立启动器、进程通知/看门狗客户端（`LauncherNotify`）、控制面客户端（`LauncherAPI`） |
| `vlink::launcher_server` | `launcher_server.h` | TOML 多层嵌套编排器（`LauncherServer`）、组件宿主机、进程守护、通知服务端（`NotifyServer`） |
| `vlink-launcher` | — | 轻量 CLI 工具（`run` / `host` / `validate` / `exec` / `plugin` / `status` / `events` / `start` / `stop` / `restart` / `reload` / `params` / `set-param` / `shutdown`） |

---

## 11.2 Write Once：节点同进程 / 独立进程无缝切换

只需编写一份继承自 `vlink::LauncherComponent` 的节点类（见 [examples/launcher/sensor_node.h](../examples/launcher/sensor_node.h)）：

```cpp
class SensorNode final : public vlink::LauncherComponent {
 public:
  bool on_start(vlink::LauncherContext& ctx) override {
    const int64_t rate = ctx.get_param_int("rate_hz", 5);
    ctx.mark_ready("ready");
    return true;
  }
  void on_stop(vlink::LauncherContext& ctx) override {}
  bool on_reload(vlink::LauncherContext& ctx) override { return true; }
  bool on_param_changed(vlink::LauncherContext& ctx, const std::string& key,
                        const vlink::LauncherParamValue& value) override { return true; }
};
```

通过 1 行宏即可在三种运行形态间自由切换，业务代码零修改：

1. **同进程组件插件（`[[component]]`）**：
   ```cpp
   VLINK_LAUNCHER_EXPORT_SINGLE_COMPONENT(SensorNode)
   ```
2. **独立进程可执行文件（`[[process]]`，仅链接 `vlink::launcher_api`）**：
   ```cpp
   VLINK_LAUNCHER_COMPONENT_MAIN(SensorNode)
   ```
   受 `LauncherServer` 托管时，`LauncherContext` 自动将 `mark_ready()`、`report_health()`、`watchdog_ping()`、`VLINK_LAUNCHER_PARAMS_JSON` 参数与 `SIGHUP -> on_reload()` 桥接到 `LauncherNotify` 协议。
3. **零 `main()` 直接独立运行插件 `.so`**：
   ```bash
   vlink-launcher exec liblauncher_demo_sensor_plugin.so SensorNode --name lidar --param rate_hz=10
   ```
   受父 `LauncherServer` 托管时自动继承 `VLINK_LAUNCHER_PLUGIN_DIRS`，可直接写短名（如 `launcher_demo_sensor_plugin`）。
4. **独立分组宿主进程（`vlink-launcher host`，故障域隔离 + 组内零拷贝）**：
   将一组需要同进程零拷贝的 `[[component]]` 写入子清单，由父清单以 `[[process]]` 启动 `vlink-launcher host group.toml`（配合 `ready.type = "notify"` 与 `watchdog_ms`）。宿主子进程默认关闭控制面，并自动将组内聚合就绪、健康状态、看门狗心跳与 `SIGHUP` 热重载桥接到 `LauncherNotify`，避免用户插件异常影响主编排器。

组件内部可通过 `ctx.resolve_url("dds://sensor/lidar/event/0")` 声明默认 VLink URL：命中 `[*.remaps]` 时原样返回重映射目标，否则在同进程模式下自动将协议切换为 `intra://`（独立进程保持原 URL）。除通用 `LauncherComponent` 外，同一头文件还直接基于 VLink 原生原语提供两个薄基类（均支持同进程与独立进程切换）：

- `LauncherTimerComponent`：托管 `vlink::Timer`，自动读取并支持在线热更新 `period_ms`、周期喂狗并触发 `on_tick(ctx)`；
- `LauncherCoroutineComponent`（C++20 `VLINK_ENABLE_COROUTINE` 门控）：托管 `vlink::Co::Task<void>`，停机时自动通过 `ctx.cancellation_token()` 协作取消并安全排空协程。

---

## 11.3 TOML 编排清单（支持多层嵌套与参数化）

清单支持以下顶层节：

- `[launcher]`：`name`、`ns`、`ctrl_enabled`、`ctrl_scheme`、`ctrl_namespace`、`ctrl_domain`、`key`、`shutdown_timeout_ms`、`plugin_dirs`、`includes`（支持多层递归嵌套，最大深度 16，自动检测循环引用）。
- `[args]`：声明启动参数默认值，在任意字符串字段中使用 `${arg.<key>}` 引用（同时支持 `${this_dir}` 与 `${env.<VAR>}`），运行时可通过 `--arg k=v` 或 `LauncherServer::Config::args_override` 覆盖。
- `[[include]]`：结构化嵌套引入子清单，支持 `file`、可选 `enabled`（布尔或 `${arg.*}`/`!${arg.*}`）、`ns`（命名空间叠加）、`group`（默认分组）、`prefix`（为子树所有单元名及子树内部依赖自动加前缀）、`args`（参数覆盖）与 `remaps`（URL 重映射下发）。
- `[[component]]`：`name`、`ns`、`group`、`lib`、`class`、`scheduler`（`shared|dedicated|multi`，兼容 `dedicated = true`）、`threads`（`multi` 模式工作线程数 `1..64`）、`queue`（`normal|lockfree|priority`）、`queue_strategy`（`optimization|pop|block`）、`enabled`、`params_file`（单文件或文件数组，支持 `.toml`/`.json`，内联 `params` 优先级更高）、`params`、`remaps`（URL 重映射）、`autostart`、`required`、`delay_ms`、`stop_timeout_ms`、`watchdog_ms`、`restart`（`no|on_failure|always`）、`restart_delay_ms`、`restart_max_ms`、`start_burst`、`start_window_ms`、`depends_on`、`after`、`ready`（`none|component|delay`）。
- `[[process]]`：`name`、`ns`、`group`、`cmd`、`args`、`cwd`、`inherit_env`、`env`、`scheduler`（`dedicated|multi`）、`threads`、`queue`、`queue_strategy`、`enabled`、`params_file`、`params`（超过 16 KiB 时自动落盘经 `VLINK_LAUNCHER_PARAMS_FILE` 传递）、`remaps`、`output`（`screen|none`）、`autostart`、`required`、`delay_ms`、`stop_timeout_ms`、`watchdog_ms`、`restart*`、`start_burst`、`start_window_ms`、`depends_on`、`after`、`ready`（`none|notify|stdout|delay`）。

参考模板见 [src/external/launcher_api/etc/launcher_example.toml](../src/external/launcher_api/etc/launcher_example.toml) 与 [examples/launcher/launcher_demo.toml](../examples/launcher/launcher_demo.toml)。

---

## 11.4 进程通知与看门狗协议（`LauncherNotify`）

普通外部进程（C++ 或 Python，无需继承 `LauncherComponent`）也可直接调用 `vlink::LauncherNotify`（类 systemd `sd_notify`）：

- `LauncherNotify::is_managed()` / `unit_name()` / `unit_generation()` / `watchdog_interval_ms()`
- `LauncherNotify::notify_ready(status)`、`notify_watchdog()`、`notify_health(ok, status)`、`notify_status(status)`
- `LauncherNotify::notify_reloading(status)`、`notify_stopping(status)`、`notify_watchdog_trigger(reason)`、`notify_extend_timeout(extra_ms)`
- 原始多行键值接口 `LauncherNotify::notify("READY=1\nSTATUS=ok\n")`

传输层在 Linux/macOS/QNX/Android 优先使用私有权限 Unix 域数据报套接字，Windows 使用回环 UDP 数据报，每条报文强制携带每单元随机 128-bit `TOKEN=` 认证。

---

## 11.5 CLI 命令与 Python 绑定

```bash
# 编排运行与独立组件宿主进程（支持参数覆盖、子集筛选与干跑预览）
vlink-launcher run manifest.toml [--arg k=v]... [--only unit|@group]... [--skip unit|@group]... \
  [--dry-run] [--scheme S] [--ns N] [--domain D] [--key K] [--no-ctrl] [--no-respawn]
vlink-launcher host group.toml [--arg k=v]... [--only unit|@group]... [--skip unit|@group]... [--dry-run] [--no-respawn]

# 静态校验、插件导出查看与独立运行插件
vlink-launcher validate manifest.toml [--arg k=v]... [--check-programs]
vlink-launcher plugin libsensor.so
vlink-launcher exec libsensor.so [ClassName] [--name N] [--ns NS] [--param k=v]... [--remap src=dst]...

# 运行期控制与实时事件观测
vlink-launcher status|events|shutdown [--scheme S] [--ns N] [--domain D] [--key K] [--timeout MS]
vlink-launcher start|stop|restart|reload <unit|@group> [ctrl opts]
vlink-launcher params [unit] [ctrl opts]
vlink-launcher set-param <unit> <key=value> [ctrl opts]
```

控制面加密密钥优先通过环境变量 `VLINK_LAUNCHER_SECURITY_KEY` 传入；若未编译 `VLINK_ENABLE_SECURITY` 却配置了密钥，服务端与客户端会显式拒绝启动而非静默降级为明文。Python 侧经 `vlink.LauncherServer`、`vlink.LauncherAPI`、`vlink.LauncherNotify` 及 `vlink.launcher` 子模块提供 1:1 绑定（详见 [第 14 章集成](14-integration.md)）。
