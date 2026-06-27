# ⌨️ 10. CLI 工具

VLink 在通信原语之外随库交付一套完整的命令行工具链（`vlink-*` 前缀），用于在不修改业务代码的前提下对运行中的系统进行发现、监控、录制、调试与性能评估。该层最轻量、可脚本化、零图形依赖，适合终端排障与 CI 对接。需要本地图形化预览图像、点云、目标检测，或经浏览器远程协作的可视化能力，见 [可视化工具](11-visualization.md)。

命令行工具与桌面、Web 可视化层共享同一套底层观测设施：服务发现枚举活跃话题，代理层（ProxyAPI / `vlink-proxy`）聚合话题、序列化类型与统计信息。因此工具对传输后端（`intra://` / `shm://` / `dds://` 等）一律透明——业务侧更换后端只改 URL 前缀，工具侧无需任何改动。

![CLI 工具生态](images/cli-tools-overview.png)

---

## 🧭 10.1 工具链总览与选型

整条工具链按"观测目标 + 呈现形态"两维划分职责，互不重叠：命令行层负责文本与统计、可脚本化；桌面层负责本地图形化、强交互；Web 层负责跨平台/远程可视化与离线归档。

| 层次 | 工具 | 职责 |
| --- | --- | --- |
| 命令行 | `vlink-info` | 查看版本号、构建时间戳与编译选项 |
| 命令行 | `vlink-check` | 环境自检：网络/内核诊断、环境变量核对、通信冒烟测试 |
| 命令行 | `vlink-list` | 列出当前活跃的节点与话题 |
| 命令行 | `vlink-monitor` | 实时监控话题的频率、速率、丢包率与时延 |
| 命令行 | `vlink-bag` | 录制、回放与运维消息数据包 |
| 命令行 | `vlink-dump` | 从话题或 bag 提取字段，导出 CSV/JSON/图像/点云 |
| 命令行 | `vlink-eproto` / `vlink-efbs` | 订阅/发布 Protobuf / FlatBuffers 消息并在终端解析显示 |
| 命令行 | `vlink-bench` | 发布/订阅性能基准测试与报告 |
| 桌面 | `vlink-viewer` | 实时监控全部活跃 URL，预览相机图像、点云、目标检测等结构化数据 |
| 桌面 | `vlink-player` | 图形化回放 bag 文件，支持速率、进度、过滤与 URL 重映射 |
| 桌面 | `vlink-analyzer` | 从 bag 中提取字段，绘制时间序列波形并导出 |
| Web | `vlink-foxglove` | 将实时数据桥接至 Foxglove Studio（WebSocket） |
| Web | `vlink-rerun` | 将实时数据桥接至 Rerun Viewer（gRPC） |
| Web | `vlink-bag2mcap` / `vlink-bag2rrd` | 将 bag 离线转为 MCAP / RRD 可视化文件 |

桌面与 Web 工具的用法见 [可视化工具](11-visualization.md)；其公共数据来源 `vlink-proxy` 的控制面机制详见 [可观测性](12-observability.md)。

**层间选型判据**　以下为跨层（命令行 / 桌面 / Web）的选型依据；命令行层内部各工具的任务映射见 §10.2.2。

| 需求 | 选择 |
| --- | --- |
| 仅需话题列表、频率、原始字节，或需脚本/CI 对接 | 命令行（`vlink-list` / `vlink-monitor` / `vlink-dump`） |
| 需本地图形化预览图像、点云、目标检测，强交互 | 桌面 `vlink-viewer` 套件 |
| 需浏览器访问、远程协作、团队共享或 CI 集成 | Web `vlink-foxglove` |
| 需本机高性能三维渲染与多模态融合 | Web `vlink-rerun` |
| 需将历史 bag 转为通用可视化文件离线分发 | `vlink-bag2mcap` / `vlink-bag2rrd` |

---

## 🖥️ 10.2 命令行工具

全部命令行工具遵循一致的接口约定：均支持 `-h` / `--help` 与 `-v` / `--version`；除本地诊断类命令外，多数工具默认经服务发现感知远端节点，因此跨机使用要求组播/广播在链路上可达。是否将某工具编入由对应的 `ENABLE_CLI_*` 构建选项控制。

### 📦 10.2.1 安装与通用约定

编译安装后工具位于 `<install_prefix>/bin/`，将该目录加入 `PATH` 即可全局调用：

