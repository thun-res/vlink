# 🗒️ 更新日志

## v2.2.0 (2026/08/30)

### 新增功能

- **MessageLoop 回调分发**：intra、FDBUS 与 SOME/IP 节点支持 `attach()` / `detach()`；intra queue 绑定后直接投递目标 loop。
- **SOME/IP payload 序列化**：新增 `Serializer::kSomeipType` 与一组 `VLINK_SOMEIP_*` 宏，按字段自动生成 `Bytes` 编解码，覆盖嵌套结构、容器、map、union 与 TLV，并可配置大小端、AUTOSAR alignment 和长度宽度；头文件 `include/vlink/extension/someip_serializer.h` 需显式包含。
- **AUTOSAR SOME/IP 生成器**：支持合并多个 ARXML，并可按类型或 package 生成带依赖关系和汇总入口的多个头文件；生成注释保留类型、字段、上限、`INIT-VALUE` 来源及实际 deployment 信息，直接字段的 maximum 同步用于接收校验或截断。
- **自研日志后端**：新增公开的 `LoggerBackend`，支持异步写入、队列背压、周期刷新、固定/时间戳轮转、UTC 和 backtrace。
- **格式化修饰符**：`vlink::format` 与 `MLOG_*` 支持完整的 std::format 风格 spec，含动态宽度/精度、`long double`、`nullptr`、`format_as` 扩展点、`{:?}` 和返回 `std::string` 的 `format()`；无效 spec 宽容忽略，不抛异常。

### 改进

- **QoS 预设调整**：`kLarge` 更名为 `kStream`（URL 名 `stream`），history 由 KeepLast(500) 调整为 KeepLast(100)；`kBest` 的 history 由 KeepLast(200) 调整为 KeepLast(10)，避免饱和链路上过深的写者历史持续重传消费端已追不上的样本。
- **日志后端配置**：CMake `ENABLE_LOG_BACKEND` / Conan `enable_log_backend` 取代原后端选择项；Android/QNX CMake 与 Android.bp 默认使用平台日志，其余构建默认启用自研后端；移除 spdlog、Quill、DLT 及其开关和依赖。
- **日志后端迁移**：原 CMake `SELECT_LOG_BACKEND` / Conan `select_log_backend` 已删除；改用 `LoggerBackend` 时设置新开关为 `ON`，在 Android、QNX、Linux 上改用平台日志时设置为 `OFF`；旧 `VLINK_ENABLE_LOG_{SPD,QUI,DLT,NAT}` 特性宏不再提供。
- **日志插件与热路径**：`LoggerPluginInterface` 新增 `flush()`，钩子改为 `noexcept`，修复初始化递归与卸载排空；日志宏通过运行期级别过滤后才求值参数。现有插件需适配并重新编译。
- **MessageLoop 背压**：普通与优先级满队列阻塞改为在容量释放或退出时唤醒；全局阻塞策略也会响应策略切换，避免固定 1 ms 轮询。
- **内存池增长粒度**：惰性增长改为按最多 64 KiB（且不小于单块）的小 chunk 安装，限定触发增长线程的单次缺页突发；预分配与分配/释放热路径不变，惰性层 chunk 数量相应增多。
- **SHM 初始化**：`auto_init_roudi()` 支持指定内存策略；`vlink-check`、`vlink-bench` 与 SHM 单元测试自启 RouDi 时使用 L1。

### 修复

