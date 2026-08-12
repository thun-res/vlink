# VLink 功能平铺索引

整个 VLink 的功能逐项展开,每项给出**代码入口**与**文档小节**指向,供
agent 渐进式披露:先在本表定位功能 → 再读对应 doc 小节 → 最后进代码。
本表只指向,不解释;语义以 doc 正文为准。

## 1. 通信模型

| 功能 | 代码入口 | 文档小节 |
| ---- | ---- | ---- |
| 模型总览与三模型选型 | `include/vlink/vlink.h` | `doc/02` §2.1;`doc/00-overview.md` §0.3/§0.9 |
| 节点基础与生命周期(`Node<ImplT, SecT>`) | `include/vlink/node.h` | `doc/02` §2.2;节点公共方法速查 `doc/14` §14.4 |
| 事件模型:发布/订阅(1 对多) | `include/vlink/publisher.h`、`include/vlink/subscriber.h` | `doc/02` §2.3 |
| 方法模型:RPC 请求/响应 | `include/vlink/client.h`、`include/vlink/server.h` | `doc/02` §2.4 |
| 字段模型:状态同步(最新值) | `include/vlink/setter.h`、`include/vlink/getter.h` | `doc/02` §2.5 |
| 原语构造与核心 API 速查 | 六个通信原语头 | `doc/14` §14.3;编程骨架 §14.1 |

## 2. 序列化

| 功能 | 代码入口 | 文档小节 |
| ---- | ---- | ---- |
| 机制:编译期按消息类型自动推导 | `include/vlink/serializer.h` | `doc/03` §3.1 |
| 类型族选型:Bytes/Dynamic/SOMEIP/CDR/Proto/ProtoPtr/FlatTable/FlatPtr/FlatBuilder/Custom/String/Chars/Standard(POD)/StandardPtr/Stream | 同上 | `doc/03` §3.2;速查 `doc/14` §14.7 |
| Protobuf 接入 | 同上 | `doc/03` §3.3 |
| FlatBuffers 与 POD | 同上 | `doc/03` §3.4 |
| SOME/IP 宏结构与 AUTOSAR non-TLV 部署边界 | `include/vlink/impl/someip_serializer.h` | `doc/03` §3.5.1 |
| 自定义序列化器 | `include/vlink/serializer.h` | `doc/03` §3.5.2 |
| DDS CDR 与 DynamicData | 同上 + `modules/dds*` | `doc/03` §3.6 |
| 边界条件与常见错误 | — | `doc/03` §3.7 |
| IDL 代码生成 `vlink_generate_cpp()` | `cmake/` | `doc/01` §1.6 |

## 3. 传输后端与 URL

| 功能 | 代码入口 | 文档小节 |
| ---- | ---- | ---- |
| URL 契约(`<scheme>://<topic>[?参数]`) | — | `doc/04` §4.1;`doc/00-overview.md` §0.2 |
| 后端选型与能力矩阵 | — | `doc/04` §4.3/§4.4 |
| 高频查询参数(通用配置) | `include/vlink/modules/*_conf.h` | `doc/04` §4.5 |
| 各后端接入要点:intra / shm / shm2(Iceoryx 系)/ dds(Fast-DDS) / ddsc / ddsr / zenoh / someip(vsomeip) / fdbus / mqtt | `modules/<名>/` + `include/vlink/modules/` | `doc/04` §4.6 |
| 后端混合、桥接与 URL 重映射 | `src/extension/url_remap.cc` | `doc/04` §4.7 |
| 高频环境变量 | — | `doc/14` §14.12;`doc/13` 环境变量节 |

## 4. QoS

| 功能 | 代码入口 | 文档小节 |
| ---- | ---- | ---- |
| 作用域与生效模型 | `include/vlink/extension/qos.h`(Doxygen 范本) | `doc/05` §5.1 |
| 预定义 Profile 选型 | `include/vlink/extension/qos_profile.h` | `doc/05` §5.3 |
| 子策略语义 / 自定义 Qos / 设置入口 | `include/vlink/extension/qos.h` | `doc/05` §5.4–§5.6 |
| 各模型 QoS 示例、DDS 兼容性约束 | `examples/qos/` | `doc/05` §5.7/§5.8;速查 `doc/14` §14.6 |

## 5. 零拷贝

