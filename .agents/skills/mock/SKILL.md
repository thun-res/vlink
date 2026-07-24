---
name: mock
description: >-
  按功能编排 VLink 入口级 mock 集成测试,覆盖普通 doctest 难以触及的
  全部 CLI 命令、Proxy、Viewer/Player/Analyzer、WebViz 及 bag 数据集
  场景。用户要求"跑 mock"、"补测试死角"、"测试所有命令行"、"用 bag
  数据集测试 viewer/webviz/proxy"或做发布前入口冒烟测试时使用。
---

# 入口级 Mock 集成测试

本 skill 验证真实可执行入口及其协作链,不替代 `/test`、`/asan` 或
`/bench`。按用户指定模块执行;未指定时先根据当前 diff 选择受影响模块,
不得默认把所有 GUI、网络和长时任务一次性全跑。

## 1. 功能路由

只读取并执行本次需要的分册:

| 模块 | 分册 | 主要目标 |
| ---- | ---- | -------- |
| CLI | [CLI.md](references/CLI.md) | 10 个 `cli/*` 工具及全部子命令 |
| Proxy | [PROXY.md](references/PROXY.md) | `vlink-proxy`、发现、转发与进程生命周期 |
| Viewer | [VIEWER.md](references/VIEWER.md) | `vlink-viewer`、`vlink-player`、`vlink-analyzer` |
| WebViz | [WEBVIZ.md](references/WEBVIZ.md) | Foxglove、Rerun、bag2mcap、bag2rrd |
| Bag 数据集 | [BAG-DATASETS.md](references/BAG-DATASETS.md) | 数据集选择、只读保护、正常/异常场景 |

## 2. 执行流程

1. 先读仓库根 `AGENTS.md`、`.agents/REPO-REFERENCE.md`,再读上表中选中的
   分册及其指向的 `doc/` 小节。
2. 明确模块、平台、构建目录、二进制目录、bag 数据集和允许的外部依赖。
   用户未提供数据集时只执行不依赖数据集的场景,并把其余项标为未覆盖。
3. 需要构建时只能使用仓库根 `build-ai/skill_mock_<function_name>`,例如
   `build-ai/skill_mock_cli`、`build-ai/skill_mock_proxy`、
   `build-ai/skill_mock_viewer` 或 `build-ai/skill_mock_webviz`;固定
   `ENABLE_CXX_STD_20=OFF`,只开启目标模块
   并构建所需 target。执行 Python、CMake、测试或辅助脚本前设置
   `PYTHONPYCACHEPREFIX={project}/build-ai/skill_mock_<function_name>/__pycache__`。
   不得复用、删除或重置维护者、IDE、CI 的构建目录。
4. 为本次运行创建独立临时目录。输入数据集始终只读;`clone`、`fix`、
   `reindex`、`tag`、录制和导出等写操作只针对临时副本或临时输出。
5. 先做参数解析和预期失败,再做单进程入口,最后做需要多个进程、GUI、
   端口或数据回放的场景。每个后台进程都要记录 PID、日志和就绪证据,
   结束时先正常终止,只清理本次创建的资源。
6. 失败后保留首个有效错误、命令、退出码、日志和产物;不得靠重试、
   放宽退出码、扩大排除项或更换数据集把失败改写为通过。

显式调用本 skill 授权选定模块的本地配置、构建、参数解析、预期失败、
单进程只读运行和临时文件写入。实时回放、联网、GUI、长驻进程与可变
数据场景必须在用户明确选中对应功能后执行;不授权上传、发布、修改原始
数据集、绑定公网地址、安装系统依赖或终止非本次启动的进程。

## 3. 结果报告

按功能模块分别列出:

- 平台、构建目录、target、二进制版本和数据集标识;
- 场景总数及通过/失败/未覆盖数;
- 每个场景的入口、关键参数、期望与实际退出码、关键输出或产物;
- GUI/网络/外部服务/缺失 schema 导致的未覆盖项;
- 首个失败的复现命令、日志路径和输入数据集副本路径。

只有实际运行并核对结果的场景才可写"通过";仅 `--help` 成功不能代表
对应业务子命令或 GUI 数据路径已覆盖。
