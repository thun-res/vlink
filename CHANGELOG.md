# 🗒️ 更新日志

## v2.1.0 (2026/07/18)

### 新增功能

- **零拷贝解析**：新增统一的 `vlink::zerocopy::MessageParser` 及 Python `ZeroCopyMessageParser`，覆盖八种内置零拷贝类型，并统一 CLI、viewer、analyzer 与 Web 可视化的字段路径、集合边界和 64 位整数精度处理。字段描述符 `Field` 额外携带枚举 / 时间 / 布尔 / 保留语义，并在同一头文件提供纯反射驱动的可读渲染器 `format_message()`；`vlink-dump`、`vlink-efbs`、`vlink-eproto` 共用同一渲染器，替代此前各自手写的按类型打印分支（`vlink-dump` 由此恢复日期、枚举符号名、`protocol` 块与张量 `shape` 等富文本输出）。viewer 属性树与 webviz 动态 Protobuf 描述符亦改由 `fields()` / `element_fields()` 反射构建，消除按类型手写解析（性能敏感的相机 / 激光实时显示除外）。
- **CameraFrame**：扩展图像格式支持，新增更多 Raw、Bayer、YUV 与压缩格式；增加编码辅助函数、Python 绑定和“自动”解码模式，同时保持线上格式的枚举值兼容。
- **zenoh**：新增调试环境变量开关，默认关闭 gossip scouting。
- **trigger**：新增内存触发式事件数据记录工具 `vlink-trigger`（EDR）及 `vlink::TriggerRecorder` 引擎，并提供 Python 绑定。为服务发现到的每个 URL 维护滚动环形缓冲，触发时（`dump()` 或 `vlink-trigger dump`）将触发点前后的窗口落盘为 bag 并轮转历史文件，窗口大小可按 URL 覆盖。默认按采集时刻写盘，可选绑定 `BagPluginInterface` 按真实数据面时间重排。另提供独立的 `TriggerPluginInterface` 生命周期插件，`on_dump_finished` 为 dump 后的上传 / 归档钩子。两类插件均由宿主（CLI 或 Python）加载并绑定。
- **bag-plugin**：将 `BagPluginInterface::on_read` 与 `on_write` 改为纯虚函数，移除 `VersionInfo` / `get_version_info`，并新增读会话 `reset()` 钩子；回放调用 `on_read()` 前会填充有效 `ser_type` / `schema_type`。插件接口版本保持 2.0，实现必须基于新头文件重新构建并声明 `VLINK_PLUGIN_DECLARE(..., 2, 0)`；C++ CLI 与 Python `Plugin.load_bag_plugin()` 均请求 2.0。

### 改进

- **公共 API 调整**：`TerminalStream` 移至 `base`，并统一 Bag/Trigger 接口命名；旧接口不再保留。
- **CMake**：使构建树中的 `VLINK_NO_INTRA_LIBRARIES` 正确移除 `vlink::intra`，与安装包配置保持一致。
- **覆盖率**：使用 lcov 2.x 生成报告时保留 `LCOV_EXCL_START` / `LCOV_EXCL_STOP` 区域。
- **URL 插件**：扩展 `VLINK_URL_PLUGINS` 的模式值，且大小写不敏感：`auto` 在首次使用已知但未链接的共享 transport 时自动加载；空值或 `none` 关闭插件加载；其他非空值仍作为显式预加载列表。拆分后的 runtime 包包含可动态加载 transport 的无版本 NAMELINK；c_api、proxy 与 exprtk 等开发 API 的 NAMELINK 仍归入 devel 包，因此 `auto` 模式不依赖开发包。
- **bag**：在 C++ 与 Python 中公开幂等的 `BagWriter::close()`，调用方可在析构前完成 metadata、footer 和 split manifest 写入，再通过 `fail()` 检查结果。
- **bag**：将帧、Schema 与插件输出的写入策略统一由 `BagWriter::Config::sync_mode` 在 writer 创建时确定：`false` 经后台队列写入并启用 VDB 周期 cache flush，`true` 在产生数据的线程执行 backend 写入并禁用该周期 flush。**破坏性变更**：移除 C++ / Python `push()`、`push_schema()` 的逐调用 `immediate` 参数，protected `record()` 改为接收已解析时间戳；原 `immediate=true` 调用须在创建前设置 `config.sync_mode=true`，外部派生类与既有二进制须重新构建。同步执行不等同于逐帧事务提交或 `fsync`。
- **bag 性能**：优化异步写入、回放过滤和未启用录制时的热路径。
- **基础库**：优化 MemoryPool 和任务调度，并修复相关并发问题。C++ / Python `MemoryPool::Config` 新增 `batch_size`（默认 `16`），默认配置支持 `VLINK_MEMORY_BATCH_SIZE`。**C++ ABI 变化**：使用该配置的外部二进制须重新构建。
- **序列化/零拷贝**：新增 C++ `Serializer::serialize_to_transport()`，修复 FlatBuilder 的 transport loan 生命周期。
- **trigger**：无论是否允许输出到 `dump_dir` 之外，相对显式输出路径都统一从 `dump_dir` 解析。
- **viewer**：使用 FFmpeg 线程解码，QImage 仅作为回退，修复多相机 JPEG 的严重卡顿；默认启用 FFmpeg；零拷贝流遵循图像类型下拉框；修复帮助链接，并通过 GitHub 最新发布版本检查更新。
- **viewer/perception**：新增面向 vmsgs Protobuf / FlatBuffers 感知消息的可配置映射，覆盖目标、雷达、车道边界、道路标线、轨迹、停车位、占据栅格与车辆 HUD；动态 FlatBuffers 反射支持 table、内联 struct、`vector<table>` 与 `vector<struct>`。车道边界类型会转换为 Viewer 线型语义，道路标线按元素过滤到车道线、停止线或人行横道图层；跟踪对象与停车位 ID 在映射及 OSG 数据链路中保持 64 位。合并同一 URL 的待渲染帧；无 OSG 构建不再排队。
- **viewer/analyzer**：每个分析单元的输入帧只解析一次 schema 类型。
- **webviz**：将 CameraFrame 支持的格式接入 Foxglove 与 Rerun，并修复 Rerun 对 16/32/64 位多通道图像的路由；Foxglove 只读查询改用共享锁。
- **shm/shm2**：删除 wait 模式的逐消息 INFO 日志。
- **shm2**：大尺寸 `Bytes` 发布改用借贷内存以避免拷贝；无 fd 的 iceoryx2 listener 使用非忙等等待路径；默认 slice 与内存大小提升至 4 KiB。
- **eproto/efbs**：兼容 protobuf 3.21.12 及以上版本的 proto3 默认标量输出，按字段编号输出，排序 map 条目，并生成合法 JSON。
- **vlink-dump**：收紧命令行参数值与模式校验；`-n` 精确停止，`--hz` 按整数微秒执行严格最大频率限制。slice/scan 将字段解析移到 URL、action、采样和时间窗口门控之后，未使用过滤/事件/CSV 时不再建立字段解码 runtime 或逐帧解析 payload；同步切片 writer 不再启动无用后台线程；质量与 manifest 输出顺序保持确定。
- **bench**：根据 payload 大小自动调整 shm2 运行时 URL，并改进报告分组与一致性。

