# 🗒️ 更新日志

## v2.1.0 (2026/07/12)

### 新增功能

- **零拷贝解析**：新增统一的 `vlink::zerocopy::MessageParser` 及 Python `ZeroCopyMessageParser`，覆盖八种内置零拷贝类型，并统一 CLI、viewer、analyzer 与 Web 可视化的字段路径、集合边界和 64 位整数精度处理。字段描述符 `Field` 额外携带枚举 / 时间 / 布尔 / 保留语义，并在同一头文件提供纯反射驱动的可读渲染器 `format_message()`；`vlink-dump`、`vlink-efbs`、`vlink-eproto` 共用同一渲染器，替代此前各自手写的按类型打印分支（`vlink-dump` 由此恢复日期、枚举符号名、`protocol` 块与张量 `shape` 等富文本输出）。viewer 属性树与 webviz 动态 Protobuf 描述符亦改由 `fields()` / `element_fields()` 反射构建，消除按类型手写解析（性能敏感的相机 / 激光实时显示除外）。
- **CameraFrame**：扩展图像格式支持，新增更多 Raw、Bayer、YUV 与压缩格式；增加编码辅助函数、Python 绑定和“自动”解码模式，同时保持线上格式的枚举值兼容。
- **zenoh**：新增调试环境变量开关，默认关闭 gossip scouting。
- **trigger**：新增内存触发式事件数据记录工具 `vlink-trigger`（EDR）及 `vlink::TriggerRecorder` 引擎，并提供 Python 绑定。为服务发现到的每个 URL 维护滚动环形缓冲，触发时（`dump()` 或 `vlink-trigger dump`）将触发点前后的窗口落盘为 bag 并轮转历史文件，窗口大小可按 URL 覆盖。默认按采集时刻写盘，可选绑定 `BagPluginInterface` 按真实数据面时间重排。另提供独立的 `TriggerPluginInterface` 生命周期插件，`on_dump_finished` 为 dump 后的上传 / 归档钩子。两类插件均由宿主（CLI 或 Python）加载并绑定。
- **bag-plugin**：将 `BagPluginInterface::on_read` 与 `on_write` 改为纯虚函数，并从 `BagPluginInterface` 移除 `VersionInfo` 与 `get_version_info`（仅 `SchemaPluginInterface` 保留；新增的 `TriggerPluginInterface` 从未包含它们）。**破坏性变更**：虚表布局已改变，`BagPluginInterface` 插件主版本提升至 2。插件必须基于新头文件重新构建并声明 `VLINK_PLUGIN_DECLARE(..., 2, 0)`；宿主 `vlink-bag`、`vlink-dump` 与 `vlink-trigger` 现请求主版本 2，并拒绝旧插件二进制。

### 改进

- **CMake**：使构建树中的 `VLINK_NO_INTRA_LIBRARIES` 正确移除 `vlink::intra`，与安装包配置保持一致。
- **覆盖率**：使用 lcov 2.x 生成报告时保留 `LCOV_EXCL_START` / `LCOV_EXCL_STOP` 区域。
- **URL 插件**：扩展 `VLINK_URL_PLUGINS` 的模式值，且大小写不敏感：`auto` 在首次使用已知但未链接的共享 transport 时自动加载；空值或 `none` 关闭插件加载；其他非空值仍作为显式预加载列表。拆分后的 runtime 包包含可动态加载 transport 的无版本 NAMELINK；c_api、proxy 与 exprtk 等开发 API 的 NAMELINK 仍归入 devel 包，因此 `auto` 模式不依赖开发包。
- **bag**：在 C++ 与 Python 中公开幂等的 `BagWriter::close()`，调用方可在析构前完成 metadata、footer 和 split manifest 写入，再通过 `fail()` 检查结果。
- **trigger**：无论是否允许输出到 `dump_dir` 之外，相对显式输出路径都统一从 `dump_dir` 解析。
- **viewer**：使用 FFmpeg 线程解码，QImage 仅作为回退，修复多相机 JPEG 的严重卡顿；默认启用 FFmpeg；零拷贝流遵循图像类型下拉框；修复帮助链接，并通过 GitHub 最新发布版本检查更新。
- **viewer/perception**：新增面向 vmsgs Protobuf / FlatBuffers 感知消息的可配置映射，覆盖目标、雷达、车道边界、道路标线、轨迹、停车位、占据栅格与车辆 HUD；动态 FlatBuffers 反射支持 table、内联 struct、`vector<table>` 与 `vector<struct>`。车道边界类型会转换为 Viewer 线型语义，道路标线按元素过滤到车道线、停止线或人行横道图层；跟踪对象与停车位 ID 在映射及 OSG 数据链路中保持 64 位。
- **webviz**：将 CameraFrame 支持的格式接入 Foxglove 与 Rerun，并修复 Rerun 对 16/32/64 位多通道图像的路由。
- **shm2**：大尺寸 `Bytes` 发布改用借贷内存以避免拷贝；无 fd 的 iceoryx2 listener 使用非忙等等待路径；默认 slice 与内存大小提升至 4 KiB。
- **eproto/efbs**：兼容 protobuf 3.21.12 及以上版本的 proto3 默认标量输出，按字段编号输出，排序 map 条目，并生成合法 JSON。
- **bench**：根据 payload 大小自动调整 shm2 运行时 URL，并改进报告分组与一致性。

### 修复

- 修复 `proxy_server` 的 `max_packet_size` 处理。
- 修复 `vlink-bag record` 退出时崩溃。
- 任务队列已满时不再丢弃已接受的异步 bag 写入；在任务的所有结束路径释放队列内存，并通过 `BagWriter::fail()` 暴露延迟帧/Schema 写入失败与关闭阶段失败。
- 防止已放弃的延迟 trigger dump 持有缓冲数据或在 recorder 重启后重新出现。
- 仅在 writer 循环启动后为限时 `vlink-bag record` 计时，并使 `--deft` 模式保持 DiscoveryViewer 后台运行；Python `TriggerRecorder.async_run()` 等待 recorder 启动完成；viewer 会报告录制文件关闭失败。
- 加强图像 payload 校验与不安全尺寸处理。
- 修复 Python `ZeroCopyMessageParser` 在 `parse()` / `parse_type()` 后借用已释放临时缓冲的悬垂访问（use-after-free）：解析器现直接借用输入 `Bytes` 的存储，并由绑定持有该输入以维持其生存期；输入指针或大小改变后，绑定会拒绝继续读取。解析器有效期间仍禁止原地修改输入内容。
- 降低 DDS/DDSC 与 shm2 生命周期测试受 teardown 竞态影响的概率。

## v2.0.0 (2025/07/01)

- 初始化源码。
