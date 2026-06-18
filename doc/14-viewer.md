# 14. 可视化工具

## 14.1 概述

VLink Viewer 套件是一组基于 Qt 的图形界面工具，专为 VLink 中间件的数据可视化、回放与分析场景设计。
套件包含三个独立的可执行程序。

> **相关文档**：命令行录制/回放工具参见 [13-cli-tools.md](13-cli-tools.md#137-vlink-bag--数据录制与回放)；录制/回放 C++ API 参见 [12-bag-recording.md](12-bag-recording.md)；代理通信层参见 [16-proxy.md](16-proxy.md)。

### 14.1.1 程序列表

| 程序名            | 功能定位                              | 源码位置                       |
| ----------------- | ------------------------------------- | ------------------------------ |
| `vlink-viewer`    | 实时通信监控与数据可视化              | `viewer/main.cc`               |
| `vlink-player`    | Bag 文件回放播放器                    | `viewer/player/player.cc`      |
| `vlink-analyzer`  | 数据波形分析器                        | `viewer/analyzer/analyzer.cc`  |

三个程序均使用 Qt Fusion 风格，支持高 DPI 缩放，在 Linux / macOS / Windows 平台下运行。

---

## 14.2 环境依赖

### 14.2.1 必选依赖

| 依赖库             | 版本要求   | 用途                                       |
| ------------------ | ---------- | ------------------------------------------ |
| Qt5 或 Qt6         | 5.12+      | 界面框架（Widgets、Sql、Network）          |
| Qt6::OpenGLWidgets | Qt6 专用   | OpenGL 渲染（仅 Qt6 时需要）              |
| protobuf           | 3.4.0+     | 动态消息解析（Protobuf 编译器接口）        |
| flatbuffers        | 2.0+       | FlatBuffers 反射动态解析                   |
| vlink::proxy_api   | 同版本      | 代理通信层，用于订阅、控制中间件数据流    |
| Eigen3             | 3.x        | 矩阵运算（相机投影等）                     |

### 14.2.2 可选依赖

| 依赖库             | CMake 选项               | 默认值 | 用途                                   |
| ------------------ | ------------------------ | ------ | -------------------------------------- |
| FFmpeg + swscale   | `ENABLE_VIEWER_FFMPEG`   | ON     | 视频帧解码（H264/H265/MPEG4/YUV 等）  |
| OpenSceneGraph     | `ENABLE_VIEWER_OSG`      | ON     | 3D 场景渲染（点云、目标检测、车道线、预测轨迹、交通信号灯、交通标志、停止线、可行驶区域、占据栅格） |
| exprtk             | `ENABLE_EXPRTK`          | ON     | 感知字段映射与分析器曲线的数学表达式求值；关闭后查看器仍正常构建，仅停用表达式功能 |

启用 FFmpeg 后，编译时自动加入 `VLINK_ENABLE_VIEWER_FFMPEG` 宏。
启用 OSG 后，编译时自动加入 `VLINK_ENABLE_VIEWER_OSG` 宏。
启用 exprtk 后，链接 `vlink::exprtk_api` 并自动加入 `VLINK_ENABLE_EXPRTK` 宏；「About This」对话框会显示其启用状态（与 FFmpeg、OSG 并列）。

OSG 所需模块：`osg`、`osgManipulator`、`osgViewer`、`osgGA`、`osgDB`、`osgUtil`、`osgText`、`OpenThreads`。

可通过环境变量 `OSG_DIR` 指定 OSG 安装路径。

---

## 14.3 编译选项

在 CMake 构建系统中，Viewer 套件通过子目录 `viewer/` 独立管理：

```cmake
# 在父项目 CMakeLists.txt 中启用（默认 OFF，需手动开启）
option(ENABLE_VIEWER "Build VLink Viewer" OFF)

if(ENABLE_VIEWER)
  add_subdirectory(viewer)
endif()
```

> `ENABLE_VIEWER` 依赖 `ENABLE_PROXY`（顶层 `CMakeLists.txt` 会在 `ENABLE_VIEWER=ON` 但 `ENABLE_PROXY=OFF` 时打印 warning 并关闭 Viewer）。Viewer 通过 `vlink::proxy_api` 接入中间件，没有 ProxyAPI 就无法编译。

`viewer/CMakeLists.txt` 自动查找 Qt5 或 Qt6（优先 Qt6），并构建三个子目标：

- `vlink-viewer`（主目录）
- `vlink-player`（`viewer/player/` 子目录）
- `vlink-analyzer`（`viewer/analyzer/` 子目录，依赖 `viewer/thirdparty/qcustomplot/`）

### 14.3.1 FFmpeg 配置

优先查找 `FFmpeg::ffmpeg` Config 模式，其次使用 `cmake/modules/` 下的 Module 模式查找。

### 14.3.2 安装

三个程序均安装到 `${CMAKE_INSTALL_BINDIR}`。

---

## 14.4 启动方式

### 14.4.1 vlink-viewer

直接启动：
```bash
vlink-viewer
```

启动后首先弹出「连接设置」对话框（SettingsDialog），用户可配置工作模式和连接参数，然后进入主窗口。

### 14.4.2 vlink-player

直接启动：
```bash
vlink-player
```

或通过拖放 bag 文件到窗口、通过「选择」按钮打开。

也可由 `vlink-viewer` 菜单（Play - P）自动启动为子进程。

### 14.4.3 vlink-analyzer

直接启动：
```bash
vlink-analyzer
```

或由 `vlink-viewer` / `vlink-player` 自动启动为子进程。支持通过 IpcChannel 接收播放时间轴同步信号。

---

## 14.5 vlink-viewer — 实时可视化主窗口

### 14.5.1 功能定位

`vlink-viewer` 是 VLink 通信监控与实时数据可视化的图形化入口，通过 `ProxyAPI`（参见 [16-proxy.md](16-proxy.md#168-proxyapi-说明)）接入本地或远端 vlink-proxy 进程；默认由 ProxyAPI 完成 Handshake/token 校验，
实时展示所有活跃 URL 的通信状态与数据内容。

### 14.5.2 连接设置（SettingsDialog）

启动时弹出连接设置对话框，参数说明如下：

| 参数                    | 说明                                                                 |
| ----------------------- | -------------------------------------------------------------------- |
| Run as Controller / Listener | 工作模式：Controller（控制器，同一 domain id 只允许一个）或 Listener（监听器） |
| Domain ID               | 通信域 ID，与中间件保持一致                                          |
| Security Key            | Handshake / Control / Time / InfoList 控制面安全密钥，需与 ProxyServer 一致 |
| DDS Implement / DDSC Implement | 分别选择 FastDDS 和 CycloneDDS 后端                          |
| Native Mode             | 仅显示本机节点                                                       |
| Reliable Mode           | 使用可靠传输模式                                                     |
| Tcp Mode                | 强制使用 TCP 传输                                                    |
| Direct Mode             | 直连模式（Discovery 直连，不经 multicast）                           |
| IP Address Binding      | 限制允许接入的 IP 地址                                               |
| IP Address Peer         | 指定对端 IP                                                          |
| DDS TX/RX Buffer        | DDS 收发缓冲区大小（字节）                                           |
| DDS MTU                 | DDS 最大传输单元大小                                                 |
| Detect Upgrade          | 自动检测新版本                                                       |
| Match Version           | 版本严格匹配                                                         |

工作模式决定进程名称格式：
- Controller：`vlink-viewer-<domain_id>-controller`
- Listener：`vlink-viewer-<domain_id>-listener`

### 14.5.3 菜单与快捷键

| 菜单项                 | 快捷键 | 功能                                     |
| ---------------------- | ------ | ---------------------------------------- |
| Local                  | D      | 切换离线/本地模式（读取本地 SQLite DB）  |
| Analyzer               | K      | 启动数据分析器子进程                     |
| Topology               | N      | 拓扑视图（进程连接关系图，当前在 UI 中隐藏，仅作为内部入口保留） |
| Record                 | R      | 打开录制对话框                           |
| Play                   | P      | 打开回放对话框（启动 vlink-player）      |
| Edit                   | E      | 数据编辑对话框（可修改字段值并发布）    |
| Raw                    | J      | 原始字节查看器                           |
| Camera                 | S      | 相机帧预览窗口                           |
| Point3D                | Z      | 3D 场景可视化窗口（点云、目标检测、车道线、预测轨迹等） |
| Map                    | G      | 地图视图（TODO）                         |
| Quit                   | Q      | 退出程序                                 |
| Status Viewer          | X      | 切换状态栏显示                           |
| Proto Viewer           | Y      | 切换 Protobuf 解析面板显示               |
| DB Browser             | W      | 在浏览器中打开 DB Browser for SQLite 官网（外链） |
| Protobuf Decoder       | F      | 在浏览器中打开在线 Protobuf 解码工具（外链）      |
| Communication Matrix   | M      | 在浏览器中打开通信矩阵 wiki（外链）               |
| About Qt               | T      | Qt 版本信息                              |
| About This             | A      | 关于 VLink Viewer                        |
| How to Use             | U      | 使用说明                                 |
| Bug Report             | B      | 提交 Bug 报告                            |
| Download               | L      | 下载更新                                 |

### 14.5.4 URL 列表面板

主窗口左侧为 URL 树状列表（QTreeWidget），展示所有通过 ProxyAPI 发现的 URL。

每个 URL 条目显示：
- URL 名称（如 `dds://chatter`）
- 序列化类型（如 `proto`、`flatbuffers`、`bytes` 等）
- 频率（Hz）、速率（B/s）、丢包率（%）、延迟（ms）等统计信息（可通过复选框切换显示项）

状态栏显示：总 URL 数、活跃数、选中数、总速率、丢包率。

### 14.5.5 属性面板（ProtoViewer）

选中某个 URL 后，右侧属性面板以树状结构展示 Protobuf 消息的字段和值。

支持的显示选项（通过复选框控制）：

| 选项           | 说明                             |
| -------------- | -------------------------------- |
| Show Freq      | 显示频率统计                     |
| Show Rate      | 显示速率统计                     |
| Show Loss      | 显示丢包率                       |
| Show Latency   | 显示延迟                         |
| View           | 展开显示消息字段详细内容         |
| Perf           | 显示性能统计信息                 |
| Array          | 展开数组字段                     |
| Hex            | 以十六进制格式显示 bytes 字段    |
| Enum           | 显示枚举类型名称                 |
| Time           | 将 timestamp 字段转为可读时间    |

支持 Protobuf 动态消息解析（DiskSourceTree + Importer + DynamicMessageFactory），可通过「Proto 目录选择」和「重载」按钮加载 `.proto` 文件。

### 14.5.6 Protobuf 目录配置

- 点击「选择」按钮浏览目录
- 点击「重载」按钮重新加载 `.proto` 文件
- 支持递归导入（`import_protos`，最大深度自动控制）

### 14.5.7 零拷贝类型显示

对于 VLink 零拷贝类型（`CameraFrame`、`PointCloud`、`RawData`、`OccupancyGrid`、`Tensor`、`ObjectArray`、`AudioFrame`），主窗口通过 `update_zero_copy_item_property` 解析并展示其 header 字段（frame_id、seq、time_meas、time_pub 等）及类型特有的元数据字段。其中 `CameraFrame` 与 `PointCloud` 另外支持专门的可视化对话框（CameraDialog / Point3DDialog）；其余类型仅在右侧属性树展示元数据，`ObjectArray` 会在子树中列出前 8 个 `Object` 记录的 label/position/class_id/track_id。

### 14.5.8 相机帧显示（CameraDialog）

按下快捷键 `S` 或从菜单打开 Camera 窗口（CameraDialog）：

**支持的图像格式**（通过 FFmpegDecoder）：

| 类型      | InType 枚举     |
| --------- | --------------- |
| JPEG      | kJPG            |
| H.264     | kH264           |
| H.265     | kH265           |
| MPEG4     | kMPEG4          |
| YUV 4:2:0 | kYUV420         |
| YUV 4:2:2 | kYUV422         |
| YUV 4:4:4 | kYUV444         |
| NV12      | kNV12           |
| YUYV      | kYUYV           |
| YVYU      | kYVYU           |
| UYVY      | kUYVY           |
| BGR888    | kBGR888         |
| RGB888    | kRGB888         |

功能特性：
- 支持多通道（multi_mode），在 QGridLayout 中并排显示多路画面
- 支持帧缓存（cache_frame）
- 支持硬件解码（use_hard_codec）
- 支持暂停播放
- 支持 YUV 格式切换
- 可打开关联的 Point3D 投影视图（projection 功能）
- 解码回调：`DataCallback(channel, seq, width, height, img_data)`

### 14.5.9 3D 场景显示（Point3DDialog）

按下快捷键 `Z` 或从菜单打开 Point3D 窗口（Point3DDialog）。

Point3DDialog 是自动驾驶全场景 3D 可视化的核心窗口，支持 9 种渲染类型：

| 渲染类型                  | 枚举值                | 说明                                       |
| ------------------------- | --------------------- | ------------------------------------------ |
| 点云                      | `kPointCloud`         | PointCloud 数据的 3D 散点渲染              |
| 目标检测                  | `kObjectDetection`    | 3D 边界框、速度向量、跟踪 ID、分类标签    |
| 车道线                    | `kLaneLine`           | 车道线折线渲染，按索引自动着色             |
| 预测轨迹                  | `kPrediction`         | 预测运动轨迹，含置信度与 track_id          |
| 交通信号灯                | `kTrafficLight`       | 信号灯位置及颜色状态、倒计时               |
| 停止线                    | `kStopLine`           | 停止线折线渲染                             |
| 交通标志                  | `kTrafficSign`        | 交通标志位置与类型标记                     |
| 可行驶区域                | `kFreespace`          | 多边形区域填充渲染                         |
| 占据栅格                  | `kOccupancyGrid`      | 栅格地图单元格着色渲染                     |

Point3DDialog 通过 `url_render_type_map_` 将每个 URL 映射到对应的渲染类型，各类型数据通过独立的 `update_ui_for_*` 方法处理：

| 方法                              | 数据源                     |
| --------------------------------- | -------------------------- |
| `update_ui_for_zero_copy_types`   | VLink 零拷贝类型           |
| `update_ui_for_proto`             | Protobuf 消息              |
| `update_ui_for_flatbuffers`       | FlatBuffers 消息           |
| `update_ui_for_od`                | 目标检测（OD）数据         |
| `update_ui_for_lane`              | 车道线数据                 |
| `update_ui_for_prediction`        | 预测轨迹数据               |
| `update_ui_for_traffic_light`     | 交通信号灯数据             |
| `update_ui_for_stop_line`         | 停止线数据                 |
| `update_ui_for_traffic_sign`      | 交通标志数据               |
| `update_ui_for_freespace`         | 可行驶区域数据             |
| `update_ui_for_occupancy_grid`    | 占据栅格数据               |

点云渲染特性：
- 点云数据结构：`Point3dMap`，每个 URL 对应多帧，每帧包含 `(x, y, z, r, g, b, intensity, value_list)` 元组
- 支持基于 OpenSceneGraph（OsgWidget / OsgGraphicsView）的实时 3D 渲染（需启用 `ENABLE_VIEWER_OSG`）
- 支持点大小调节、颜色范围设置、点云滤波（基于表达式，expression 语法、ASTNode 解析）
- 支持平台/车辆模型叠加显示（platform_node、car_node）
- 支持点选（OsgSelectHandler）
- 与相机画面（CameraDialog）关联，可在 3D 点云上叠加投影（ProjectionDialog）

Protobuf 字段自动映射（ProtoOdCandidate）：

对于目标检测类数据，Point3DDialog 通过 `ProtoOdCandidate` 自动映射 Protobuf repeated 消息中的字段路径，包括：position (x/y/z)、size (length/width/height)、yaw、score、track_id、class_id、velocity (vx/vy/vz)。车道线、预测轨迹等折线类数据通过 `ProtoPolylineCandidate` 映射 repeated 消息中的嵌套点序列字段。FlatBuffers 数据同样具备对应的 `FlatbuffersOdCandidate` 和 `FlatbuffersPolylineCandidate` 映射结构。

源码文件组织：

| 文件                              | 职责                         |
| --------------------------------- | ---------------------------- |
| `point3ddialog.h`                 | 主头文件（类定义与数据结构） |
| `point3ddialog.cc`                | 主对话框逻辑                 |
| `point3ddialog_3dscene.cc`        | 3D 场景管理与 OSG 初始化     |
| `point3ddialog_pointcloud.cc`     | 点云数据处理与渲染更新       |
| `point3ddialog_internal.h`        | 内部工具函数（颜色计算、距离计算、Protobuf/FlatBuffers 数值读取等） |

### 14.5.10 进程面板

主窗口展示所有关联进程的状态，通过 `process_map_` 维护进程列表，由 `update_process_widget()` 定期刷新。

### 14.5.11 本地模式（Local/Offline）

按下快捷键 `D` 可切换到离线模式，直接从本地 SQLite 数据库（.vdb / .vdbx / .vcap / .vcapx 文件）读取数据，
使用 `QSqlDatabase`（local_database_）加载，不依赖实时中间件。

---

## 14.6 vlink-player — Bag 文件回放播放器

### 14.6.1 功能定位

`vlink-player` 是 VLink bag 文件的图形化回放工具，基于 `vlink::BagReader` 实现。
支持文件拖放打开、回放速率控制、进度条跳转、URL 过滤和 URL 重映射。

### 14.6.2 界面操作说明

主界面工具栏按钮（按钮文字以 UI 中实际显示为准）：

| 按钮                      | 功能                                              |
| ------------------------- | ------------------------------------------------- |
| Open File（选择 bag）     | 打开文件选择对话框，选择 bag 文件                 |
| Close File（关闭 bag）    | 关闭当前 bag 文件                                 |
| Check Point（锚点）       | 打开 PointDialog（设置 URL 过滤和 URL 重映射）    |
| Bag Information（信息）   | 打开 InfoDialog，查看 bag 文件元信息              |
| Start Viewer（启动查看器）| 启动 vlink-viewer 子进程                          |
| Run CMD（命令行）         | 打开命令行工具                                    |
| Bag Analyzer（分析器）    | 启动 vlink-analyzer 子进程（并传递当前文件路径）  |
| Play（播放）              | 开始/恢复回放                                     |
| Pause（暂停）             | 暂停回放                                          |
| Real Time（时间模式）     | 切换时间戳显示模式（相对/本地/UTC）               |
| Remap（重映射）           | 切换 URL 重映射开关                               |
| Skip Blank（跳过空白）    | 切换跳过空白区间模式                              |
| Start Proxy（代理）       | 切换是否启用 proxy 进程辅助回放                   |

### 14.6.3 回放控制

**进度条（HorizontalSlider）**：
- 拖动时暂停回放（press_and_paused_），松开后恢复
- 实时显示当前进度百分比

**速率调节（DoubleSpinBox）**：
- 调节回放速率倍率（last_rate_），默认 1.0x

**其他选项**：

| 复选框                | 功能                                                       |
| --------------------- | ---------------------------------------------------------- |
| Loop（循环）          | 循环回放                                                   |
| Native（原生）        | 原生传输模式                                               |
| Blacklist（黑名单）   | 把 `Filter` 输入框作为黑名单使用（命中即排除，否则为白名单）|

### 14.6.4 时间戳显示模式

| 模式        | 枚举值         | 说明                     |
| ----------- | -------------- | ------------------------ |
| 相对时间    | kTimeRel       | 显示从起点开始的相对时间 |
| 本地时间    | kTimeLocal     | 系统本地时区时间         |
| UTC 时间    | kTimeUtc       | UTC 时区时间             |

### 14.6.5 文件拖放

实现 `dragEnterEvent` / `dropEvent`，支持将 bag 文件（.vdb / .vdbx / .vcap / .vcapx）直接拖入窗口打开。

### 14.6.6 URL 过滤与重映射（PointDialog）

- `filter_urls_`：设置需要回放的 URL 白名单（不在列表内的 URL 不会被发布）
- `remap_`（vlink::UrlRemap）：将 bag 文件中记录的 URL 映射到其他 URL 发布
- `remap_path_`：重映射配置文件路径

### 14.6.7 进程管理

PlayerWindow 持有三个 QProcess 成员：

| 成员              | 对应进程        |
| ----------------- | --------------- |
| proxy_process_    | vlink-proxy     |
| viewer_process_   | vlink-viewer    |
| analyzer_process_ | vlink-analyzer  |

启动 analyzer 时会通过标准输入管道传递时间戳（IpcChannel 机制），实现 player 与 analyzer 之间的时间轴联动。

### 14.6.8 状态说明

回放过程中通过 `status_` 原子变量跟踪回放状态，`has_played_`、`has_error_`、`quit_flag_` 等原子标志用于线程安全控制。

---

## 14.7 vlink-analyzer — 数据分析器

### 14.7.1 功能定位

`vlink-analyzer` 是基于 QCustomPlot 的波形数据分析工具，可从 bag 文件中读取任意 Protobuf 字段的历史数据，
绘制时间序列折线图，支持统计分析与数据导出。

### 14.7.2 工作模式

| 模式类型         | 枚举值            | 说明                                   |
| ---------------- | ----------------- | -------------------------------------- |
| 频率类型         | kFrequencyType    | 分析消息发布频率随时间的变化           |
| 数值类型         | kValueType        | 分析 Protobuf 字段的数值随时间的变化   |
| 自定义类型       | kCustomType       | 用户自定义表达式（exprtk 支持）        |

### 14.7.3 界面操作说明

**工具栏按钮**（按钮文字以 UI 中实际显示为准）：

| 按钮                          | 功能                                              |
| ----------------------------- | ------------------------------------------------- |
| Open Bag File（路径）         | 选择 bag 文件路径                                 |
| Open Config File（配置）      | 加载分析配置文件（JSON 格式）                     |
| Open Proto Dir（原型）        | 选择 Protobuf `.proto` 文件目录                   |
| Open FlatBuffers Dir（Fbs）   | 选择 FlatBuffers `.fbs` 文件目录                  |
| Generate（生成）              | 开始解析 bag 文件并绘制图表                       |
| Interrupt（中断）             | 中断当前解析任务                                  |
| Export（导出）                | 导出图表数据（CSV 等格式）                        |
| Close（关闭）                 | 关闭当前配置/文件                                 |

**图表显示选项**：

| 复选框                         | 功能                                     |
| ------------------------------ | ---------------------------------------- |
| Time（时间轴 groupBox）        | 以时间格式（QCPAxisTicker）显示 X 轴     |
| Legend Visible（图例）         | 显示/隐藏图例                            |
| Grid Visible（网格）           | 显示/隐藏网格线                          |
| Points Visible（数据点）       | 显示/隐藏数据点标记                      |
| Timeline Visible（时间线）     | 显示/隐藏时间线游标                      |
| Tracking Pos（跟踪）           | 开启时间轴跟踪（随播放器时间轴移动）     |

**下拉选框**：

| 控件             | 功能                                     |
| ---------------- | ---------------------------------------- |
| Zoom（缩放）     | 选择 X 轴缩放范围                        |
| Line（线型）     | 选择折线样式                             |
| Count（数量）    | 控制显示的图表数量上限                   |

### 14.7.4 PlotUnit 数据结构

每个图表单元（PlotUnit）对应一条折线，包含以下字段：

| 字段                  | 类型                   | 说明                               |
| --------------------- | ---------------------- | ---------------------------------- |
| index                 | int                    | 图表索引                           |
| label                 | string                 | 图例标签                           |
| url                   | string                 | 数据来源 URL                       |
| url_filter            | string                 | URL 过滤条件                       |
| expressions           | vector<string>         | 字段表达式列表                     |
| color                 | QColor                 | 折线颜色                           |
| ext_operation_pro     | bool                   | 是否启用扩展运算                   |
| ext_sample_interval   | int                    | 采样间隔                           |
| ext_zero_start_x      | bool                   | X 轴从零开始                       |
| ext_zero_start_y      | bool                   | Y 轴从零开始                       |
| ext_limit_max/min_x/y | double                 | 轴范围限制                         |
| ext_operation_x/y     | string                 | X/Y 轴自定义运算表达式             |
| x_values / y_values   | QVector<double>        | 数据点序列                         |
| condition_lists       | vector<vector<string>> | 条件过滤规则列表                   |
| graph                 | QCPGraph*              | QCustomPlot 图形对象               |
| root_msg              | protobuf::Message*     | 关联的 Protobuf 消息实例           |

### 14.7.5 时间线游标（timeline_）

`timeline_` 是 `QCPItemLine` 类型的垂直线，表示当前时间位置。

当 `enable_timeline_` 为 true 且与 vlink-player 联动时，IpcChannel 接收到时间戳后自动移动游标。

### 14.7.6 键盘操作

| 按键        | 功能                         |
| ----------- | ---------------------------- |
| Space       | 暂停/恢复（与 player 联动）  |
| Esc         | 中断解析任务                 |
| 方向键      | 调整 X/Y 轴范围              |

### 14.7.7 数据导出

点击「Export」按钮，将当前所有 PlotUnit 中的 `x_values` 和 `y_values` 数据导出为文件（支持 CSV 等格式）。

### 14.7.8 Protobuf 集成

AnalyzerWindow 通过以下成员支持动态 Protobuf 解析：

```
source_tree_   — DiskSourceTree（磁盘路径映射）
importer_      — Compiler::Importer（.proto 文件导入）
factory_       — DynamicMessageFactory（动态消息工厂）
des_pool_      — DescriptorPool（消息描述符池）
```

支持自定义表达式（exprtk_parser.h，exprtk 库，需 `ENABLE_EXPRTK=ON`），可对字段值做数学运算后再绘图；未启用时表达式求值返回空，不参与绘图。

---

## 14.8 IpcChannel — 播放器与分析器时间轴同步机制

### 14.8.1 原理

`IpcChannel` 是 `vlink-player` 和 `vlink-analyzer` 之间的进程间通信桥梁，基于标准输入（stdin）管道传递时间戳。

```
vlink-player  ──(stdin pipe)──>  vlink-analyzer
                 timestamp(int64_t)
```

当 vlink-player 启动 vlink-analyzer 子进程时，analyzer 进程的 stdin 已与 player 进程的某个管道相连。
analyzer 通过 IpcChannel 异步监听 stdin，接收到时间戳后触发信号，驱动分析器的时间轴游标同步移动。

### 14.8.2 类结构

```cpp
class IpcChannel : public QObject {
public:
  void send_timestamp(int64_t timestamp);

signals:
  void timestamp_changed(int64_t timestamp);

private:
  std::atomic<bool> quit_flag_;
  QSocketNotifier* notifier_;
  void* hstdin_dup_;
  void* hstop_event_;
  QFile file_;
  std::thread thread_;
};
```

| 成员 | 说明 |
| --- | --- |
| `quit_flag_` | 退出标志 |
| `notifier_` | 异步 IO 通知（Linux/macOS） |
| `hstdin_dup_` | 复制的 stdin 句柄（Windows） |
| `hstop_event_` | 停止事件（Windows） |
| `file_` | 文件对象 |
| `thread_` | 读取线程 |

### 14.8.3 平台差异

| 平台          | 实现方式                                              |
| ------------- | ----------------------------------------------------- |
| Linux / macOS | `QSocketNotifier` 监听 stdin 文件描述符，事件驱动    |
| Windows       | 独立线程（thread_）读取 hstdin_dup_，hstop_event_ 用于优雅退出 |

### 14.8.4 时序流程

```
[vlink-player]
  1. 启动 vlink-analyzer 子进程，建立管道
  2. 每次播放进度更新时调用 ipc_->send_timestamp(current_timestamp)

[vlink-analyzer]
  1. IpcChannel 初始化，监听 stdin
  2. 收到数据 → emit timestamp_changed(timestamp)
  3. AnalyzerWindow::move_timeline(time) 被调用
  4. 时间线游标移动到对应位置
```

---

## 14.9 FFmpegDecoder — 视频帧解码器

### 14.9.1 功能

`FFmpegDecoder` 继承自 `vlink::MessageLoop`，在独立消息循环中异步解码视频帧。

### 14.9.2 配置（Config）

| 字段             | 类型      | 说明                               |
| ---------------- | --------- | ---------------------------------- |
| in_type          | InType    | 输入格式（H264/H265/YUV 等）       |
| out_type         | OutType   | 输出格式（BGR888 / RGB888）        |
| width / height   | int       | 图像尺寸（0 表示自动检测）         |
| scale            | double    | 输出缩放比例（默认 1.0）           |
| cache_frame      | bool      | 是否缓存最后一帧（用于暂停显示）   |
| use_hard_codec   | bool      | 启用硬件解码                       |
| max_elapsed_time | int       | 单帧最大解码时间（ms，默认 100）   |
| max_codec_time   | int       | 编解码超时限制（ms，0=不限）       |

### 14.9.3 使用方式

```cpp
FFmpegDecoder::Config config;
config.in_type = FFmpegDecoder::InType::kH264;
config.out_type = FFmpegDecoder::OutType::kRGB888;
config.width = 1920;
config.height = 1080;

FFmpegDecoder decoder(config);

decoder.register_handler([](int channel, int seq, int width, int height, const vlink::Bytes& img) {
});

decoder.register_error_handler([](int channel, int seq) {
});

decoder.post_data(channel, seq, raw_data);
```

---

## 14.10 OSG 3D 渲染组件

所有 OSG 组件位于 `viewer/osg/` 目录，仅在编译时定义 `VLINK_ENABLE_VIEWER_OSG` 时生效（条件编译保护）。

### 14.10.1 OsgWidget

`OsgWidget` 继承自 `QOpenGLWidget`，封装 `osgViewer::Viewer` 与 Qt 的 OpenGL 集成。

特性：
- FPS 率通过属性 `fpsRate` 暴露，发生变化时触发 `fpsRateChanged` 信号
- 支持完整鼠标交互：旋转、缩放、平移、双击
- 支持键盘事件透传至 OSG
- 定时器驱动帧渲染

### 14.10.2 OsgGraphicsView

`OsgGraphicsView` 继承自 `QGraphicsView`，提供基于 `QGraphicsScene` + `QOpenGLWidget` 的 OSG 集成方案。

与 `OsgWidget` 功能对等（FPS 属性、鼠标/键盘交互透传），适用于需要在 3D 场景上叠加 Qt Graphics Item 的场景。

### 14.10.3 OsgManipulator

`OsgManipulator` 继承自 `osgGA::OrbitManipulator`，自定义的轨道操控器。

特性：
- 左键旋转、中键平移、右键缩放
- 滚轮平滑缩放（带动画插值）
- 拖拽惯性动画
- 飞行路径动画（`AnimationPath` 列表 + `playFly` / `stopFly`）
- 视角移动动画（`moveToPoint`，带平滑过渡）
- 距离和位置范围限制（`setLimit`）

### 14.10.4 OsgSelectHandler

`OsgSelectHandler` 继承自 `osgGA::GUIEventHandler`，提供框选功能。

通过 `registerSelectCallback` 注册选区回调（返回 xMin/xMax/yMin/yMax），在 HUD 相机上绘制选择框。支持 Ctrl 修饰键模式（`registerCtrlCallback`）。

### 14.10.5 场景基础设施

| 组件               | 类/命名空间          | 功能                                             |
| ------------------ | -------------------- | ------------------------------------------------ |
| 坐标轴叠加         | `OsgCoord`           | 在视口角落渲染 XYZ 坐标轴，跟随相机旋转         |
| 场景灯光           | `OsgLight`           | 创建跟随相机的方向光源                           |
| 地面平台           | `OsgPlatform`        | 创建网格地面参考平面                             |
| 工具函数           | `OsgCommon`          | 世界坐标→屏幕坐标转换、矩阵解析、飞行路径解析   |
| 节点访问器         | `OsgNameNodeVisitor` / `OsgAnimationNodeVisitor` | 按名称查找节点 / 提取动画回调 |

### 14.10.6 渲染器组件

以下渲染器均位于 `viewer/osg/` 目录，采用命名空间 + 自由函数的设计模式，每个渲染器提供 `create()`（创建 Geode）和 `update()`（更新数据）两个核心接口。

#### OsgPointCloud — 点云渲染

| 接口                  | 说明                                 |
| --------------------- | ------------------------------------ |
| `create()`            | 创建点云 Geode，指定点大小和缩放比   |
| `update_point_size()` | 动态调整点大小和选中点大小           |
| `clear_arrays()`      | 清空顶点/颜色数组                    |
| `finalize_arrays()`   | 提交顶点/颜色数据到 GPU              |

#### OsgObjectArray — 目标检测渲染

数据结构 `ObjectData`：

| 字段          | 类型        | 说明                       |
| ------------- | ----------- | -------------------------- |
| position[3]   | double      | 中心坐标 (x, y, z)        |
| size[3]       | double      | 尺寸 (length, width, height) |
| yaw           | double      | 航向角                     |
| velocity[3]   | double      | 速度向量 (vx, vy, vz)     |
| score         | double      | 置信度分数                 |
| class_id      | uint32_t    | 分类 ID                    |
| track_id      | uint32_t    | 跟踪 ID                    |
| color         | uint32_t    | 自定义颜色                 |
| label         | string      | 显示标签                   |

| 接口                | 说明                                             |
| ------------------- | ------------------------------------------------ |
| `create()`          | 创建目标检测 Geode                               |
| `update()`          | 更新 3D 边界框和速度向量                         |
| `update_labels()`   | 更新文字标签（需要 osgText::Font）               |
| `get_class_color()` | 根据 class_id 获取预设颜色                       |
| `get_class_name()`  | 根据 class_id 获取分类名称                       |
| `find_class_id()`   | 根据分类名称反查 class_id                        |

#### OsgOccupancyGrid — 占据栅格渲染

数据结构 `GridData`：

| 字段          | 类型            | 说明                         |
| ------------- | --------------- | ---------------------------- |
| origin_x/y/z  | double          | 栅格原点坐标                 |
| resolution    | double          | 栅格分辨率（默认 0.1）       |
| width/height  | uint32_t        | 栅格行列数                   |
| cells         | vector<int8_t>  | 单元格占据值数组             |

| 接口                | 说明                                       |
| ------------------- | ------------------------------------------ |
| `create()`          | 创建占据栅格 Geode                         |
| `update()`          | 更新栅格数据，指定透明度                   |
| `get_cell_color()`  | 根据单元格占据值获取颜色                   |

#### OsgFreespace — 可行驶区域渲染

数据结构 `FreespaceData`：

| 字段       | 类型                     | 说明               |
| ---------- | ------------------------ | ------------------ |
| polygon    | vector<FreespacePoint>   | 多边形顶点序列     |
| color      | uint32_t                 | 填充颜色           |

`FreespacePoint` 包含 `x`、`y`、`z` 三个 double 坐标分量。

| 接口        | 说明                                         |
| ----------- | -------------------------------------------- |
| `create()`  | 创建可行驶区域 Geode                         |
| `update()`  | 更新多边形区域列表，指定透明度               |

#### OsgLaneLine — 车道线渲染

数据结构 `LaneData`：

| 字段       | 类型                | 说明               |
| ---------- | ------------------- | ------------------ |
| points     | vector<LanePoint>   | 车道线点序列       |
| color      | uint32_t            | 线条颜色           |
| lane_type  | int                 | 车道线类型         |

`LanePoint` 包含 `x`、`y`、`z` 三个 double 坐标分量。

| 接口                | 说明                                   |
| ------------------- | -------------------------------------- |
| `create()`          | 创建车道线 Geode                       |
| `update()`          | 更新车道线数据，指定线宽               |
| `get_lane_color()`  | 根据索引获取预设车道线颜色             |

#### OsgPrediction — 预测轨迹渲染

数据结构 `PredictionData`：

| 字段        | 类型                | 说明               |
| ----------- | ------------------- | ------------------ |
| points      | vector<PredPoint>   | 轨迹点序列         |
| color       | uint32_t            | 轨迹颜色           |
| track_id    | uint32_t            | 关联的跟踪 ID      |
| confidence  | float               | 置信度（默认 1.0） |

`PredPoint` 包含 `x`、`y`、`z` 三个 double 坐标分量。

| 接口                       | 说明                                     |
| -------------------------- | ---------------------------------------- |
| `create()`                 | 创建预测轨迹 Geode                       |
| `update()`                 | 更新轨迹数据，指定线宽                   |
| `get_prediction_color()`   | 根据 track_id 获取预设颜色               |

#### OsgStopLine — 停止线渲染

数据结构 `StopLineData`：

| 字段       | 类型                    | 说明               |
| ---------- | ----------------------- | ------------------ |
| points     | vector<StopLinePoint>   | 停止线点序列       |
| color      | uint32_t                | 线条颜色           |
| line_type  | int                     | 停止线类型         |

`StopLinePoint` 包含 `x`、`y`、`z` 三个 double 坐标分量。

| 接口        | 说明                                   |
| ----------- | -------------------------------------- |
| `create()`  | 创建停止线 Geode                       |
| `update()`  | 更新停止线数据，指定线宽               |

#### OsgTrafficLight — 交通信号灯渲染

数据结构 `TrafficLightData`：

| 字段          | 类型      | 说明                         |
| ------------- | --------- | ---------------------------- |
| position[3]   | double    | 信号灯位置 (x, y, z)        |
| color_state   | uint8_t   | 灯色状态                     |
| confidence    | float     | 置信度（默认 1.0）           |
| countdown     | int32_t   | 倒计时秒数（-1 表示无）     |
| label         | string    | 显示标签                     |

| 接口                 | 说明                                     |
| -------------------- | ---------------------------------------- |
| `create()`           | 创建交通信号灯 Geode                     |
| `update()`           | 更新信号灯数据，指定点大小               |
| `get_state_color()`  | 根据灯色状态获取颜色                     |

#### OsgTrafficSign — 交通标志渲染

数据结构 `TrafficSignData`：

| 字段          | 类型      | 说明                         |
| ------------- | --------- | ---------------------------- |
| position[3]   | double    | 标志位置 (x, y, z)          |
| type_id       | uint32_t  | 标志类型 ID                  |
| color         | uint32_t  | 标志颜色                     |
| marker_size   | double    | 标记大小（默认 0.5）        |
| label         | string    | 显示标签                     |

| 接口                | 说明                                     |
| ------------------- | ---------------------------------------- |
| `create()`          | 创建交通标志 Geode                       |
| `update()`          | 更新交通标志数据，指定线宽               |
| `get_sign_color()`  | 根据类型 ID 获取预设颜色                 |

### 14.10.7 渲染管线

```
数据源（vlink::PointCloud / vlink::ObjectArray / Protobuf / FlatBuffers）
  -> Point3DDialog::update_ui_for_* 方法
     -> 各类型数据结构（Point3dMap / ObjectData / LaneData / ...）
        -> osg::Geode（每个 URL 对应一个 Geode，存储于 geo_node_map_）
           -> 对应 Osg* 渲染器的 update() 方法
              -> osgViewer::Viewer（最终渲染）
```

点云渲染支持：
- 按强度（intensity）着色
- 按 Y 轴范围过滤点云
- 自定义表达式过滤（ASTNode 解析）
- 投影矩阵叠加（与相机画面联动，Eigen::Matrix<float,3,4>）

---

## 14.11 典型使用流程

### 14.11.1 场景一：实时系统监控

```
1. 启动 vlink-proxy（或依赖 vlink 中间件自动发现）
2. 运行目标应用程序
3. 启动 vlink-viewer
   - 选择 Controller 模式，配置 domain id
4. 主窗口 URL 列表中出现所有活跃 URL
5. 选中 URL，右侧面板实时显示 Protobuf 消息内容
6. 点击 Camera(S) 预览相机图像帧
7. 点击 Point3D(Z) 打开 3D 场景窗口，查看点云、目标检测、车道线、预测轨迹等数据
```

### 14.11.2 场景二：Bag 文件离线分析

```
1. 录制 bag 文件（vlink-bag record 或 viewer 中 Record 功能）
2. 启动 vlink-player，拖入 bag 文件
3. 点击 Analyzer 按钮自动启动 vlink-analyzer
4. 在 analyzer 中加载配置文件，选择要分析的字段
5. 点击 Gen 开始解析
6. player 播放时，analyzer 时间线游标与播放位置同步
7. 分析完成后点击 Export 导出数据
```

### 14.11.3 场景三：离线 DB 数据查看

```
1. 在 vlink-viewer 中按 D 切换到 Local 模式
2. 选择 .vdb / .vdbx / .vcap / .vcapx 文件
3. 在 URL 列表中浏览历史数据
4. 选中 URL 可查看字段值
5. 按 K 启动 Analyzer 进一步分析
```

### 14.11.4 场景四：录制与多进程联动

```
1. 启动 vlink-viewer（Controller 模式）
2. 按 R 打开 Record 对话框，配置录制路径和过滤条件
3. 开始录制
4. 录制完成后按 P 打开 Player 对话框
5. Player 自动载入刚录制的文件，可立即回放验证数据
```

---

## 14.12 注意事项

1. **单例限制**：Controller 模式的 vlink-viewer 同一 domain id 只允许启动一个实例，由 `vlink::Utils::check_singleton` 保证。

2. **Protobuf 版本要求**：动态 Protobuf 解析依赖 Protobuf >= 3.4.0；`vlink-viewer` 入口在 `viewer/main.cc` 中通过 `#if GOOGLE_PROTOBUF_VERSION < 3004000` 做编译期检查，`vlink-analyzer` 复用相关解析能力但入口文件本身没有同样的宏检查。

3. **Qt6 额外依赖**：使用 Qt6 时必须额外链接 `Qt6::OpenGLWidgets`，否则 OSG 渲染窗口无法初始化。

4. **OSG 格式兼容性**：OSG 渲染使用 `QSurfaceFormat::CompatibilityProfile`（兼容模式 OpenGL），在某些平台上可能需要安装兼容模式 OpenGL 驱动。

5. **IpcChannel 平台差异**：Windows 下 IpcChannel 使用独立线程 + hstop_event，确保退出时能够干净终止；Linux/macOS 下使用 QSocketNotifier 事件驱动，效率更高。

6. **FFmpeg 可选**：不启用 FFmpeg 时（`ENABLE_VIEWER_FFMPEG=OFF`），相机图像仅支持 JPEG 格式的软件解码，不支持 H264/H265 视频流。

7. **字体加载**：三个程序均在启动时从资源文件（`:/resource/notomono.ttf`）加载等宽字体 NotoMono，用于代码/数据展示。