```bash
cmake -B build
cmake --build build
cmake --install build --prefix /usr/local
export PATH=/usr/local/bin:$PATH
```

通用约定：

- 默认安装短别名软链（`list`、`monitor`、`bag`、`eproto` 等价于对应 `vlink-*`）；构建时设 `-DENABLE_SYMLINKS=OFF` 仅安装全名，以规避与发行版命令冲突。
- 首次部署建议先执行 `vlink-check diag` 自检环境（见 §10.2.4）。

### 🗺️ 10.2.2 任务到工具的映射

工具按观测目标划分职责，下表给出从工程任务到工具的选用判据。

| 任务 | 工具 |
| --- | --- |
| 确认运行环境是否就绪 | `vlink-check diag` |
| 枚举当前节点与话题 | `vlink-list` |
| 量化话题频率与丢包 | `vlink-monitor` |
| 检视话题消息内容 | `vlink-eproto sub` / `vlink-efbs sub` |
| 录制与回放数据 | `vlink-bag record` / `vlink-bag play` |
| 导出字段供离线分析 | `vlink-dump` |
| 评估链路吞吐与时延 | `vlink-bench run` |

最高频的三条工作流：

```bash
vlink-list

vlink-monitor -l -i camera

vlink-eproto sub dds://sensor/imu -d /home/protos/
```

### ℹ️ 10.2.3 vlink-info：版本与编译选项

`vlink-info` 输出当前二进制的版本号、构建时间戳、Git 标签与提交 ID；附加 `-l` 时列出该二进制启用的全部编译选项。该信息用于确认现场部署的版本与功能集是否符合预期。

```bash
vlink-info

vlink-info -l
```

输出示例：

```
┌──────── VLink Informations ────────────────────────────────────────────────────
│ Version:                     2.0.0
│ Time stamp:                  2026-03-17 10:00:00
│ Git tag:                     v2.0.0
│ Git commit-id:               e78d342
└────────────────────────────────────────────────────────────────────────────────
```

### 🩺 10.2.4 vlink-check：环境自检

`vlink-check` 在部署或排障前确认环境是否满足通信前置条件，含三个子命令，分别面向系统诊断、配置核对与通信验证。

| 子命令 | 作用 |
| --- | --- |
| `vlink-check diag` | 逐项检查 IP/网卡/内核参数/文件系统/运行时进程，给出 PASSED / WARNING / FAILED |
| `vlink-check env` | 列出常用 `VLINK_*` 环境变量及当前取值（按编译进的传输模块裁剪） |
| `vlink-check test` | 执行通信冒烟测试，验证三种通信模型与各后端能否收发 |

`diag` 遍历一组诊断项（VLink 版本、可用 IP、组播路由、内核网络缓冲、文件描述符上限、`/dev/shm` 空间、时间同步、CPU/内存占用、相关工具是否在运行等），以彩色状态栏汇报，并将退出码置为失败项数量，全部通过返回 0，因而可直接用于脚本判定。

```bash
vlink-check diag

vlink-check diag -a -s

vlink-check diag -f dds
```

| 参数 | 说明 |
| --- | --- |
| `-a` / `--all` | 附加检查所有编译选项开关状态 |
| `-s` / `--summary` | 结束时打印 PASSED / WARNING / FAILED 统计 |
| `-f` / `--filter <substring>` | 仅执行标题包含指定子串的诊断项 |

`env` 列出常用环境变量子集（路径/插件、日志、bag/发现、各传输后端、TLS 等），已设置项以绿色、未设置项以红色标识，用于快速核对配置一致性；它仅覆盖常用子集，完整清单以 [集成与环境变量](13-integration.md) 为准。

```bash
vlink-check env

vlink-check env -b

vlink-check env -p VLINK_ZENOH_
```

`test` 先在 `intra://` 上对 Event / Method / Field 三种模型做最小自检，再对每个编译进的传输后端执行一次通信往返。缺少运行时前置条件的后端标 WARNING 跳过，不影响其它项；退出码等于失败项数量。

```bash
vlink-check test
```

### 📋 10.2.5 vlink-list：枚举活跃节点与话题

`vlink-list` 扫描网络上活跃的 VLink 进程，按进程分组以树状结构列出其注册的话题（Publisher / Subscriber / Server / Client / Setter / Getter）及序列化类型。它在启动后等待约 1 秒收集发现信息，随后输出并退出。发现机制详见 [可观测性](12-observability.md)。

