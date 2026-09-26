# 仓库参考

改动任何目录前,先据此表定位其职责与对应的 `doc/` 章节;产品级说明一律
去读文档章节,本表只做指向。找具体功能用 `FEATURE-INDEX.md`。

## 1. 用户文档（`doc/`，产品知识的唯一出处）

| 章节 | 主题 | 章节 | 主题 |
| ---- | ---- | ---- | ---- |
| `00-overview.md` | 概述与设计哲学 | `08-base-library.md` | 基础库 |
| `00-whitepaper.md` | 技术白皮书(行文范文) | `09-recording.md` | 录制与回放 |
| `01-started.md` | 快速上手 | `10-cli-tools.md` | CLI 工具 |
| `02-communication.md` | 通信模型 | `11-visualization.md` | 可视化 |
| `03-serialization.md` | 消息序列化 | `12-observability.md` | 服务发现与代理监控 |
| `04-transport.md` | 传输后端与 URL | `13-integration.md` | 集成与扩展 |
| `05-qos.md` | QoS 配置 | `14-reference.md` | 速查与故障排查 |
| `06-zerocopy.md` | 零拷贝 | `15-contributing.md` | 测试与贡献规范 |
| `07-security.md` | 安全加密 | | |

## 2. 核心代码

| 目录 | 职责 | 对应文档 |
| ---- | ---- | -------- |
| `include/vlink/*.h` | 六大通信原语 + `node.h`、`serializer.h`、`version.h` 与总入口 `vlink.h` | `doc/01-started.md`、`doc/02-communication.md`、`doc/03-serialization.md` |
| `include/vlink/base/` | 基础库:Logger、MessageLoop、Timer、ThreadPool、Bytes、宏(`macros.h`,含 `VLIKELY`) | `doc/08-base-library.md` |
| `include/vlink/extension/` | QoS、Security、Bag、发现/状态、动态数据、SOME/IP 序列化 codec 与插件接口等扩展 | `doc/03-serialization.md`、`doc/05-qos.md`、`doc/07-security.md`、`doc/09-recording.md`、`doc/12-observability.md`、`doc/13-integration.md` |
| `include/vlink/modules/` | 各后端 Conf(DdsConf、ShmConf、ZenohConf 等) | `doc/04-transport.md` |
| `include/vlink/zerocopy/` | CameraFrame、PointCloud、Tensor 等零拷贝容器;**线格式冻结,见 languages/CPP.md §9.3** | `doc/06-zerocopy.md` |
| `include/vlink/external/` | 纯 C API、代理监控/嵌入式代理 API、表达式引擎 API | `doc/13-integration.md`、`doc/12-observability.md`;表达式用法见 `doc/01-started.md`、`doc/10-cli-tools.md` |
| `include/vlink/internal/`、`include/vlink/impl/` | 实现路径,不作为首选直接 include 入口;进入公开签名的类型仍按公开契约维护 | 随对应公开 API 章节 |
| `src/` | 库实现(base/extension/impl/private/zerocopy) | 随对应头文件章节 |
| `modules/` | 后端实现:intra、shm、shm2、dds、ddsc、ddsr、fdbus、mqtt、someip、zenoh | `doc/04-transport.md` |

## 3. 周边

| 目录 | 职责 | 对应文档 |
| ---- | ---- | -------- |
| `cli/` | CLI 工具:bag、bench、check、efbs、eproto、info、list、monitor、parse、trigger | `doc/10-cli-tools.md` |
| `languages/c_api/` / `languages/python_api/` | C / Python 绑定(python:`vlink_python.cc` 注册入口 + 各功能 `.cc` + `vlink.py`) | `doc/13-integration.md` |
| `proxy/` | 服务发现与代理监控 | `doc/12-observability.md` |
| `viewer/`、`webviz/` | 可视化(Qt Viewer / Foxglove / Rerun) | `doc/11-visualization.md` |
| `examples/` | 分主题示例(quickstart、communication、qos、security、zerocopy 等);编码专属规则见 languages/CPP.md §12 | `doc/01-started.md` |
| `test/` | doctest 单元测试,见 `.agents/testing/UNIT-TESTS.md` | `doc/15-contributing.md` |
| `exprtk/` | 表达式引擎封装(`vlink::exprtk_api`,PIMPL 共享库) | `doc/01-started.md`、`doc/10-cli-tools.md`(API 无专节) |
| `cmake/`、`conanfile.py`、`Android.bp`、`tools/vcpkg/`、`packup/` | CMake/Conan/Soong/vcpkg 构建、安装与打包 | `doc/01-started.md`;运行时变量另见 `doc/13-integration.md` |
| `tools/` | 维护脚本:`format.sh`、`check.sh`、`update_version.sh`、平台脚本 | — |
| `.github/workflows`、`.github/scripts` | CI、发布与对应 runner 脚本 | — |
| `.github/copilot-instructions.md` | GitHub 原生 Copilot 的仓库级规则入口 | `AGENTS.md`、`.agents/README.md` |
| `doc/` | 用户文档 00–15 章(中文)+ Doxygen 配置 | 自身 |

## 4. 工程设施

| 位置 | 内容 |
| ---- | ---- |
| `.clang-format` / `.clang-tidy` | 格式与静态检查配置;误报只按 CPP.md §11 做最小范围 NOLINT |
| `tools/` | 维护脚本;版本同步入口为 `update_version.sh`,脚本编码规则见 `languages/SHELL.md`/`BATCH.md`/`POWERSHELL.md` 并保持相邻入口接口 |
| `.github/workflows/` | CI 与发布工作流;触发矩阵见 `/cicd` skill |
| `.github/scripts/` | CI、打包、发布与通知脚本 |
| `.github/copilot-instructions.md` | 原生 Copilot Agent/Review 的仓库指令,继续按根 `AGENTS.md` 路由 |
| `.github/wiki/` | GitHub Wiki 落地页(`index.html`),与 `doc/` 同源,同步规则见 `DOCS-AND-FORMATTING.md` |
| `.agents/cache-report/` | 本地评审/审计报告缓存,已忽略提交 |
| `cmake/functions/common.cmake` | `vlink_test_sanitize` / `vlink_test_coverage` 等构建函数 |
| 根及子目录 `CMakeLists.txt` / `cmake/*.cmake` | 顶层主要 `ENABLE_*`、子项目开关与测试/覆盖率接线 |
| `version.txt` + 版本镜像 | 以 `version.txt` 为源,用 `tools/update_version.sh` 同步头文件、包清单、README 徽章与 CHANGELOG |

## 5. 相关仓库

- `github.com/thun-res/vlink` — 本仓库
- `github.com/thun-res/vkit` — 官方构建工具(对外推荐的构建入口)
- `github.com/thun-res/vmsgs` — 消息定义库