| 功能 | 代码入口 | 文档小节 |
| ---- | ---- | ---- |
| 两层机制(容器 + 传输层 loan) | `include/vlink/zerocopy/` | `doc/06` §6.1/§6.10 |
| 通用容器接口 | 同上 | `doc/06` §6.2 |
| CameraFrame(图像/编码视频) | 同上 | `doc/06` §6.3 |
| PointCloud(带 Schema 点云) | 同上 | `doc/06` §6.4 |
| OccupancyGrid(2D 占据/代价地图) | 同上 | `doc/06` §6.5 |
| Tensor(N 维张量) | 同上 | `doc/06` §6.6 |
| ObjectArray(3D 检测/跟踪目标) | 同上 | `doc/06` §6.7 |
| AudioFrame(PCM/编码音频) | 同上 | `doc/06` §6.8 |
| RawData(自定义二进制) | 同上 | `doc/06` §6.9 |
| ProxyData(跨进程代理数据) | `include/vlink/zerocopy/proxy_data.h` | `doc/06` §6.9 |
| MessageParser(零拷贝消息解析) | `include/vlink/zerocopy/message_parser.h` | `doc/06` §6.9.1 |
| 生命周期约束、与裸 Bytes 对照 | `include/vlink/zerocopy/` | `doc/06` §6.11/§6.12 |
| ⚠ 线格式冻结规则(sizeof 即契约) | — | `.agents/languages/CPP.md` §9.3 |

## 6. 安全

| 功能 | 代码入口 | 文档小节 |
| ---- | ---- | ---- |
| 消息级加密机制与数据流 | `include/vlink/extension/security.h` | `doc/07` §7.1 |
| 节点变体(`SecurityType::kWithSecurity`、Security 六原语) | `include/vlink/impl/types.h`、六个通信原语头 | `doc/07` §7.3 |
| `Security::Config` 三种模式(对称 key/passphrase、RSA 公私钥、自定义 encrypt/decrypt 回调) | `include/vlink/extension/security.h` | `doc/07` §7.4/§7.5 |
| 约束、编译开关(`ENABLE_SECURITY`)、不支持的传输 | 根 `CMakeLists.txt` | `doc/07` §7.6/§7.7 |
| 传输层 TLS、性能特征 | — | `doc/07` §7.8/§7.9;速查 `doc/14` §14.9 |

## 7. 基础库（base）

| 组件 | 代码入口 | 文档小节 |
| ---- | ---- | ---- |
| 组件索引 | `include/vlink/base/` | `doc/08` §8.1 |
| Logger 日志系统(含日志宏) | `include/vlink/base/` | `doc/08` §8.2;宏速查 `doc/14` §14.8 |
| Bytes 字节载体(shallow_copy 语义) | `include/vlink/base/` | `doc/08` §8.3 |
| 内存池与 PMR 接入 | `include/vlink/base/` | `doc/08` §8.4 |
| Function 与 MoveFunction | `include/vlink/base/functional.h`(注释范本) | `doc/08` §8.5 |
| MessageLoop 消息循环 | `include/vlink/base/` | `doc/08` §8.6 |
| 定时器 Timer | `include/vlink/base/` | `doc/08` §8.7 |
| ThreadPool 与 MultiLoop 并行 | `include/vlink/base/` | `doc/08` §8.8 |
| 可追踪任务与协作取消 | `include/vlink/base/graph_task.h` 等 | `doc/08` §8.9 |
| Process 子进程管理 | `include/vlink/base/` | `doc/08` §8.10 |
| 并发工具 / 跨进程 IPC / 任务调度 | `include/vlink/base/condition_variable.h` 等 | `doc/08` §8.11–§8.13 |
| CPU 利用率、Uuid、Coroutine | `include/vlink/base/coroutine.h` 等 | `doc/08` §8.14–§8.16 |
| 宏:`VLIKELY`/`VUNLIKELY`、C++20 门控 | `include/vlink/base/macros.h` | `.agents/languages/CPP.md` §7/§9 |

## 8. 录制与回放

| 功能 | 代码入口 | 文档小节 |
| ---- | ---- | ---- |
| 概念与数据模型、存储格式(含 `ENABLE_SQLITE`/`ENABLE_ZSTD`) | `include/vlink/extension/`(BagWriter/BagReader) | `doc/09` §9.1/§9.2 |
| 录制与录制配置 | 同上 | `doc/09` §9.3/§9.4 |
| 回放、参数控制、游标顺序读取、元数据 | 同上 | `doc/09` §9.5–§9.8 |
| 与通信 API 集成、进程级自动录制 | 同上 | `doc/09` §9.9/§9.10 |
| MCAP 与 Foxglove、多文件合并回放 | 同上 | `doc/09` §9.11/§9.12 |
| 触发录制与内存打点 | 同上 + `cli/trigger/` | `doc/09` §9.13 |

## 9. 服务发现与代理监控

| 功能 | 代码入口 | 文档小节 |
| ---- | ---- | ---- |
| 服务发现:机制、核心 API、FilterType、快照结构、传输状态事件 | `include/vlink/extension/discovery_viewer.h`、`proxy/` | `doc/12` §12.1–§12.6 |
| 服务发现 CLI 入口 | `cli/list/`、`cli/info/` | `doc/12` §12.7 |
| 代理监控:组件模型与 ProxyAPI(继承式扩展) | `include/vlink/external/proxy_api.h`(注释范本) | `doc/12` §12.8–§12.11 |
| vlink-proxy 命令行 / ProxyServer 嵌入式 | `proxy/` | `doc/12` §12.12/§12.13 |
| 安全与版本兼容、话题过滤、跨网段部署、录制回放、CMake 集成、排错 | 同上 | `doc/12` §12.14–§12.19 |