```bash
vlink-list

vlink-list -n

vlink-list -m my_process
vlink-list -p 1234

vlink-list -c
echo "Process count: $?"
```

| 参数 | 说明 |
| --- | --- |
| `-n` / `--native` | 本地模式，仅发现本机节点 |
| `-m` / `--name <name>` | 按进程名过滤 |
| `-p` / `--pid <pid>` | 按进程 ID 过滤 |
| `-c` / `--check_process_count` | 仅以退出码返回进程数，不输出文本（用于脚本判定） |

输出示例：

```
camera_node (pid: 1001, host: myhost, ip: 192.168.1.10)
  Publisher:
    dds://camera/image       CameraFrame
    dds://camera/pointcloud  PointCloud

detection_node (pid: 1002, host: myhost, ip: 192.168.1.10)
  Subscriber:
    dds://camera/image       CameraFrame
  Publisher:
    dds://detection/result   pb.DetectResult
```

### 📊 10.2.6 vlink-monitor：实时通信监控

`vlink-monitor` 是终端交互式监控工具，持续显示网络中各话题的频率、速率、丢包率与时延，并可叠加 Sparkline 图表面板与进程面板。它解决"系统是否在按预期速率投递、是否存在丢包或时延异常"的观测问题。

两项核心交互能力：

- **按 `I` 弹出过滤框**：在屏幕中央弹出悬浮输入框，每输入一个字符即实时过滤话题列表，命中字符以加粗与下划线高亮；支持空格/逗号分隔多关键字、大小写不敏感子串匹配。命令行 `-i` / `--filter` 与之共用同一套过滤逻辑。
- **选中话题后按 `Enter` 跳转**：工具按服务发现推断的序列化类型自动启动 `vlink-eproto sub` 或 `vlink-efbs sub` 检视该话题内容；若话题属于字段模型，则自动以 Getter 接收。启动时附 `-b` / `--blob` 时，跳转后强制以十六进制显示。

```bash
vlink-monitor

vlink-monitor -x

vlink-monitor -lo

vlink-monitor -i camera

vlink-monitor -i debug -k

vlink-monitor -u dds://camera/image dds://lidar/points

vlink-monitor --plain > monitor_output.txt
```

| 参数 | 说明 |
| --- | --- |
| `-l` / `--detail` | 详情模式，显示频率/速率/丢包/时延列（热键 `L`） |
| `-o` / `--observe_all` | 为所有行订阅数据（配合 `-l`，热键 `O`） |
| `-t` / `--node_count` | 节点计数模式（热键 `T`） |
| `-e` / `--profiler` | 显示 profiler 面板（热键 `E`） |
| `-s` / `--ser` | 显示序列化类型（热键 `S`） |
| `-a` / `--active` | 仅显示活跃行（配合 `-l`，热键 `A`） |
| `-y` / `--pubsub` | 仅显示 pub/sub（热键 `Y`） |
| `-i` / `--filter <str>` | URL 关键字过滤（运行时可按 `I` 编辑） |
| `-k` / `--black` | 黑名单模式，剔除命中的 URL |
| `-u` / `--urls <url...>` | 仅监控指定 URL |
| `-x` / `--preset` | 常用组合预设，等价 `-l -o -p -c` |
| `-p` / `--process` | 进程面板（热键 `P`） |
| `-c` / `--chart` | Sparkline 图表面板（热键 `C`） |
| `-n` / `--native` | 本地模式 |
| `-b` / `--blob` | Enter 跳转时强制以十六进制显示 |
| `-g` / `--proto_args <str>` | Enter 跳转检视时追加的 eproto/efbs 参数 |
| `-d` / `--proto_dir <dir>` | Proto 目录（默认取环境变量 `VLINK_PROTO_DIR`） |
| `-f` / `--fbs_dir <dir>` | Flatbuffers 目录（默认取环境变量 `VLINK_FBS_DIR`） |
| `--dot` | 图表使用点状字符绘制 |
| `--rows <n>` | 最大行数，`0` 表示自动（默认 `0`） |
| `--columns <n>` | 最大列数，`0` 表示自动（默认 `0`） |
| `--chart_width <n>` | 图表宽度（默认 `30`，范围 10 - 100） |
| `--process_width <n>` | 进程列宽度（默认 `40`，范围 20 - 100） |
| `--plain` | 纯文本输出，禁用交互（用于重定向） |

