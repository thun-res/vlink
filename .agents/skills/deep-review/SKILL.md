---
name: deep-review
description: >-
  对 VLink 全仓或指定范围执行只读重度代码评审,按功能规划覆盖范围和
  subagent,逐文件检查正确性、并发、性能与规范,经对抗复核后给出中文
  证据结论。用户要求"全面 review"、"深度检查全仓"、"大规模审计代码"
  或"对抗评审"时使用;明确要求落盘时生成报告。
---

# 全局重度代码评审

本 skill 只评审,不修改源码。用户指定范围时审计对应子集;未指定时覆盖
全部第一方受管文件;只有明确要求落盘时才写 `.agents/cache-report/`。

## 1. 功能路由

只加载当前阶段需要的分册:

| 功能 | 必读分册 |
|---|---|
| 建立文件全集、模块边界和排除项 | [SCOPE-PLANNING.md](references/SCOPE-PLANNING.md) |
| 逐文件检查并判定严重级 | [REVIEW-DIMENSIONS.md](references/REVIEW-DIMENSIONS.md) |
| 并行派发与对抗复核 | [AGENT-ORCHESTRATION.md](references/AGENT-ORCHESTRATION.md) |
| 汇总证据、覆盖率和报告 | [REPORTING.md](references/REPORTING.md) |

## 2. 执行顺序

1. 固化审计范围和文件清单,再按职责与依赖切分任务。
2. 每个任务逐文件审查,返回覆盖清单和带行号发现。
3. 汇总后回读源码核实所有 P0/P1,对重要结论做反驳式复核。
4. 跨模块去重,列出未覆盖项;明确要求落盘时写入报告。

不得以抽样代替承诺的全量覆盖,不得把“未发现”写成“已证明没有问题”。