- **QoS 时间约束默认值**：所有内置 `QosProfile` 均默认构造 Deadline 与 Lifespan，使其时长字段统一为 `-1`，避免预设额外施加发布周期约束，并防止跨机时钟偏差导致样本被过早丢弃；自定义 QoS 仍可显式配置有限值。
- **边界正确性**：自定义日志 handler 异常不再越过 `noexcept` 边界，C 风格超长日志不再写入截断 NUL；修复 signed `__int128_t` 构造丢失高 64 位。
- **RPC 超时判定**：请求按绝对截止时间等待并拒绝截止时间之后到达的响应，避免调度延迟导致超时响应被误判为成功。
- **SHM 并发发布**：序列化共享同一地址的 Publisher 对底层发布端口的 loan、归还和发送操作，避免多发布者并发破坏端口状态。
- **Windows 退出挂死**：新增 `Utils::is_terminating()` 探测进程终止阶段；终止期间各模块单例改为泄漏式释放、析构跳过跨线程清理，避免 `ExitProcess` 后 DLL 卸载回调中的锁死锁。

## v2.1.0 (2026/07/26)

### 新增功能

- **日志限频**：C++ 新增线程安全的调用点级 `VLOG_{T,D,I,W,E}_EVERY_MS(interval_ms, ...)` 宏，支持按周期抑制重复日志且不求值被抑制的日志参数。
- **CLI/dump 改名**：`cli/dump` 已彻底改为 `cli/parse`，命令由 `vlink-dump` / `dump` 改为 `vlink-parse` / `parse`，构建开关由 `ENABLE_CLI_DUMP` 改为 `ENABLE_CLI_PARSE`；不提供旧名称兼容入口。
- **零拷贝解析**：新增统一的 C++ / Python `MessageParser`，支持反射解析、零拷贝视图和通用文本渲染；Parse、Viewer、Analyzer、eproto 与 efbs 已接入。
- **PointCloud**：C++ / Python 新增 `get_key_list()` 字段描述接口；Python `Key.type` 返回 `PointCloud.Type` 枚举。
- **CameraFrame**：扩展 Raw、Bayer、YUV 和压缩图像格式，新增编码辅助函数、Python 绑定与自动解码模式，线上枚举值保持兼容。
- **Trigger**：新增 `vlink-trigger`、`TriggerRecorder` 及 Python 绑定，支持触发前后数据缓存、Bag 落盘、历史轮转和完成回调插件。
- **Bag 插件**：新增读会话 `on_reset()`，回放前补全序列化和 Schema 类型。`on_read()` / `on_write()` 改为纯虚函数，并移除版本查询接口；插件仍声明 2.0，但需按新头文件适配并重新构建。
- **Viewer 感知显示**：新增可配置的 Protobuf / FlatBuffers 消息映射，覆盖目标、雷达、车道、轨迹、停车位、占据栅格和车辆 HUD。
- **原生 DDS CDR**：支持 FastDDS IDL、ROS 2 消息及按类型名加入 Topic，并支持 CDR 录制、回放和发现上报。

### 改进