常用热键：`q` / `Esc` 退出，`Space` 暂停/恢复，`I` 过滤框，`Enter` 跳转检视，`Z` 清除选中行，`L` / `O` / `T` / `E` / `S` / `A` / `Y` / `P` / `C` 切换各显示模式，方向键翻页与移动选中行。

行颜色语义：绿色表示持续有数据且统计稳定，黄色表示启动或过渡态，红色表示约 2 秒以上无新数据。

### 🎞️ 10.2.7 vlink-bag：录制与回放

`vlink-bag` 对话题进行录制与回放，并提供克隆、校验、重建索引、修复、打标签等运维子命令。录制/回放的 C++ API、文件格式（`.vdb` / `.vdbx` / `.vcap` / `.vcapx`）与整体架构详见 [录制与回放](09-recording.md)。

![录制与回放数据流](images/bag-record-playback-flow.png)

最高频的三条命令为 `record`、`play`、`info`：

```bash
vlink-bag record /tmp/test.vdb

vlink-bag record /tmp/test.vdb -u dds://camera/image dds://lidar/points -d 60 -p

vlink-bag play /tmp/test.vdb -r 2.0 -t 3

vlink-bag play /tmp/test.vdb -b 10 -e 60

vlink-bag info /tmp/test.vdb
```

`record` 常用参数（完整列表见 `--help`）：

| 参数 | 说明 |
| --- | --- |
| `path` | 输出文件路径（必填） |
| `-u` / `--urls <url...>` | 仅录制指定 URL（空为全部） |
| `-i` / `--filter <str>` / `-k` / `--black` | URL 关键字过滤 / 黑名单 |
| `-d` / `--duration <s>` | 录制时长（秒），≤0 不限制 |
| `-p` / `--compress` | 启用压缩 |
| `-f` / `--force` | 覆盖已有文件 |
| `-t` / `--tag <name>` | 录制标签名 |
| `-z` / `--split_by_size <GB>` / `-y` / `--split_by_time <s>` | 按大小/时间分割文件 |

录制时 `Space` 暂停/恢复，`q` / `Esc` 停止。

`play` 常用参数：

| 参数 | 说明 |
| --- | --- |
| `path` | bag 文件路径（必填） |
| `-u` / `--urls <url...>` | 仅回放指定 URL |
| `-r` / `--rate <f>` | 回放速率（0.01~100） |
| `-t` / `--times <n>` | 回放次数，≤0 无限循环 |
| `-b` / `--begin_time <s>` / `-e` / `--end_time <s>` | 回放起止相对时间（秒） |
| `-m` / `--skip_blank` | 跳过空白段 |

回放时 `Space` 暂停/恢复，方向键前后跳转（`Left` / `Right` 1 秒、`Up` / `Down` 5 秒），暂停态下 `p` 单步前进一帧。

运维子命令：

| 子命令 | 作用 |
| --- | --- |
| `vlink-bag clone <src> <dst>` | 克隆/转换：支持格式转换、话题过滤、时间裁剪、压缩转换 |
| `vlink-bag check <path>` | 校验文件完整性（0 正常，-1 异常） |
| `vlink-bag reindex <path>` | 重建时间索引 |
| `vlink-bag fix <path>` | 修复未完整写入的文件（如录制中途断电） |
| `vlink-bag tag <path> <name>` | 设置/修改标签名 |

```bash
vlink-bag clone /tmp/test.vdb /tmp/clipped.vdb -b 10 -e 60 -p

vlink-bag clone /tmp/capture.vcap /tmp/result.vdb

vlink-bag fix /tmp/broken.vdb

vlink-bag tag /tmp/test.vdb "highway_test_20260317"
```

### 📤 10.2.8 vlink-dump：字段提取与转储

`vlink-dump` 从实时话题或 bag 文件中提取指定字段，输出为 CSV、JSON、原始二进制、图像/视频或点云，并支持对离线 bag 做切片（`slice`）与扫描（`scan`）。它面向"将通信数据转为可离线分析的结构化产物"这一任务。

提取 Protobuf 字段需经 `-d` 或 `VLINK_PROTO_DIR` 指定 `.proto` 目录，FlatBuffers 经 `--fbs_dir` 或 `VLINK_FBS_DIR` 指定 `.fbs` 目录；零拷贝类型（CameraFrame、PointCloud、Tensor、OccupancyGrid 等）的内置字段无需额外配置，字段定义见 [零拷贝](06-zerocopy.md)。表达式功能需构建时启用 `ENABLE_EXPRTK=ON`。

