---
name: test
description: >-
  按 Linux、macOS 或 Windows 的仓库测试链路配置并构建普通
  vlink-test,运行完整 CTest 或指定 suite/case 并汇总失败。用户要求
  "跑 test"、"执行单元测试"、"只测某个 suite/case"、"验证测试是否
  通过"或排查非 ASan 的 ci-test 失败时使用。
---

# 普通单元测试

本 skill 执行不含 ASan 和覆盖率插桩的普通单元测试。内存检测使用
`/asan`,覆盖率报告使用 `/coverage`。只在用户显式调用本 skill 时执行
构建和测试。

## 1. 选择执行路径

只加载当前任务需要的分册:

| 功能 | 必读分册 |
|---|---|
| 配置或重新构建普通测试 | [CONFIGURE-AND-BUILD.md](references/CONFIGURE-AND-BUILD.md) |
| 运行完整 CTest | [FULL-TEST.md](references/FULL-TEST.md) |
| 列举、运行或排除指定 suite/case | [TARGETED-TEST.md](references/TARGETED-TEST.md) |
| 排查配置、构建、Proxy 或测试失败 | [FAILURE-DIAGNOSIS.md](references/FAILURE-DIAGNOSIS.md) |

已有兼容且最新的构建产物时,可以只加载运行分册。未指定范围时运行完整
CTest;用户指定 suite/case 时只运行该范围,不得擅自扩大为全量测试。

## 2. 通用流程

1. 从任意子目录解析仓库根目录,确认当前平台和用户要求的测试范围。
2. 使用仓库根 `build-ai/skill_test` 配置普通测试,明确关闭 ASan 和覆盖率。
3. 构建 `vlink-test` 和测试链路必需的 `vlink-proxy`。
4. 全量测试复用平台 CI runner;定向测试按对应功能分册执行。
5. 汇总实际执行范围、通过数、失败数、跳过项和首个有效错误。

- 配置或构建失败时原样报告首个错误,不得继续使用可能陈旧的二进制宣称
  测试通过。
- 不得复用、删除或重置维护者、IDE、CI 的构建目录。既有
  `build-ai/skill_test` 的生成器或开关不兼容时,使用描述具体任务的
  `build-ai/skill_test_<task_name>`。传给平台 runner 的 `BUILD_DIR` 必须是对应的
  仓库根相对路径。
- 测试失败时列出失败的 CTest suite、首个有效错误和对应日志位置;不要
  只返回总退出码。
- 完成后报告平台、构建目录、关键开关和测试范围。后端因外部服务不可用
  而跳过时不得写成已覆盖。
- 定向测试只能得出指定范围的结论,不得写成“单元测试全部通过”。
