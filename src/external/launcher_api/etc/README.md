# VLink Launcher TOML 配置参数详解

本目录提供 VLink Launcher 的参考清单 [`launcher_example.toml`](./launcher_example.toml) 与 Shell 自动补全脚本。本文档按节完整展开说明 **每一个 TOML 参数** 的类型、默认值、取值范围、生效机制、兼容别名以及典型用法。

> 建议配合以下命令实时校验配置与查看解析结果：
>
> ```bash
> vlink-launcher validate launcher_example.toml
> vlink-launcher inspect  launcher_example.toml --show_args
> vlink-launcher graph    launcher_example.toml --format ascii
> ```

---

## 目录

1. [模板变量、条件表达式与作用域规则](#1-模板变量条件表达式与作用域规则)
2. [`[launcher]` 全局编排与控制面](#2-launcher-全局编排与控制面)
3. [`[args.<name>]` 启动参数声明](#3-argsname-启动参数声明)
4. [`[urls]` / `[env]` / `[remaps]` / `[params]` 全局上下文](#4-urls--env--remaps--params-全局上下文)
5. [`[logger]` 日志管理与 `logd` 插件集成](#5-logger-日志管理与-logd-插件集成)
6. [`[record]` 主动 Bag 录制配置](#6-record-主动-bag-录制配置)
7. [`[proxy]` 受管 `vlink-proxy` 边车](#7-proxy-受管-vlink-proxy-边车)
8. [`[[slice]]` 与 `[*.resources]` 资源治理限制](#8-slice-与-resources-资源治理限制)
9. [`[[group]]` 逻辑任务组与属性继承](#9-group-逻辑任务组与属性继承)
10. [`[[scheduler]]` VLink 原生组件调度器](#10-scheduler-vlink-原生组件调度器)
11. [`[[container]]` 独立隔离组件容器](#11-container-独立隔离组件容器)
12. [`[[component]]` 进程内托管组件](#12-component-进程内托管组件)
13. [`[[process]]` 受管外部进程](#13-process-受管外部进程)
14. [`[[task]]` 一次性与周期性任务](#14-task-一次性与周期性任务)
15. [`[[include]]` 多文件组合包含](#15-include-多文件组合包含)
16. [通用依赖、探针、重启与触发器子表](#16-通用依赖探针重启与触发器子表)

---

## 1. 模板变量、条件表达式与作用域规则

### 1.1 字符串变量插值 `${...}`

所有字符串字段与字符串数组元素均支持 `${expr}`、默认值回退 `${expr:-fallback}` 以及字面量转义 `$${...}`（展开为原样 `${...}`）：

| 变量表达式 | 展开内容 | 说明 |
| --- | --- | --- |
| `${arg.<name>}` | `[args.<name>]` 的生效值 | 支持 `--arg <name>=<val>` 或 `[include.args]` 覆盖 |
| `${url.<key>}` | 当前作用域已声明的 URL | 优先级：单元 `[*.urls]`/`inputs`/`outputs` > 组 `[group.urls]` > 文件 `[urls]` |
| `${env.<VAR>}` | 宿主系统环境变量 | 未设置或为空时触发 `:-fallback` |
| `${this_dir}` | 当前 TOML 文件所在目录 | 便于写相对路径，跨 `[[include]]` 自动切换为子文件目录 |
| `${cwd}` | 当前进程工作目录 | 启动 `vlink-launcher` 时的工作目录 |
| `${host}` | 当前主机名 | 底层调用 `Utils::get_host_name()` |
| `${pid}` | 当前宿主进程 PID | 底层调用 `Utils::get_pid_str()` |
| `${os}` | 操作系统标识 | `linux` / `windows` / `macos` / `qnx` / `android` |
| `${arch}` | CPU 架构标识 | `x86_64` / `aarch64` / `arm` / `riscv64` / `x86` |
| `${ns}` | 当前生效的命名空间 | 由根 `[launcher].namespace` 与 `[[include]].namespace` 拼接 |
| `${item}` | `for_each` / `count` 当前元素值 | 仅在模板展开块内有效 |
| `${index}` | `for_each` / `count` 当前序号（从 `0` 开始） | 仅在模板展开块内有效 |

### 1.2 条件表达式 `condition`

`condition` 可填布尔值（`true`/`false`）或字符串表达式，**先按运算符语法解析为 AST，再对各原子操作数执行变量展开并求值**（可防止参数值中包含 `||` / `&&` 时产生逻辑注入；非布尔/非字符串类型、括号不匹配或悬空运算符均报解析错误）：

- 运算符优先级（从低到高）：`||` -> `&&` -> `!` -> `==` / `!=` -> `(...)` 分组。
- 原子真值判定：空串、`false`、`0`、`no`、`off`（大小写不敏感）为假，其余为真。
- 示例：`condition = "(${arg.profile} == vehicle || ${arg.profile} == hil) && ${os} == linux"`。

### 1.3 命名空间与作用域优先级

- 命名空间限定：不以 `/` 开头的单元名自动添加当前 `namespace/` 前缀；以 `/` 开头表示全局绝对名（去掉前导 `/`）；以 `@` 开头引用逻辑组（`@/grp` 表示全局绝对组名）。在 URL 中使用 `scheme:///abs/path`（`://` 后带前导 `/`）可显式跳过当前单元的 `namespace` 自动前缀（非 `uds://` 协议在解析时自动去掉该前导 `/`）。
- 属性继承优先级（由高到低）：**单元自身声明 > 所属 `[[group]]` > 所在文件顶层作用域 > 父文件词法作用域**。
- `[[include]]` 拥有独立词法作用域：子文件的 `[env]`、`[urls]`、`[remaps]`、`[params]` 不会反向污染父文件或其他兄弟 `[[include]]`。
- **严格 Schema 与别名互斥规则**：所有 TOML 表均校验允许键集合与字段类型，拼写错误或未知键会直接报解析错误；同一 TOML 表中**不能同时填写**标准键与其历史别名（如同时写 `startup` 与 `startup_mode`），避免静默覆盖。

---

## 2. `[launcher]` 全局编排与控制面

仅根 TOML 文件的 `[launcher]` 定义全局控制面和启动策略；子 `[[include]]` 文件中的 `[launcher]` 仅叠加 `namespace` 与 `plugin_dirs`。

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明与用法 |
| --- | --- | --- | --- | --- |
| `name` | - | string | `"vlink-launcher"` | 系统/会话名称；同时作为 `[logger].plugin` (`logd`) 初始化时的 `app_name` 以及 `[record].tag` 默认值。 |
| `namespace` | - | string | `""` | 默认命名空间前缀，自动拼接到未以 `/` 开头的单元、组及依赖引用前。 |
| `startup` | `startup_mode` | string | `"parallel"` | 冷启动并发策略：`parallel`（按 DAG 最大并行）、`serial`（严格逐个启动并等待就绪）、`max_parallel`（受 `max_parallel` 上限约束）。可被 CLI `--serial` / `--parallel` 覆盖。 |
| `max_parallel` | - | int | `0` | `startup = "max_parallel"` 时必填且 `>= 1`，限制同时处于 `Starting` 状态的单元数量。 |
| `target` | `default_target` | string | `""` | 默认启动目标（单个单元名或 `@group`）。留空时按全部 `autostart = true` 且 `condition = true` 单元冷启动。 |
| `stop_timeout_ms` | `shutdown_timeout_ms` | int | `5000` | 优雅停机全局超时（毫秒，最小 `200`）；逆拓扑逐层停机受此预算约束。 |
| `ctrl` | `control_enabled` | bool | `true` | 是否启动 VLink 控制面端点（状态快照、RPC、事件流、日志流）。可被 CLI `--no_control` 关闭。 |
| `ctrl_scheme` | `control_scheme` | string | `""` | 控制面传输协议：`dds` / `ddsc` / `ddsr` / `zenoh` / `intra`。留空按编译支持的协议自动选择。**使用 `[[container]]`、`key` 加密或跨进程 `logd` 时不可为 `intra`**。 |
| `ctrl_namespace` | `control_namespace` | string | `"launcher"` | 控制面 VLink 主题命名空间前缀（与 `vlink-launcher ctl --namespace` 对应）。 |
| `ctrl_domain` | `control_domain` | int | `0` | 控制面传输域 ID（对应 `--domain`，避免同机多套 Launcher 互相发现）。 |
| `key` | `security_key` | string | `""` | 可选 AES-128-GCM 密钥，开启控制面通信加密（也可通过 `VLINK_LAUNCHER_SECURITY_KEY` 传入；不可与 `intra` 共用）。内置 `logd` 会额外生成独立会话密钥，不会将控制面密钥暴露给普通子进程。 |
| `bind_ip` | - | string | `""` | 控制面绑定的网卡 IP（透传至 `dds.ip` / `zenoh.ip` 等传输参数）。 |
| `native` | `native_mode` | bool | `false` | `true` 时限制控制面发现范围在本机（`local-only`），避免跨主机串扰。 |
| `remote_load` | `allow_remote_load` | bool | `false` | 安全开关：未配置 `key` 加密时默认拒绝远程 `ctl load` 动态加载 `.so`，置 `true` 显式放行。 |
| `plugin_dirs` | - | string[] | `[]` | 组件动态库与 Logger 插件的额外搜索目录列表；相对路径基于当前 TOML 所在目录解析，搜索优先级高于 `VLINK_PLUGIN_DIR` 与默认系统路径。 |

---

## 3. `[args.<name>]` 启动参数声明

支持两种写法：

1. **简写形式**（直接赋默认值）：
   ```toml
   [args]
   cams = "front,rear"
   rate_ms = 100
   ```
2. **完整表形式**（带可选值约束与描述）：

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明与用法 |
| --- | --- | --- | --- | --- |
| `default` | - | string / int / float / bool | `""` | 参数默认值；所有类型统一转为字符串供 `${arg.<name>}` 插值。 |
| `desc` | `description` | string | `""` | 参数用途说明，在 `vlink-launcher run --show_args` 和 `inspect --show_args` 表格中展示。 |
| `choices` | - | string[] | `[]` | 允许取值白名单；非空时若最终值（含 `--arg` 覆盖）不在列表中则解析失败。 |

---

## 4. `[urls]` / `[env]` / `[remaps]` / `[params]` 全局上下文

| 表名 | 键 / 值格式 | 作用与继承机制 |
| --- | --- | --- |
| `[urls]` | `key = "scheme://..."` | 声明全局逻辑 URL（强校验包含 `scheme://`）。可通过 `${url.<key>}` 在任意字段引用，并自动注入子进程环境变量 `VLINK_URL_<KEY>`（大写、非字母数字转 `_`）以及组件 `ctx.get_url("<key>")`。 |
| `[env]` | `KEY = string \| int \| bool` | 注入到所有进程、容器、任务和 Proxy 的环境变量；布尔值转为 `"1"`/`"0"`。单元自身 `[*.env]` 优先覆盖组 `[group.env]`，组优先覆盖全局 `[env]`。 |
| `[remaps]` | `"src_url" = "dst_url"` | 声明 URL 重映射规则，采用“后定义优先、边界前缀匹配并保留后缀”语义（单元测试/数据流图/组件/进程共用同一语义）。优先级：单元 > 组 > 全局。 |
| `[params]` | 任意嵌套 TOML 表 | 扁平化为点分路径键（如 `a.b = 1`），支持 `bool`、`int64`、`double`、`string` 及对应数组类型；注入到所有托管组件 `ctx.get_param*()`，可运行期 `ctl set-param` 热更新。 |
| `params_file` / `params_files` | string / string[] | 可置于文件顶层、`[[group]]` 或 `[[component]]`，加载外部 TOML 参数文件（相对路径基于 `this_dir`），内联 `[params]` 覆盖外部文件同名键。 |

---

## 5. `[logger]` 日志管理与 `logd` 插件集成

VLink Launcher 既支持内置异步滚动文件后端（`LoggerBackend`），也**原生集成 VLink `LoggerPluginInterface`（`logd`）**，实现宿主、进程内组件、隔离容器与外部 VLink 进程全链路统一日志汇聚。

### 5.1 参数列表

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明与用法 |
| --- | --- | --- | --- | --- |
| `dir` | - | string | `""` | 会话日志根目录。留空依次回退 `VLINK_LOG_DIR` 与系统临时目录 `/tmp/vlink-launcher`；每次运行自动创建独立会话子目录 `<dir>/<session_id>/{launcher,nodes,containers,tmp}`。可被 CLI `--log_dir` 覆盖。 |
| `console_level` | - | string | `"info"` | 控制台输出最低级别：`trace` / `debug` / `info` / `warn`（别名 `warning`） / `error` / `fatal` / `off`（别名 `none`）。可被 CLI `--log_level` 覆盖。 |
| `file_level` | - | string | `"debug"` | 文件后端与 Logger 插件（`logd`）通道最低级别；设为 `off` 时禁用文件落盘与插件加载。 |
| `max_size_mb` | `max_file_size_mb` | int | `10` | 单个滚动日志文件大小上限（MB，最小 `1`），同步透传子进程 `VLINK_LOG_MAX_SIZE`。 |
| `max_files` | - | int | `10` | 每单元保留的滚动日志文件个数上限（最小 `1`），透传 `VLINK_LOG_MAX_COUNT`。 |
| `flush_ms` | `flush_delay_ms` | int | `500` | 内置异步日志后端刷新间隔（毫秒，最小 `10`），透传 `VLINK_LOG_FLUSH_DELAY`。 |
| `history` | `history_max_records` | int | `4096` | 宿主内存环形日志缓冲区容量（条数，`0` 表示不缓存）；用于 `ctl logs -n` 历史回溯。 |
| `fixed_name` | `fixed_filename` | bool | `false` | `true` 时使用固定文件名轮转（透传 `VLINK_LOG_STORE_STRATEGY=1`）。 |
| `append` | - | bool | `false` | `true` 时以追加模式打开已有日志文件（透传 `VLINK_LOG_OPEN_APPEND=1`）。 |
| `utc` | - | bool | `false` | `true` 时日志时间戳使用 UTC（透传 `VLINK_LOG_UTC_ENABLE=1`）。 |
| `console_fmt` | - | bool | `false` | `true` 时子进程控制台输出附带时间戳/线程 ID/级别前缀（透传 `VLINK_LOG_CONSOLE_FMT=1`）。 |
| `capture_file` | `capture_to_file` | bool | `true` | 是否捕获子进程 stdout/stderr（当单元 `output` 为 `log` 或 `both` 时写入 `captured/` 或在 `plugin_capture=true` 时转发至插件）。 |
| `strip_ansi` | - | bool | `true` | 捕获输出与历史缓冲中是否自动剥离 ANSI 颜色转义序列。 |
| `plugin` | - | string | `""` | **Logger 插件（`logd`）库名或路径**。填入 `"logd"`（或 `"vlink-launcher_logd"`）启用 Launcher **内置集中式 `logd` 后端**（通过内置 `libvlink-launcher_logd` 插件库经 VLink 传输将宿主、容器与外部进程日志统一汇入 `LauncherServer` 的 `<session_dir>/logd/` 滚动文件及 `ctl logs` 缓冲）；也可填入任意自定义 `vlink::LoggerPluginInterface` v1.0 动态库名或路径（如 `"my_logd"` 或 `"/opt/lib/libmy_logd.so"`）。支持 CLI `run --log_plugin <lib>` 覆盖，也可用 `vlink-launcher plugin <lib>` 检验。 |
| `plugin_strict` | - | bool | `false` | `false`（默认）：插件加载或 `init()` 失败时打印告警并自动降级为内置文件后端（同时不向子进程透传失效的 `VLINK_LOG_PLUGIN`）；`true`：加载或 `init()` 失败直接返回启动错误。 |
| `plugin_capture` | - | bool | `false` | `false`（默认）：避免与 VLink 子进程原生 `VLINK_LOG_PLUGIN` 产生双写，捕获的 stdout/stderr 仍写入本地 `captured/`；`true`：将非容器子进程（如 Python/Shell/第三方程序）在 `output = "log"\|"both"` 下捕获的 stdout/stderr（满足 `file_level` 时）一并转发给 Logger 插件。 |

### 5.2 `logd` 插件在四类运行单元中的路由行为

| 单元类型 | 日志进入 `logd` 插件的方式 | 说明 |
| --- | --- | --- |
| 宿主与宿主内 `[[component]]` | `LogManager` 按 `plugin_dirs` + 默认路径加载插件，通过 `Logger::register_file_handler` 接管宿主文件通道 | 按 `file_level` 过滤（独立于 `console_level`），每条日志自动附带 `[unit_name]` 标签，停机前调用 `flush()`。 |
| `[[container]]` 与容器内组件 | 容器清单自动序列化 `[logger]` 配置，容器内 `LogManager` 独立加载插件（`app_name = container.name`） | 容器环境显式清空 `VLINK_LOG_PLUGIN=""`，防止继承宿主环境导致重复加载或双写。 |
| 外部 VLink `[[process]]` / `[proxy]` | 宿主自动注入 `VLINK_LOG_PLUGIN`（优先解析为绝对路径）与逗号分隔的 `VLINK_PLUGIN_DIR` | 外部 VLink 进程由自身 `vlink::Logger` 原生直连 `logd`，零额外进程间拷贝。 |
| 非 VLink 外部程序（Shell/Python 等） | 开启 `plugin_capture = true` 且单元 `output = "log"\|"both"` | 宿主捕获其 stdout (`Info`) / stderr (`Warn`) 并转发至 `logd` 插件。 |

---

## 6. `[record]` 主动 Bag 录制配置

基于 VLink 原生节点级主动录制能力（`Node::set_record_path` / `VLINK_BAG_PATH` / `Publisher::publish(..., force)`），支持**声明式静态开启**与**运行期 `ctl record start/stop` 动态热切换**。

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明与用法 |
| --- | --- | --- | --- | --- |
| `enabled` | - | bool | `false` | 全局默认是否启用主动录制。也可通过 CLI `vlink-launcher run --record_dir <dir>` 一键启用。 |
| `dir` | - | string | `""` | Bag 输出根目录（自动转为绝对路径）；留空默认为 `<session_dir>/bags`，自动按 `components/`、`containers/`、`processes/` 分目录生成带代次编号（`_g<gen>`）的独立文件，重启或分段不会覆写历史数据。 |
| `format` | - | string | `"vcap"` | 默认 Bag 文件格式：`vcap` / `vcapx` / `vdb` / `vdbx`。 |
| `tag` | - | string | `[launcher].name` | 会话录制标签标识。 |
| `force` | `force_publish` | bool | `false` | `true` 时，托管组件使用 `LauncherPublisher::publish()` 在无在线订阅者情况下也会强制序列化并写入 Bag，避免“无人订阅导致录制为空”。 |

> **重要限制与安全保证**：
> - `intra://` 传输不支持节点级 `set_record_path()`，`LauncherPublisher` / `LauncherSubscriber` 会自动检测并安全降级为告警日志，不会触发崩溃。
> - 不同 OS 进程（如宿主组件 vs 容器组件、不同外部进程）禁止共用同一个显式 `record_path`，`validate` 与 `ctl record` 均会拦截以防并发截断损坏 Bag；同一 OS 进程内的多个组件可安全共享同一个 `record_path`（共用同一个 `BagWriter`）。

---

## 7. `[proxy]` 受管 `vlink-proxy` 边车

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明与用法 |
| --- | --- | --- | --- | --- |
| `enabled` | - | bool | `false` | 是否启用受管 `vlink-proxy` 边车单元。 |
| `name` | - | string | `"vlink_proxy"` | 单元名称（遵循当前命名空间 `namespace` 限定）。 |
| `cmd` | `program` | string | `"vlink-proxy"` | Proxy 可执行程序名或路径。 |
| `dds` | `dds_impl` | string | `""` | DDS 实现后端：`dds` / `ddsc` / `ddsr`；留空按构建启用的后端自动选择。 |
| `domain` | `domain_id` | int | `0` | Proxy 服务的 DDS 域 ID。 |
| `iox` | `use_iox` | bool | `false` | 是否启用 Iceoryx 零拷贝共享内存传输。 |
| `iox_strategy` | - | int | `0` | Iceoryx 内存池策略编号（`>= 0`）。 |
| `iox_config` | - | string | `""` | Iceoryx 配置文件路径（相对路径基于 `this_dir`）。 |
| `config` | `config_file` | string | `""` | 自定义 Proxy JSON 配置文件（相对路径基于 `this_dir`）；留空时由 Launcher 自动在会话 `tmp/` 目录生成。 |
| `extra_args` | `args` | string[] | `[]` | 追加在自动 `--config <file>` 之后的额外命令行参数。 |
| `stage` | - | int | `-100` | 启动阶段号；默认 `-100` 确保先于默认 `stage = 0` 的业务单元就绪。 |
| `group` | - | string | `""` | 所属逻辑任务组，可继承组环境变量与资源切片。 |
| `required` | - | bool | `false` | `true` 时 Proxy 异常退出将触发整个系统安全停机。 |
| `autostart` | - | bool | `true` | 是否随冷启动自动拉起。 |
| `output` | - | string | `"log"` | 输出去向：`screen` / `log` / `both` / `none`。 |
| `ready_delay_ms` | `ready_ms` | int | `300` | 启动后等待就绪的延迟时间（毫秒）。 |
| `stop_timeout_ms` | `sigterm_timeout_ms` | int | `5000` | 发送停止信号后的等待超时（毫秒，最小 `200`）。 |
| `kill_mode` | - | string | `"process"` | 停止范围：`process` / `control-group` / `mixed`。 |
| `stop_signal` | - | string | `"SIGTERM"` | 优雅停止信号：`SIGTERM` / `SIGINT` / `SIGHUP` / `SIGKILL`。 |
| `reload` | - | string | `"none"` | 热重载模式：`none` / `sighup`。 |
| `[proxy.env]` | - | table | `{}` | Proxy 专属环境变量。 |
| `[proxy.resources]` | - | table | `{}` | 资源限制配置，见第 8 节。 |
| 依赖与重启键 | - | - | - | 支持全部第 16 节依赖字段与重启/限流字段。 |

---

## 8. `[[slice]]` 与 `[*.resources]` 资源治理限制

`[[slice]]` 定义可复用的资源预算模板；进程、容器、任务与 Proxy 在 `[*.resources]` 中通过 `slice = "<name>"` 继承模板，并可按字段覆盖。Linux 下优先使用 **cgroup v2**（配额/内存/PID 上限），无 cgroup 委派权限时自动平滑回退到跨平台 `prlimit` / `nice` / CPU 亲和性 / Windows `JobObject`。

| 参数名 | 兼容别名 | 类型 | 默认值 | 有效范围与详细说明 |
| --- | --- | --- | --- | --- |
| `name` | - | string | 必填（仅 `[[slice]]`） | 资源切片唯一名称，供 `[*.resources].slice` 或 `[[group]].slice` 引用。 |
| `slice` | - | string | `""`（仅 `[*.resources]`） | 引用的 `[[slice]]` 名称；单元未显式指定的字段自动继承自该 slice。 |
| `cpu_quota_percent` | `cpu_quota` | int | 未限制 | CPU 配额百分比（`> 0`，如 `50` = 0.5 核，`200` = 2 核）；Linux 映射至 `cpu.max`。 |
| `cpu_weight` | - | int | 未限制 | CPU 调度权重 `[1, 10000]`；Linux 映射至 cgroup v2 `cpu.weight`。 |
| `mem_max_mb` | `memory_max_mb` | int | 未限制 | 内存硬上限（MB，`> 0`）；Linux 映射至 `memory.max`，Windows 映射至 JobObject 内存上限。 |
| `mem_high_mb` | `memory_high_mb` | int | 未限制 | 内存软节流阈值（MB，`> 0` 且 `<= mem_max_mb`）；Linux 映射至 `memory.high`。 |
| `pids_max` | - | int | 未限制 | 最大进程/线程数上限（`>= 1`）；Linux 映射至 `pids.max`。 |
| `oom_adj` | `oom_score_adj` | int | 未限制 | Linux OOM Killer 评分调整 `[-1000, 1000]`（负值更不易被杀）。 |
| `nice` | - | int | 未限制 | POSIX 进程优先级 `[-20, 19]`（值越小优先级越高；负值需权限）。 |
| `cpus` | `cpu_affinity` | int[] | `[]` | 进程 CPU 亲和性逻辑核号列表（如 `[0, 1]`）。 |
| `nofile` | `rlimit_nofile` | int | 未限制 | 最大打开文件描述符数（`>= 0`，`RLIMIT_NOFILE`）。 |
| `vmem_max_mb` | `rlimit_as_mb` | int | 未限制 | 虚拟地址空间上限（MB，`>= 0`，`RLIMIT_AS`）。 |
| `core_max_mb` | `rlimit_core_mb` | int | 未限制 | Core dump 文件大小上限（MB，`>= 0`，`0` 禁止生成 core，`RLIMIT_CORE`）。 |
| `strict` | - | bool | `false` | `false`：平台不支持或权限不足时打印告警并平滑降级继续运行；`true`：任何资源限制应用失败或平台不支持时立即中止该单元启动（slice 的 `strict = true` 会被引用单元继承）。 |

### 8.1 跨平台资源治理兼容矩阵

不同操作系统对 cgroup、作业对象和进程资源隔离的支持差异如下（`strict = false` 时跨平台自动平滑降级，同一份 TOML 清单可跨平台复用）：

| 能力项 | Linux | Android | Windows | macOS | QNX |
| --- | --- | --- | --- | --- | --- |
| `cpu_quota_percent` | cgroup v2 `cpu.max` | cgroup v2（需委派） | JobObject CPU Hard Cap | 告警降级（`strict=true` 拒绝） | 告警降级（`strict=true` 拒绝） |
| `mem_max_mb` | cgroup v2 `memory.max` | cgroup v2（需委派） | JobObject Process Memory | 告警降级（`strict=true` 拒绝） | 告警降级（`strict=true` 拒绝） |
| `pids_max` | cgroup v2 `pids.max` | cgroup v2（需委派） | JobObject ActiveProcessLimit | 告警降级（`strict=true` 拒绝） | 告警降级（`strict=true` 拒绝） |
| `cpu_weight` / `mem_high_mb` / `oom_adj` | cgroup v2 / `/proc` | cgroup v2 / `/proc` | 告警降级（`strict=true` 拒绝） | 告警降级（`strict=true` 拒绝） | 告警降级（`strict=true` 拒绝） |
| `cpus`（进程亲和性） | `sched_setaffinity` + `cpuset` | `sched_setaffinity` | `SetProcessAffinityMask` | 告警降级（线程绑定支持） | 告警降级（线程 `ThreadCtl` 支持） |
| `nice`（优先级） | `setpriority` | `setpriority` | `SetPriorityClass` | `setpriority` | `setpriority` |
| `nofile` / `vmem_max_mb` / `core_max_mb` | `prlimit` | `prlimit64` (API >= 21) | 告警降级（`strict=true` 拒绝） | 告警降级（`strict=true` 拒绝） | 告警降级（`strict=true` 拒绝） |
| `kill_mode = control-group/mixed` | cgroup v2 / `/proc` 子树防复用 | cgroup v2 / `/proc` 子树 | JobObject `KILL_ON_JOB_CLOSE` | 进程组 / 进程信号 | 进程组 / 进程信号 |
| 遥测采样（CPU / RSS / 线程数） | `/proc` + cgroup v2 | `/proc` + cgroup v2 | `GetProcessTimes` + `PSAPI` | `proc_pidinfo` + `sysctl` | `/proc/<pid>/ctl` `DCMD_PROC_INFO` |

---

## 9. `[[group]]` 逻辑任务组与属性继承

逻辑组类似 systemd target 与 ROS 2 Group 的结合，支持按 `@group` 批量 `start`/`stop`/`restart`/`reload`/`isolate`/`record`，同时向成员下发默认属性。

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明与用法 |
| --- | --- | --- | --- | --- |
| `name` | - | string | 必填 | 逻辑组名（受当前 `namespace` 限定）；在依赖或 CLI 中使用 `@<name>` 引用。 |
| `namespace` | - | string | 继承当前 `namespace` | 逻辑组命名空间。 |
| `mode` | `execution` | string | `"parallel"` | 组成员启动顺序：`parallel`（并行）或 `serial`（按 `members` 声明顺序自动串联依赖边）。 |
| `slice` | - | string | `""` | 默认资源切片名；组成员若未自设 `resources.slice` 则自动继承。 |
| `condition` | - | bool / expr | `true` | 为 `false` 时，组内所有成员的 `condition` 一并置为 `false`（跳过启动）。 |
| `autostart` | - | bool | `true` | 为 `false` 时，组内所有成员的 `autostart` 一并置为 `false`（仅手动 `ctl start @group` 启动）。 |
| `members` | - | string[] | `[]` | 显式列出成员单元名（单元自身声明 `group = "..."` 也会自动归入该组）。 |
| `after` / `before` | - | string[] | `[]` | 组级顺序约束：展开为组内所有成员对目标单元/目标组的排序边。 |
| `[group.urls]` / `[group.env]` / `[group.remaps]` / `[group.params]` | - | table | `{}` | 组级上下文，自动下发给组成员（优先级介于全局与单元自身之间）。 |

---

## 10. `[[scheduler]]` VLink 原生组件调度器

将 CyberRT 级实时调度能力直接映射到 VLink 原生 `MessageLoop`、`MultiLoop` 与 `ThreadPool`。未声明时系统自动提供名为 `"default"` 的共享调度器。

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明与用法 |
| --- | --- | --- | --- | --- |
| `name` | - | string | 必填 | 调度器组名称，供 `[[component]].scheduler` 或 `[[component]].pool` 引用。 |
| `mode` | - | string | `"shared"` | 调度拓扑模式：<br>- `shared`：按 `threads` 创建固定数量的共享 `MessageLoop` 线程（默认 `1`）；<br>- `dedicated`：每个组件独占 1 个独立 `MessageLoop` 线程；<br>- `multi`：基于 `MultiLoop` 的多线程工作池（回调可能并发执行，组件须自保线程安全；同样生效 `max_elapsed_ms`）；<br>- `pinned`（别名 `choreography`）：按组件 `worker` 下标绑定到固定独立 `MessageLoop` 线程（确定性零竞争）；<br>- `pool`（别名 `thread_pool` / `threadpool`）：基于 VLink `ThreadPool`，专供组件 `ctx.post_compute_task()` 卸载耗时计算。 |
| `queue` | - | string | `"normal"` | 任务队列实现：`normal`（标准互斥队列）/ `lockfree`（无锁环形队列）/ `priority`（优先级堆队列；`mode = "pool"` 不支持 `priority`，会给出告警）。 |
| `strategy` | - | string | `"optimization"` | 队列满时背压策略：`optimization`（优化平衡）/ `pop`（丢弃最旧）/ `block`（阻塞等待）。 |
| `threads` | - | int | `1` | 工作线程数（`>= 1`，适用于 `shared` / `multi` / `pinned` / `pool` 模式）。 |
| `cpus` | `cpu_affinity` | int[] | `[]` | 工作线程绑核列表（按线程下标轮询或绑定对应 CPU 核）。 |
| `policy` | `sched_policy` | string | `"default"` | OS 线程调度策略：`default`（系统默认）/ `other`（`SCHED_OTHER`）/ `fifo`（`SCHED_FIFO`）/ `rr`（`SCHED_RR`）。 |
| `priority` | `thread_priority` | int | `0` | OS 线程调度优先级（`fifo`/`rr` 实时优先级数值）。 |
| `max_elapsed_ms` | - | int | `0` | 单次事件循环回调耗时监控阈值（毫秒，`0` 表示不限制），对应 `MessageLoop::get_max_elapsed_time()`。 |

---

## 11. `[[container]]` 独立隔离组件容器

容器本身由宿主派生独立子进程 `vlink-launcher _container` 运行，拥有独立的故障隔离边界、资源 cgroup/rlimit、日志插件实例以及子控制面；宿主 `ctl` 命令会自动把针对容器内组件的指令透明路由过去。

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明与用法 |
| --- | --- | --- | --- | --- |
| `name` | - | string | 必填 | 容器唯一名称（受 `namespace` 限定），供 `[[component]].container` 引用。 |
| `namespace` | - | string | 当前 `namespace` | 容器命名空间。 |
| `group` | - | string | `""` | 所属逻辑组。 |
| `stage` | - | int | `0` | 冷启动阶段号（数值越小越早启动，阶段之间存在全量屏障）。 |
| `condition` | - | bool / expr | `true` | 条件为假时跳过容器及其内部组件。 |
| `autostart` | - | bool | `true` | 是否随系统冷启动自动拉起。 |
| `required` | - | bool | `false` | `true` 时容器异常终止会触发整个系统安全停机。 |
| `delay_ms` | - | int | `0` | 启动前延迟毫秒数。 |
| `stop_timeout_ms` | `sigterm_timeout_ms` | int | `5000` | 停止等待超时（毫秒，最小 `200`）。 |
| `inherit_env` | - | bool | `true` | 是否继承宿主环境变量。 |
| `cwd` | - | string | `""` | 容器子进程工作目录。 |
| `prefix` | `launch_prefix` | string | `""` | 调试/追踪前缀命令（如 `"gdbserver :2345"`）。 |
| `output` | - | string | `"screen"` | 容器标准输出模式：`screen` / `log` / `both` / `none`。 |
| `kill_mode` | - | string | `"control-group"` | 停止范围：`control-group`（推荐，清扫整个进程树）/ `mixed` / `process`。 |
| `stop_signal` | - | string | `"SIGTERM"` | 优雅停止信号：`SIGTERM` / `SIGINT` / `SIGHUP` / `SIGKILL`。 |
| `on_exit` | - | string | `"ignore"` | 非预期退出后系统级动作：`ignore` / `shutdown` / `restart`。 |
| `record` | - | bool | 继承全局 | `true` 时为整个容器进程注入 `VLINK_BAG_PATH`（容器内全部组件共享单进程 BagWriter）；未设置或 `false` 时容器内各组件按自身 `[record]`/`record` 独立或动态录制。 |
| `record_path` | - | string | `""` | 容器级显式 Bag 路径（必须以 `.vcap`/`.vcapx`/`.vdb`/`.vdbx` 结尾，不可与其他进程碰撞）。 |
| `for_each` / `count` | - | array/str / int | - | 模板批量展开（`count` 范围 `[0, 256]`），展开 `${item}` 与 `${index}`。 |
| `[container.env]` / `[container.resources]` | - | table | `{}` | 容器环境变量与资源限制。 |
| 依赖、重启与触发器 | - | - | - | 支持第 16 节全部依赖、重启策略、退出码与触发器字段。 |

---

## 12. `[[component]]` 进程内托管组件

通过 VLink `Plugin` 系统动态加载 `LauncherModuleInterface`（或兼容旧版 `RunablePluginInterface`），在同一进程内实现零拷贝 `intra://` 通信、状态机热切（`Active`/`Inactive`）、参数热更新、原地重载与软件看门狗。

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明与用法 |
| --- | --- | --- | --- | --- |
| `name` | - | string | 必填 | 组件唯一名称（受 `namespace` 限定）。 |
| `namespace` | - | string | 当前 `namespace` | 组件命名空间（影响组件 `ctx.resolve_url()` 的相对主题前缀）。 |
| `group` | - | string | `""` | 所属逻辑任务组。 |
| `lib` | `library` | string | 必填 | 插件动态库基础名（不含 `lib`/`.so`/`.dll`）或文件路径。 |
| `class` | `class_name` | string | `""` | `VLINK_LAUNCHER_EXPORT_COMPONENT` 注册的组件类名（`legacy = false` 时填写）。 |
| `scheduler` | - | string | `"default"` | 绑定的 `[[scheduler]]` 名称（承载生命周期回调、定时器、订阅回调与协程）。 |
| `pool` | `compute_pool` | string | `""` | 可选计算线程池名称（通常指向 `mode = "pool"` 的 `[[scheduler]]`，供 `ctx.post_compute_task()` 使用）。 |
| `container` | - | string | `""` | 目标 `[[container]]` 名称；留空则直接在宿主进程内加载运行。 |
| `stage` | - | int | `0` | 冷启动阶段号。 |
| `worker` | `worker_index` | int / string | `-1` | `pinned`（`choreography`）调度模式下的目标工作线程下标（`-1` 自动轮询）；支持字符串模板如 `"${index}"`。 |
| `priority` | `task_priority` | int | `0` | 组件提交到 `priority` 调度队列的默认优先级 `[0, 65535]`。 |
| `watchdog_ms` | - | int | `0` | 软件看门狗超时（毫秒，`0` 关闭）；启用后组件须周期调用 `ctx.watchdog_ping()`，超时将触发故障恢复/重启（`legacy = true` 不支持）。 |
| `legacy` | `legacy_runnable` | bool | `false` | `true` 时按旧版 `RunablePluginInterface` 加载（不支持 `watchdog_ms`、`reload = "component"` 与 `init_state = "inactive"`）。 |
| `init_state` | `autostart_state` | string | `"active"` | 初始生命周期状态：`active`（配置并立即激活）或 `inactive`（仅执行 `on_configure`，处于热备待机，待 `ctl activate` 秒级激活）。 |
| `reload` | - | string | `"none"` | 原地重载模式：`none` 或 `component`（触发 `on_reload()` 回调；组件不可用 `sighup`）。 |
| `condition` / `autostart` / `required` / `delay_ms` | - | - | `true`/`true`/`false`/`0` | 与容器相同。 |
| `record` | - | bool | 继承全局 | 组件级主动录制开关；运行期可通过 `ctl record start/stop -u <name>` 动态热切换（包含 `inactive` 态组件）。 |
| `record_path` | - | string | `""` | 显式 Bag 文件路径（同进程多组件可共享同一路径；跨进程不可共享）。留空自动按 `<dir>/components/<name>[_g<gen>][_s<seg>].<fmt>` 分配。 |
| `record_force` | `record_force_publish` | bool | 继承 `[record].force` | 组件级强制序列化发布覆盖，作用于 `LauncherPublisher::publish()`。 |
| `for_each` / `count` | - | array/str / int | - | 模板批量展开。 |
| `[component.urls]` / `[component.inputs]` / `[component.outputs]` | - | table | `{}` | 声明组件端口与命名 URL；供 `ctx.create_publisher()`/`ctx.create_subscriber()`/`ctx.get_url()` 使用，同时自动推导系统数据流拓扑图并检查跨进程 `intra://` 误用。 |
| `[component.params]` / `params_file` / `params_files` | - | table / str | `{}` | 组件初始参数，优先级高于组与全局参数。 |
| `[component.remaps]` | - | table | `{}` | 组件专属 URL 重映射规则。 |
| `[component.ready]` / `[component.liveness]` | - | table | - | 就绪与探活探针，见第 16 节。 |
| 依赖、重启与触发器 | - | - | - | 支持第 16 节全部依赖、重启策略与触发器字段。 |

---

## 13. `[[process]]` 受管外部进程

用于托管任意外部可执行程序（C++、Python、Shell、第三方服务等），具备完整 systemd 级守护、退出码策略、启动限流熔断、探针、信号重载、资源限制与环境变量 URL 注入。

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明与用法 |
| --- | --- | --- | --- | --- |
| `name` | - | string | 必填 | 进程唯一名称（受 `namespace` 限定）。 |
| `namespace` | - | string | 当前 `namespace` | 命名空间（同时影响 `[process.inputs]`/`outputs`/`urls` 相对路径解析）。 |
| `group` | - | string | `""` | 所属逻辑任务组。 |
| `stage` | - | int | `0` | 冷启动阶段号。 |
| `cmd` | `program` | string | 必填 | 可执行文件路径或 `PATH` 中的命令名。 |
| `args` | - | string[] | `[]` | 命令行参数数组（支持 `${...}` 插值）。 |
| `prefix` | `launch_prefix` | string | `""` | 命令包装前缀（按 shell 规则拆分并置于 `cmd` 前，如 `"valgrind --leak-check=full"` 或 `"taskset -c 2"`）。 |
| `cwd` | - | string | `""` | 子进程工作目录。 |
| `inherit_env` | - | bool | `true` | 是否继承宿主进程环境变量（`false` 时仅注入 Launcher 构造的最小环境）。 |
| `condition` / `autostart` / `required` / `delay_ms` | - | - | `true`/`true`/`false`/`0` | 条件开关、自动启动、强关键单元标记、启动前延时。 |
| `stop_timeout_ms` | `sigterm_timeout_ms` | int | `5000` | 发送停止信号后等待优雅退出的超时时间（毫秒，最小 `200`），超时后升级强杀。 |
| `output` | - | string | `"screen"` | 标准输出/错误去向：`screen`（终端）/ `log`（文件/插件）/ `both` / `none`。 |
| `kill_mode` | - | string | `"process"` | 终止范围：`process`（仅主进程）/ `control-group`（整组进程树）/ `mixed`（先主进程后清扫子进程树）。 |
| `stop_signal` | - | string | `"SIGTERM"` | 优雅停止信号：`SIGTERM` / `SIGINT` / `SIGHUP` / `SIGKILL`。 |
| `reload` | - | string | `"none"` | 原地重载方式：`none` 或 `sighup`（外部进程不可用 `component`）。 |
| `on_exit` | - | string | `"ignore"` | 进程终止后的系统级行为：`ignore` / `shutdown`（触发整个 Launcher 安全停机） / `restart`。 |
| `record` | - | bool | 继承全局 | 进程级主动录制开关；开启时自动注入按代次隔离的 `VLINK_BAG_PATH`（重启第 `g` 代自动加 `_g<g>` 后缀防止覆盖旧 Bag）。 |
| `record_path` | - | string | `""` | 进程显式 Bag 文件路径（必须为 `.vcap`/`.vcapx`/`.vdb`/`.vdbx`，不可与其他进程冲突）。 |
| `for_each` / `count` | - | array/str / int | - | 模板批量展开。 |
| `[process.urls]` / `[process.inputs]` / `[process.outputs]` | - | table | `{}` | 自动推导数据流图并向子进程注入 `VLINK_URL_<KEY>`、`VLINK_INPUT_<KEY>`、`VLINK_OUTPUT_<KEY>` 环境变量。 |
| `[process.env]` / `[process.remaps]` / `[process.resources]` | - | table | `{}` | 进程级环境变量、URL 重映射（自动写入临时 JSON 注入 `VLINK_URL_REMAP`）与资源限制。 |
| `[process.ready]` / `[process.liveness]` | - | table | - | 就绪与探活探针，见第 16 节。 |
| 依赖、重启与触发器 | - | - | - | 支持第 16 节全部依赖、重启策略、退出码与触发器字段。 |

---

## 14. `[[task]]` 一次性与周期性任务

适用于初始化脚本、硬件自检、数据库迁移（`oneshot`）或定时巡检、日志清理（`periodic`）。

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明与用法 |
| --- | --- | --- | --- | --- |
| `name` / `namespace` / `group` / `stage` | - | - | 必填/`""`/`""`/`0` | 基本标识、命名空间、所属组与启动阶段。 |
| `kind` | - | string | `"oneshot"` | 任务类型：`oneshot`（运行至结束，成功退出即满足下游依赖并转 `Stopped`）或 `periodic`（按 `interval_ms` 周期拉起）。 |
| `cmd` | `program` | string | 必填 | 任务可执行程序路径或命令名。 |
| `args` / `prefix` / `cwd` / `inherit_env` | - | - | `[]`/`""`/`""`/`true` | 与 `[[process]]` 相同。 |
| `condition` / `autostart` / `required` / `delay_ms` | - | - | `true`/`true`/`false`/`0` | 与 `[[process]]` 相同。 |
| `interval_ms` | - | int | `5000` | `kind = "periodic"` 时的触发间隔（毫秒，最小 `50`）。 |
| `timeout_ms` | - | int | `10000` | 单次执行超时上限（毫秒，最小 `100`）；超时自动终止并记为失败。 |
| `ok_codes` | `success_exit_codes` | int[] | `[0]` | 视为任务执行成功的退出码列表（`0..255`，如 `[0, 7]`）。 |
| `output` / `kill_mode` | - | string | `"screen"` / `"process"` | 输出与终止范围模式。 |
| `for_each` / `count` | - | array/str / int | - | 模板批量展开。 |
| `[task.urls]` / `inputs` / `outputs` / `env` / `remaps` / `resources` | - | table | `{}` | 端点环境变量、重映射与资源限制。 |
| 依赖与触发器 | - | - | - | 支持第 16 节全部依赖与触发器字段（任务不支持 `restart` 策略）。 |

---

## 15. `[[include]]` 多文件组合包含

用于将大型系统拆分为按子系统维护的多个 TOML 清单，具备最大 16 层递归保护与循环包含检测。

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明与用法 |
| --- | --- | --- | --- | --- |
| `path` | - | string | 必填 | 被包含 TOML 文件路径（相对路径基于当前文件 `this_dir`）。 |
| `namespace` | - | string | `""` | 为被包含文件附加的子命名空间（自动拼接在当前 `namespace` 之后）。 |
| `condition` | - | bool / expr | `true` | 为假时跳过整个文件包含。 |
| `for_each` / `count` | - | array/str / int | - | 批量多次包含同一模板文件（每次展开不同的 `${item}`/`${index}` 与 `namespace`）。 |
| `[include.args]` | - | table | `{}` | 传递给被包含文件的参数覆盖表（仅作用于该子文件及其后代）。 |

---

## 16. 通用依赖、探针、重启与触发器子表

### 16.1 依赖与排序（systemd 正交语义）

所有依赖数组元素均支持单元名或 `@group` 组引用：

| 参数名 | 语义类型 | 详细说明 |
| --- | --- | --- |
| `depends_on` | 语法糖（排序 + 强依赖） | 等价于同时声明 `after` 与 `requires`：等待目标就绪后启动，目标失败则本单元跳过。 |
| `after` | 纯启动顺序 | 本单元在目标达到就绪/完成状态之后才启动（不自动拉起目标）。 |
| `before` | 纯启动顺序 | 目标在本单元就绪之后才启动（与 `after` 对称）。 |
| `requires` | 强依赖 | 手动启动本单元时自动拉起目标；目标失败则本单元不启动。 |
| `wants` | 弱依赖 | 尽量拉起目标，但目标失败不阻塞本单元启动。 |
| `binds_to` | 强绑定生命周期 | 在 `requires` 基础上，若目标运行中意外退出或被停止，本单元立即级联停止。 |
| `part_of` | 伴随停止/重启 | 当目标停止或重启时，本单元同步停止或重启（不影响目标启动）。 |
| `conflicts` | 互斥冲突 | 两个单元不可同时自启（`validate` 直接报错）；运行期启动其中一方会自动停止冲突方。 |

### 16.2 重启策略与熔断限流（适用于 `process` / `container` / `component` / `proxy`）

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明 |
| --- | --- | --- | --- | --- |
| `restart` | `restart_policy` / `respawn_policy` | string / bool | `"no"` | 故障重启策略：`no`（别名 `never`）/ `on_failure`（非 `ok_codes` 退出重启）/ `on_abnormal`（信号/超时/看门狗异常重启）/ `always`（除显式停止和 `no_restart_codes` 外始终重启）。传布尔 `true` 等价于 `"on_failure"`，`false` 等价于 `"no"`（兼容旧键 `respawn = true`）。 |
| `restart_delay_ms` | `respawn_delay_ms` | int | `1000` | 首次崩溃后的重启等待延迟（毫秒）。 |
| `restart_max_ms` | `respawn_max_delay_ms` | int | `30000` | 指数退避重启延迟上限（毫秒）。 |
| `restart_backoff` | `respawn_backoff` | float | `1.5` | 连续失败后的退避乘子（`>= 1.0`）。 |
| `start_burst` | `start_limit_burst` | int | `5` | 统计窗口内允许的最大连续启动次数；超过即触发 `start_limit` 熔断并进入 `Failed` 状态（`0` 表示不限制）。不可与旧键 `max_restarts`/`restart_window_ms` 混用。 |
| `start_window_ms` | `start_limit_interval_ms` | int | `60000` | 启动限流单调固定统计窗口（毫秒）。 |
| `ok_codes` | `success_exit_codes` | int[] | `[0]` | 视为正常退出的退出码列表（`0..255`）。 |
| `no_restart_codes` | `restart_prevent_exit_codes` | int[] | `[]` | 命中后即使策略为 `always`/`on_failure` 也禁止自动重启的退出码列表（`0..255`）。 |

### 16.3 就绪探针 `[*.ready]` 与探活探针 `[*.liveness]`

| 子表 | 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明 |
| --- | --- | --- | --- | --- | --- |
| `[*.ready]` | `type` | - | string | `"none"` | 探针类型：`none`（立即就绪）/ `component`（等待组件调 `ctx.mark_ready()`）/ `delay`（延时 `delay_ms`）/ `stdout`（匹配输出 `pattern`）/ `file`（别名 `device`，等待 `path` 文件/设备节点出现）/ `topic`（等待 VLink 服务发现出现 `topic`）。 |
| `[*.ready]` | `pattern` | - | string | `""` | `type = "stdout"` 时的子串匹配模式。 |
| `[*.ready]` | `path` | - | string | `""` | `type = "file"` 时的目标文件或设备路径。 |
| `[*.ready]` | `topic` | - | string | `""` | `type = "topic"` 时的目标 VLink URL（支持命名空间与 remap 解析）。 |
| `[*.ready]` | `delay_ms` | - | int | `0` | `type = "delay"` 时的等待毫秒数。 |
| `[*.ready]` | `timeout_ms` | - | int | `10000` | 就绪等待超时上限（毫秒，最小 `100`）；超时标记单元失败。 |
| `[*.liveness]` | `enabled` | - | bool | `true`（当 `type != "none"`） | 可选显式开关；设为 `false` 时禁用该探活配置。 |
| `[*.liveness]` | `type` | - | string | `"none"` | 持续探活类型：`none` / `file` / `topic`。 |
| `[*.liveness]` | `path` / `topic` | - | string | `""` | 探活检查的文件路径或 VLink 主题 URL。 |
| `[*.liveness]` | `interval_ms` | - | int | `5000` | 探活巡检周期（毫秒，最小 `100`）。 |
| `[*.liveness]` | `failure_threshold` | `max_fails` | int | `3` | 连续失败达到此次数后判定不健康（最小 `1`）。 |
| `[*.liveness]` | `restart_on_failure` | - | bool | `true` | 探活失败时是否自动触发单元重启。 |

### 16.4 生命周期事件触发器

| 参数名 | 兼容别名 | 类型 | 默认值 | 详细说明 |
| --- | --- | --- | --- | --- |
| `on_start` | `on_start_trigger` | string[] | `[]` | 本单元进入启动阶段时，自动触发启动的单元名或 `@group` 列表。 |
| `on_ready` | `on_ready_trigger` | string[] | `[]` | 本单元达到就绪状态时，自动触发启动的单元名或 `@group` 列表。 |
| `on_exit_trigger` | - | string[] | `[]` | 本单元退出时，自动触发启动的清理/善后单元名或 `@group` 列表（与动作字段 `on_exit` 区分）。 |
| `on_error` | `on_error_trigger` | string[] | `[]` | 本单元进入 `Failed` 状态时，自动触发启动的故障处置/告警单元名或 `@group` 列表。 |