基础参数：

| 参数 | 说明 |
| --- | --- |
| `url` | 目标话题 URL（dump/导出模式必填具体 URL，不可省；`slice` / `scan` 可省，默认 `*` 匹配全部话题） |
| `-t` / `--type <type>` | 输出类型：`console`（别名 `text`）/`csv`/`json`/`bin`/`jpg`（别名 `jpeg`）/`h264`/`h265`/`raw`/`pcd`/`slice`/`scan`（默认 `csv`） |
| `-c` / `--condition <fields>` | 提取字段，逗号/空格分隔（CSV/JSON 必填），如 `header.seq,pose.x` |
| `-f` / `--bag_file <path>` | 从 bag 提取（缺省则从实时通信） |
| `-b` / `--begin_time` / `-e` / `--end_time` | 时间范围（秒，仅对 `-f` 有效） |
| `-n` / `--count <n>` / `--hz <hz>` | 最大样本数 / 最大输出频率 |
| `-o` / `--out_dir` / `-m` / `--base_name` | 输出目录 / 文件基础名 |
| `-d` / `--proto_dir` / `--fbs_dir` | schema 目录 |
| `-x` / `--expression <expr>` | 表达式，可重复（配合 `-c`，需 exprtk） |

输出类型语义：`console` 在终端打印消息内容；`csv` / `json` 输出选定字段；`bin` 将每条消息的原始字节存为独立文件；`jpg` / `h264` / `h265` / `raw` 适用于 CameraFrame 或含 bytes 字段的消息；`pcd` 将零拷贝 PointCloud 每帧存为 PCD 文件。`slice` / `scan` 为离线 bag 的高级用法（按窗口/事件切片、按事件或质量扫描），参数较多，详见 `vlink-dump --help`。

字段路径写法：Protobuf 使用点路径（`header.seq`、`status.velocity.x`）；零拷贝类型使用其内置字段名（如 CameraFrame 的 `width` / `height` / `data`，PointCloud 的 `size` / `data[N].field`），完整清单见 [零拷贝](06-zerocopy.md)。

```bash
vlink-dump dds://sensor/imu -t csv -c "header.seq" -o /tmp/ -d /home/protos/

vlink-dump dds://vehicle/state -t csv -c "velocity.x" \
  -f /tmp/test.vdb -b 10 -e 60 -o /tmp/ -d /home/protos/

vlink-dump dds://test -t console -n 5

vlink-dump dds://camera/front -t jpg -c data -o /tmp/frames/ -d /home/protos/

vlink-dump dds://lidar -t pcd -f /tmp/bag.vdb -d /home/protos/

vlink-dump dds://test -c "pose.x,pose.y" \
  -x "sqrt(pose_x*pose_x+pose_y*pose_y)" -t csv --hz 10

vlink-dump -t slice -f /tmp/test.vdb -w 30 -o /tmp/slices

vlink-dump -t scan -f /tmp/test.vdb -c "brake" --event "brake>80" -d /home/protos/ -o /tmp/scan
```

### 🔍 10.2.9 vlink-eproto / vlink-efbs：消息调试

`vlink-eproto`（Protobuf）与 `vlink-efbs`（FlatBuffers）用于在终端检视与发布消息，解决"某话题实际承载的内容是什么"的问题。两者结构一致，均含 `sub`（订阅显示）、`pub`（发布）、`import`（持久化 schema 目录）三个子命令。下文以 `vlink-eproto` 为基准，`vlink-efbs` 的差异在末尾说明。

最常用的是 `sub <url>`：订阅并打印消息内容，序列化类型由服务发现自动推断。

```bash
vlink-eproto sub dds://sensor/imu -d /home/protos/

vlink-eproto sub dds://sensor/imu -d /home/protos/ -s pb.ImuData

vlink-eproto sub dds://vehicle/state -d /home/protos/ -s pb.VehicleState -g

vlink-eproto sub dds://sensor/imu -d /home/protos/ -s pb.ImuData -i angular_velocity

vlink-eproto sub dds://sensor/imu -d /home/protos/ -s pb.ImuData -j
```

`sub` 常用参数：