- **工程布局**：C / Python 绑定源码迁至 `languages/c_api/` 与 `languages/python_api/`；公开构建开关、CMake target、库名和安装布局保持不变。
- **接收端 loan API 移除**：删除 C++ / Python 的 `set_manual_unloan()` 与 `is_manual_unloan()`；接收缓冲统一在回调后自动归还，发布侧 `loan()` / `return_loan()` 不受影响。
- **传输后端移除**：删除 `modules/ddst` 与 `modules/qnx`，不再支持 `ddst://` 和 `qnx://`；同步移除 `DdstConf` / `QnxConf`、对应 `TransportType` 与启用标志、构建选项、CLI / 补全入口、Python 枚举、测试和文档。QNX 目标操作系统的工具链、原生日志及平台适配不受影响。
- **公共 API**：`TerminalStream` 移至 base，并统一 Bag / Trigger 接口命名；旧接口不再保留。
- **URL 插件**：扩展 `VLINK_URL_PLUGINS`，支持按需自动加载 transport、关闭加载或显式预加载；运行时包可独立完成动态加载。
- **Bag**：公开幂等的 `BagWriter::close()`，并由 `Config::sync_mode` 统一帧、Schema 和插件输出的写入模式。逐调用 `immediate` 参数及受保护 `record()` 签名已调整，外部派生类和既有二进制需适配并重新构建。
- **Bag 性能**：优化异步写入、回放过滤、Schema 持久化和未启用录制时的热路径。
- **基础库**：优化 MemoryPool 和任务调度；`MemoryPool::Config` 新增 `batch_size`，使用该配置的外部 C++ 二进制需重新构建。
- **序列化**：新增 `Serializer::serialize_to_transport()`，修复 transport loan 生命周期并加强边界校验；C 字符串改为仅支持发送，接收端须使用 `std::string`。
- **CDR 数据面**：统一使用含 encapsulation header 的字节负载，接收侧在回调期间直接借用 FastDDS payload；C API、Python API 与构建宏同步支持 CDR / ROS 2。
- **接收回调**：每次投递使用独立值对象，裸 Protobuf 指针也在 Arena 中逐条分配，避免嵌套回调覆盖。
- **Zenoh**：原生 Key Expression 增加 domain / event 隔离；采用该寻址的新旧版本不能互通，升级时需同步通信两端。未设置环境变量时沿用配置文件默认值；zenoh-pico 构建要求启用 local subscriber、local queryable、liveliness 和 matching。
- **Viewer / Webviz**：改进多相机解码、感知帧合并、动态 Schema 解析及多种图像格式在 Foxglove / Rerun 中的展示。
- **Parse**：收紧参数校验，优化 slice / scan 的解析时机、频率限制和同步写入路径，并保证质量与 manifest 输出稳定。
- **eproto / efbs**：兼容较新 Protobuf 版本，统一默认标量、字段顺序、map 排序和 JSON 输出行为。
- **构建与基准**：修正无 intra 构建和 lcov 2.x 排除规则；benchmark 可按 payload 调整 shm2 配置，并改进延迟统计与报告分组。

### 修复

- **Bytes 压缩**：修复错误地将 1 MiB LZAV 哈希缓存大小当作压缩输入上限的问题。
- **DDS**：节点释放时清理回调、请求状态和匹配计数，支持同进程重新初始化；安全处理无数据样本，修复 FastDDS 3 与 CycloneDDS 的内置 Raw 类型互操作，同时修正 DDSR liveliness 配置及无 SSL DDSC 的 TCP 误启用。
- **DDS CDR**：修复类型名兼容、运行中切换类型、RPC 类型一致性、ROS 2 空消息、接收 payload loan 生命周期及旧版 FastDDS 变长 payload 扩容。
- **SHM / SHM2**：回复路径现在返回真实发送结果；SHM2 正确应用 iceoryx 配置，优化大消息 loan、listener 等待和节点生命周期处理。
- **VDB / VCAP**：修复多次回放、停止或跳转后的重排缓存污染；新会话通过 `on_reset()` 清理旧状态，并正确处理自然完成与中断边界。
- **Bag / Trigger**：修复退出崩溃、队列满时丢写、延迟写入错误未上报，以及已放弃的 Trigger dump 残留或重现。
- **Parse**：修复插件解绑死锁、live 资源生命周期、slice / scan 边界遗漏、输入输出碰撞、Schema 插件路径加载和 proxy 包大小处理。
- **图像与 Schema**：加强异常图像尺寸处理；在 efbs、Parse、Viewer 和 Webviz 解码前验证 FlatBuffers Schema 与 payload。
- **Viewer / Webviz**：修复动态 Schema 生命周期、缺失字段默认值、慢客户端队列增长、Rerun 多格式路由及录制关闭错误上报。
- **Intra**：修复回调内注销、嵌套投递、同 pipeline RPC 死锁和跨 event 误连接。
- **Python**：修复状态字典类型读取、零拷贝解析器 use-after-free 和 `vlink` 模块路径；`Bytes` 支持负索引。
- **基础库**：修复 `MemoryResource::is_equal()` 与外部 PMR resource 比较时的未定义行为。

## v2.0.0 (2025/07/01)

- 初始化源码。