### 修复

- 修复 VDB / VCAP 多次回放、`stop()` 后重播及 `jump()` 后新会话之间的 bag 重排缓存污染：自然完成的每轮回放同步 `flush()`；若在边界排空前观察到中断则跳过 `flush()`，新顶层读会话在 ready 回调前同步 `reset()` 并丢弃中断会话尾帧，同时重置数据时间与输出时间锚点。
- 修复 `vlink-dump` 从 bag 插件输出回调达到数量上限时同步解绑插件造成的自等待死锁；回调现在只请求 reader 停止，插件在安全生命周期边界处理。
- 修复 `vlink-dump` live 订阅初始化失败、提前返回和输出写失败时的资源生命周期问题；最终状态只在回调停止且 CSV/JSON/切片 writer 完成关闭后报告，空 JSON 结果稳定输出 `[]`。
- 修复 `vlink-dump slice/scan` 的末尾亚毫秒消息遗漏、空尾段未生成、Method URL 选择、bag/分片/segments/schema config 输入与输出碰撞、跨 Protobuf 根目录与同名异构 schema 导入，以及缺失/不支持的 schema 解码器导致的静默空输出；字段相关操作现仅放行可解码的 ZeroCopy 与 Protobuf 主题。
- 修复 `--schema_plugin` / `VLINK_SCHEMA_PLUGIN` 无法按文档加载共享库完整路径的问题；显式配置的插件加载失败时不再静默回退。
- 修复 `proxy_server` 的 `max_packet_size` 处理。
- 修复 `vlink-bag record` 退出时崩溃。
- 任务队列已满时不再丢弃已接受的异步 bag 写入；在任务的所有结束路径释放队列内存，并通过 `BagWriter::fail()` 暴露延迟帧/Schema 写入失败与关闭阶段失败。
- 防止已放弃的延迟 trigger dump 持有缓冲数据或在 recorder 重启后重新出现。
- 仅在 writer 循环启动后为限时 `vlink-bag record` 计时，并使 `--deft` 模式保持 DiscoveryViewer 后台运行；Python `TriggerRecorder.async_run()` 等待 recorder 启动完成；viewer 会报告录制文件关闭失败。
- 加强图像 payload 校验与不安全尺寸处理。
- 在 `vlink-efbs`、`vlink-dump`、viewer 感知显示和 webviz RPC 解码前验证 FlatBuffers schema 与 payload，拒绝损坏或类型不匹配的数据。
- 同步内置 Foxglove FlatBuffers schema，并修复 webviz 动态 schema / 字段 / 表达式线程缓存的实例与 schema 生命周期隔离、FlatBuffers table 缺失字段的映射默认值、重复 RPC 请求的多余拷贝及慢客户端发送队列无界增长；Rerun typed tensor 在对齐数据上直接借用输入缓冲，图像与占据栅格转换减少整帧拷贝，CMake 3.15 可从默认安装路径发现 Rerun SDK。
- 修复 Release 构建的 Python 状态字典将非 `PublicationMatched` 状态按错误派生类型读取，导致字段错误或未定义行为的问题。
- 修复 Python `ZeroCopyMessageParser` 在 `parse()` / `parse_type()` 后借用已释放临时缓冲的悬垂访问（use-after-free）：解析器现直接借用输入 `Bytes` 的存储，并由绑定持有该输入以维持其生存期；输入指针或大小改变后，绑定会拒绝继续读取。解析器有效期间仍禁止原地修改输入内容。
- 修复 `MemoryResource::is_equal()` 与外部 PMR resource 比较时的未定义行为。
- 无 SSL 支持的 DDSC 构建不再因 `ssl.*` 配置隐式启用 TCP。
- 降低 DDS/DDSC 与 shm2 生命周期测试受 teardown 竞态影响的概率。

## v2.0.0 (2025/07/01)

- 初始化源码。