| 参数 | 说明 |
| --- | --- |
| `url` | 目标 URL（必填） |
| `-d` / `--proto_dir <dir>` | `.proto` 目录（默认读 `VLINK_PROTO_DIR`） |
| `-s` / `--ser_type <type>` | 显式指定消息名（指定后不等服务发现） |
| `-x` / `--encoding <type>` | 编码提示：`protobuf`/`flatbuffers`/`raw`/`blob`/`zerocopy`（`blob` 以十六进制显示） |
| `-i` / `--filter <str>` / `-k` / `--black` | 字段名过滤 / 黑名单 |
| `-j` / `--json` | 以 JSON 格式输出 |
| `-g` / `--getter` | 强制以 Getter 接收（字段模型） |
| `-n` / `--native` | 本地模式 |

未指定 `-s` 时，工具等待一轮服务发现自动推断类型，并在话题属于字段模型时自动切到 Getter 接收。标题行实时显示 URL、活动指示点与帧率，颜色策略与 `vlink-monitor` 一致。交互热键：`q` / `Esc` 退出、`Space` 暂停、方向键翻页，以及 `E` / `R` / `T` / `Y` / `U` / `O` / `P` 切换枚举/数组/字符串/时间/十六进制/默认值/repeated 等显示选项。

`pub <url>` 用于发布消息，内容可经 `-c` 直接给出或 `-f` 从文件读取：

```bash
vlink-eproto pub shm://control/cmd -d /home/protos/ -s pb.ControlCmd \
  -c "speed:10.0 steering:0.5"

vlink-eproto pub shm://control/cmd -d /home/protos/ -s pb.ControlCmd -j \
  -c '{"speed":10.0,"steering":0.5}'

vlink-eproto pub dds://test/msg -d /home/protos/ -s pb.TestMsg \
  -f /tmp/test.prototxt -t 0 -l 500
```

`pub` 常用参数：`-s` 消息名（省略时经服务发现自动推断）、`-c` / `-f` 消息内容/文件（二者必择其一）、`-j` 按 JSON 解析、`-t` 发布次数（≤0 无限）、`-l` 发布间隔（毫秒，默认 100）。

`import <dir>` 将 schema 目录持久化，之后任意 shell 中无需再传 `-d` 或设环境变量：

```bash
vlink-eproto import /home/protos
vlink-eproto sub dds://sensor/imu -s pb.ImuData
```

**`vlink-efbs` 差异**：针对 FlatBuffers，schema 目录用 `-d` / `--fbs_dir`（或 `VLINK_FBS_DIR`），消息类型为 FlatBuffers table 名；`pub` 的内容/文件参数为 `-c` / `--fbstxt_content` 与 `-f` / `--fbstxt_file`，输入按 FlatBuffers JSON 语法解析，显示默认即为 JSON 风格，故不提供 `-j` 开关。其余 `sub` / `pub` / `import` 用法、热键与参数含义与 `vlink-eproto` 一致。

```bash
vlink-efbs sub dds://sensor/scan -d /home/fbs_schemas/ -s MyScan

vlink-efbs pub shm://control/cmd --fbs_dir /home/fbs_schemas/ -s CtrlCmd \
  -c '{ speed: 10.0, mode: 1 }'
```

### ⚡ 10.2.10 vlink-bench：性能基准

`vlink-bench` 围绕不同 URL、通信模式、拓扑、QoS、payload 与报文大小执行矩阵化的发布/订阅性能测试，输出吞吐与时延报告。核心子命令为 `run`（执行测试）与 `plot`（从已有结果重建报告）。

不带 `--preset` 直接执行 `vlink-bench run` 时，运行一套约 2 分钟完成的 showcase 默认矩阵：覆盖 `throughput` 与 `latency` 两个套件、当前编译可用的跨进程传输、`bytes` payload，默认输出 HTML 报告与终端交互表格。

```bash
vlink-bench run

vlink-bench run \
  -u shm://bench/custom \
  -q sensor \
  --size 64,256,1024 \
  --rate 1000,10000 \
  --report terminal

vlink-bench run --preset full --report html,csv,json -o /tmp/bench-full

vlink-bench run --preset quick --report json --no-pager -o /tmp/ci-bench

vlink-bench plot /tmp/bench-full.json --report html,terminal
```

`run` 常用参数：