## 10. 语言绑定与集成

| 功能 | 代码入口 | 文档小节 |
| ---- | ---- | ---- |
| 纯 C API(多语言集成,`ENABLE_C_API`) | `languages/c_api/` + `include/vlink/external/c_api.h` | `doc/13` §13.1–§13.8 |
| Python API(`ENABLE_PYTHON_API`) | `languages/python_api/vlink_python.cc` + `languages/python_api/vlink.py` | `doc/13` §13.7.1/§13.8.3 |
| UrlRemap 与 DynamicData | `include/vlink/extension/url_remap.h`、`include/vlink/extension/dynamic_data.h` | `doc/13` §13.10/§13.11 |
| 动态库 Plugin 加载器 | `include/vlink/base/plugin.h` | `doc/13` §13.12/§13.13 |
| Logger 插件接口 | `include/vlink/base/logger_plugin_interface.h` | `doc/13` §13.14 |
| Runable/Convert/Schema 插件接口 | `include/vlink/extension/*_plugin_interface.h` | `doc/13` §13.15–§13.17 |
| Bag/Trigger 插件与 DiscoveryReporter | `include/vlink/extension/` 对应接口头 | `doc/13` §13.9 |
| ConfPluginInterface(已有 scheme 的外部 Conf 工厂) | `include/vlink/impl/conf_plugin_interface.h` | `doc/13` §13.17 后说明 |
| 自定义传输后端 | `modules/`(参照既有后端) | `doc/04` §4.6;`doc/15` §15.18.4 |
| 运行时环境变量 | — | `doc/13` §13.18–§13.27 |
| CMake 集成(`find_package(vlink)` + `vlink::vlink`) | 根 `CMakeLists.txt` | `doc/01` §1.5;速查 `doc/14` §14.13 |
| 表达式引擎(`vlink::exprtk_api`,PIMPL 共享库) | `include/vlink/external/exprtk_api.h` + `exprtk/` | `doc/01` §1.4.3;`doc/10` §10.2.9(API 暂无专节) |

## 11. 构建与发布

| 功能 | 代码入口 | 文档小节 |
| ---- | ---- | ---- |
| VKit 构建(官方推荐) | github.com/thun-res/vkit | `doc/01` §1.1 |
| standalone CMake / 环境依赖 / 构建选项(主要 `ENABLE_*`) | 根及子目录 CMake 文件 | `doc/01` §1.2–§1.4 |
| Conan / Soong / vcpkg / 交叉编译 / 安装与打包发布 | `conanfile.py`、`Android.bp`、`tools/vcpkg/`、`packup/`、`tools/` | `doc/01` §1.7–§1.10 |
| 构建与运行问题排查 | — | `doc/01` §1.13 |

## 12. CLI 工具（`cli/<名称>/`）

总览与选型 `doc/10` §10.1,各命令参数 §10.2,速查 `doc/14` §14.11:
bag(录制包)、bench(基准,详见 `/bench` skill)、check(诊断)、
efbs / eproto(FlatBuffers/Proto 编辑)、info、list、monitor(TUI 监控)、
parse、trigger(触发录制)。

## 13. 可视化

| 功能 | 代码入口 | 文档小节 |
| ---- | ---- | ---- |
| 桌面 Viewer 套件(Qt,含 `viewer/perception/` 感知可视化) | `viewer/` | `doc/11` §11.1 |
| WebViz(Foxglove / Rerun) | `webviz/foxglove/`、`webviz/rerun/` | `doc/11` §11.2 |

## 14. 排障与速查（`doc/14`）

编程骨架 §14.1 → 常用代码模式 §14.14 → 示例目录 §14.15 → 诊断流程
§14.16;专项:收不到数据 §14.17、跨机/容器不连通 §14.18、共享内存
loan 失败 §14.19、构造抛异常/链接失败 §14.20、性能抖动 §14.21、排障
API §14.22、Bag/C API/安全排障 §14.23、错误文本反查 §14.24。
提交问题前的信息收集见 §14.25。

## 15. 测试与贡献（`doc/15`）

测试体系 §15.1–§15.7(agent 硬约定另见 `.agents/testing/UNIT-TESTS.md`);提交流程
§15.8–§15.12(分支/Commit/PR 规范);代码风格与静态检查 §15.13/§15.14;
文档同步规则 §15.16(配套 `/sync-review` skill);API/ABI 兼容与 SemVer
§15.17;评审与禁止事项 §15.19。
