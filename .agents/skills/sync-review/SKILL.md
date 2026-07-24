---
name: sync-review
description: >-
  审计 VLink 代码、doc、Doxygen、Python/C API、测试、示例、Wiki、
  Agent 索引、版本与 CHANGELOG 的同步关系,定位 API 或功能变更后的
  遗漏和过时描述。公开 API、功能入口或文档结构变化时必须使用;用户
  要求"检查文档同步"、"API 改了查遗漏"、"同步审计"或发版前核对时使用。
---

# 全仓同步审计

代码定义行为,`doc/` 定义产品与架构语义;Doxygen、绑定、测试、示例、
Wiki、Agent 索引和变更摘要均须与对应事实同步。

## 1. 功能路由

只加载当前审计需要的分册:

| 功能 | 必读分册 |
|---|---|
| 确定全量/增量范围和事实基准 | [CHANGE-SCOPE.md](references/CHANGE-SCOPE.md) |
| 核对 API、绑定、文档、测试与示例 | [API-SURFACES.md](references/API-SURFACES.md) |
| 核对图、Wiki、选项、索引与版本等易漂移项 | [CROSS-CHECKS.md](references/CROSS-CHECKS.md) |
| 分类发现、修复授权、并行审计与报告 | [REPORTING.md](references/REPORTING.md) |

## 2. 执行边界

- 全量审计覆盖全部分册;增量审计按变更面加载对应功能,但不得漏掉直接
  派生面。
- 除非用户明确要求修复,否则只读审计,不修改仓库内容;只有明确要求
  落盘时才写报告。
- 每项结论必须同时给出事实侧和派生侧证据;不适用项说明原因。
- 已落盘时最终回复摘要结论并给出路径;否则直接给出审计结论。