| 参数 | 说明 |
| --- | --- |
| `-p` / `--preset` | 预设矩阵：`showcase`（默认）/ `quick` / `full` |
| `-u` / `--url` | URL 列表，可重复或逗号/空格分隔；省略时取当前可用的内建 URL |
| `-s` / `--suite` | 套件：`throughput` / `latency` / `topology` / `fanout` / `serialization` / `backpressure` |
| `-m` / `--mode` | 执行模式：`local-direct` / `local-loop` / `process` |
| `-t` / `--topology` | 拓扑：`1:1` / `1:n` / `n:1` / `n:n` |
| `-k` / `--payload` | 负载类型：`bytes` / `string` / `rawdata` |
| `-q` / `--qos` | QoS profile 列表 |
| `--size` / `--latency-size` | 吞吐 / 时延测试的 payload 大小列表（字节） |
| `-r` / `--rate` | 固定速率场景的速率列表（Hz） |
| `--report` | 输出目标：`html` / `json` / `csv` / `terminal` / `both`，可组合（默认 `html` + `terminal`） |
| `--no-pager` / `--silent` | 关闭交互分页 / 静默（适合 CI） |
| `-o` / `--output` | 输出文件前缀（不带扩展名） |

预设差异：`showcase` 与 `quick` 为保守默认（`throughput` + `latency`、`process` 模式、`1:1` 拓扑、`bytes` payload，分钟级耗时）；`full` 默认展开 `throughput` / `latency` / `topology` / `serialization` 四个套件、三种执行模式（`local-direct` / `local-loop` / `process`）、更大的 payload 阶梯与拓扑/QoS 扫描，适合正式报告与横向对比。`fanout` 与 `backpressure` 套件不在任何预设的默认集中，需经 `--suite fanout` / `--suite backpressure` 显式启用。

报告结构：HTML 报告以"结论—定位—细节"为序组织，顶部为推荐传输配置与综合评分，往下依次为测试概览、传输健康、按消息大小的延迟/吞吐对比、分项结果与完整明细表，并提供可缩放/拖拽/悬浮的趋势折线图；评分以延迟与吞吐为主、辅以资源占用与丢包等维度。终端视图（`--report terminal`）为可翻页/搜索/排序/导出的聚合表格，`q` / `Esc` 退出。

### 🔗 10.2.11 命令行工具组合工作流

各工具的观测目标互补，串联即可覆盖多数现场场景。组合时需注意：经服务发现的工具要求后端链路可达，跨机时其覆盖范围受传输后端选型约束，详见 [传输后端与 URL](04-transport.md)。

**故障定位：从环境到消息内容逐层下钻**

```bash
vlink-check diag
vlink-list
vlink-monitor -x
vlink-eproto sub dds://sensor/imu -d /home/protos/
```

在 `vlink-monitor` 中选中目标行按 `Enter`，即自动跳转至上述第四步，无需手动调用 `vlink-eproto` / `vlink-efbs`。

**采集分析：录制 → 检视 → 提字段 → 裁剪**

```bash
vlink-bag record /tmp/test.vdb -d 60 -p
vlink-bag info /tmp/test.vdb
vlink-dump dds://sensor/imu -t csv -c "linear_acceleration.x" \
  -f /tmp/test.vdb -o /tmp/analysis/ -d /home/protos/
vlink-bag clone /tmp/test.vdb /tmp/clipped.vcap -b 5 -e 55 -p
```

**自动化对接：以退出码与纯文本输出衔接 shell**

```bash
vlink-list -c
if [ $? -eq 0 ]; then
  echo "No active VLink processes"
fi

vlink-monitor --plain -l > /tmp/monitor_$(date +%s).log &
vlink-bag record /tmp/test.vdb -d 30
```

---

## 📚 相关文档

- [快速开始](01-started.md)：最小可运行示例与环境准备
- [传输后端与 URL](04-transport.md)：跨机工具的链路可达性与后端选型
- [零拷贝](06-zerocopy.md)：CameraFrame / PointCloud 等零拷贝类型字段定义
- [录制与回放](09-recording.md)：bag 文件格式与录制/回放 C++ API
- [可视化工具](11-visualization.md)：桌面 Viewer 套件与 Web 可视化（Foxglove / Rerun）
- [可观测性](12-observability.md)：服务发现与代理监控（`vlink-proxy`）
- [集成与环境变量](13-integration.md)：完整 `VLINK_*` 环境变量清单与 C API
