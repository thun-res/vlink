# 🗒️ 更新日志

## v2.1.0 (2026/07/18)

### 新增功能

- **日志限频**：C++ 新增线程安全的调用点级 `VLOG_{T,D,I,W,E}_EVERY_MS(interval_ms, ...)` 宏，支持按周期抑制重复日志且不求值被抑制的日志参数。
- **CLI/dump 改名**：`cli/dump` 已彻底改为 `cli/parse`，命令由 `vlink-dump` / `dump` 改为 `vlink-parse` / `parse`，构建开关由 `ENABLE_CLI_DUMP` 改为 `ENABLE_CLI_PARSE`；不提供旧名称兼容入口。
- **零拷贝解析**：新增统一的 C++ / Python `MessageParser`，支持反射解析、零拷贝视图和通用文本渲染；Parse、Viewer、Analyzer、eproto 与 efbs 已接入。
- **CameraFrame**：扩展 Raw、Bayer、YUV 和压缩图像格式，新增编码辅助函数、Python 绑定与自动解码模式，线上枚举值保持兼容。
- **Trigger**：新增 `vlink-trigger`、`TriggerRecorder` 及 Python 绑定，支持触发前后数据缓存、Bag 落盘、历史轮转和完成回调插件。
- **Bag 插件**：新增读会话 `on_reset()`，回放前补全序列化和 Schema 类型。`on_read()` / `on_write()` 改为纯虚函数，并移除版本查询接口；插件仍声明 2.0，但需按新头文件适配并重新构建。
- **Viewer 感知显示**：新增可配置的 Protobuf / FlatBuffers 消息映射，覆盖目标、雷达、车道、轨迹、停车位、占据栅格和车辆 HUD。

### 改进

- **接收端 loan API 移除**：删除 C++ / Python 的 `set_manual_unloan()` 与 `is_manual_unloan()`；接收缓冲统一在回调后自动归还，发布侧 `loan()` / `return_loan()` 不受影响。
- **传输后端移除**：删除 `modules/ddst` 与 `modules/qnx`，不再支持 `ddst://` 和 `qnx://`；同步移除 `DdstConf` / `QnxConf`、对应 `TransportType` 与启用标志、构建选项、CLI / 补全入口、Python 枚举、测试和文档。QNX 目标操作系统的工具链、原生日志及平台适配不受影响。
- **公共 API**：`TerminalStream` 移至 base，并统一 Bag / Trigger 接口命名；旧接口不再保留。
- **URL 插件**：扩展 `VLINK_URL_PLUGINS`，支持按需自动加载 transport、关闭加载或显式预加载；运行时包可独立完成动态加载。
- **Bag**：公开幂等的 `BagWriter::close()`，并由 `Config::sync_mode` 统一帧、Schema 和插件输出的写入模式。逐调用 `immediate` 参数及受保护 `record()` 签名已调整，外部派生类和既有二进制需适配并重新构建。
- **Bag 性能**：优化异步写入、回放过滤、Schema 持久化和未启用录制时的热路径。
- **基础库**：优化 MemoryPool 和任务调度；`MemoryPool::Config` 新增 `batch_size`，使用该配置的外部 C++ 二进制需重新构建。
- **序列化**：新增 `Serializer::serialize_to_transport()`，修复 transport loan 生命周期，并加强字符串、Schema 和 payload 的边界校验。
- **Zenoh**：原生 Key Expression 增加 domain / event 隔离；采用该寻址的新旧版本不能互通，升级时需同步通信两端。未设置环境变量时沿用配置文件默认值；zenoh-pico 构建要求启用 local subscriber、local queryable、liveliness 和 matching。
- **Viewer / Webviz**：改进多相机解码、感知帧合并、动态 Schema 解析及多种图像格式在 Foxglove / Rerun 中的展示。
- **Parse**：收紧参数校验，优化 slice / scan 的解析时机、频率限制和同步写入路径，并保证质量与 manifest 输出稳定。
- **eproto / efbs**：兼容较新 Protobuf 版本，统一默认标量、字段顺序、map 排序和 JSON 输出行为。
- **构建与基准**：修正无 intra 构建和 lcov 2.x 排除规则；benchmark 可按 payload 调整 shm2 配置，并改进延迟统计与报告分组。

### 修复

- **Bytes 压缩**：修复错误地将 1 MiB LZAV 哈希缓存大小当作压缩输入上限的问题。
- **DDS**：节点释放时清理回调、请求状态和匹配计数，支持同进程重新初始化；同时修正 DDSR liveliness 配置及无 SSL DDSC 的 TCP 误启用。
- **SHM / SHM2**：回复路径现在返回真实发送结果；SHM2 正确应用 iceoryx 配置，优化大消息 loan、listener 等待和节点生命周期处理。
- **VDB / VCAP**：修复多次回放、停止或跳转后的重排缓存污染；新会话通过 `on_reset()` 清理旧状态，并正确处理自然完成与中断边界。
- **Bag / Trigger**：修复退出崩溃、队列满时丢写、延迟写入错误未上报，以及已放弃的 Trigger dump 残留或重现。
- **Parse**：修复插件解绑死锁、live 资源生命周期、slice / scan 边界遗漏、输入输出碰撞、Schema 插件路径加载和 proxy 包大小处理。
- **图像与 Schema**：加强异常图像尺寸处理；在 efbs、Parse、Viewer 和 Webviz 解码前验证 FlatBuffers Schema 与 payload。
- **Viewer / Webviz**：修复动态 Schema 生命周期、缺失字段默认值、慢客户端队列增长、Rerun 多格式路由及录制关闭错误上报。
- **Python**：修复状态字典的错误派生类型读取，以及零拷贝解析器借用临时缓冲导致的 use-after-free。
- **基础库**：修复 `MemoryResource::is_equal()` 与外部 PMR resource 比较时的未定义行为。

## v2.0.0 (2025/07/01)

- 初始化源码。
