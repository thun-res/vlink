---
name: sync-agent
description: >-
  基于 VLink 当前代码、doc、目录、工具和 GitHub 工作流事实,审计并同步
  AGENTS.md、AI-POLICY.md、Copilot 指令、.agents 路由/索引/语言分册/
  CI 说明、skill frontmatter、agents/openai.yaml 与 README 清单.用户
  要求"同步 Agent 文件"、"根据代码更新 .agents"、"检查 AGENTS.md 和
  skills"、"Agent 规则跟仓库对齐"时使用;产品与公开 API 的完整同步仍
  使用 /sync-review.
---

# 同步 Agent 体系

目标是让 Agent 规则和路由准确指向当前仓库,不在 Agent 文件复制产品知识。
代码定义实现事实,`doc/00`–`15` 定义产品与架构语义;Agent 文件只维护
强制规则、工作流程、目录职责和渐进式披露入口。

## 1. 授权与边界

- 用户只要求检查、分析或报告时保持只读,不得修改或生成报告文件。
- 用户明确要求"同步"、"更新"或"修复"时,只修改 `AGENTS.md`、
  `AI-POLICY.md`、`.github/copilot-instructions.md`、`.agents/**` 及
  直接负责 Agent 校验的 CI/工具文件。
- 不修改产品代码、公开 API、`doc/`、README、测试或构建配置来迁就
  Agent 描述;发现这些事实侧本身冲突时报告并交给对应任务。
- 公开 API、功能入口或文档结构变化必须同批使用 `/sync-review`;本
  skill 只处理其 Agent 派生面,不得替代产品同步审计。
- 未经维护者明确命令不得升级版本,也不得提交、push、创建 PR、安装
  skill、构建、运行测试或执行无关项目脚本。
- 保留维护者和其他 Agent 的工作树改动;写前重读目标文件、`git status`
  和最新 diff,不得覆盖、回退、stash 或清理。

## 2. 建立事实基线

先读根 `AGENTS.md`、`AI-POLICY.md`、`.agents/README.md`、
`REPO-REFERENCE.md`、`FEATURE-INDEX.md` 与
`.github/copilot-instructions.md`,再从当前仓库反向验证:

- 顶层目录、`include/vlink/`、`src/`、`modules/`、`cli/`、绑定、示例
  和测试的实际职责。
- `doc/00`–`15` 的现有文件、小节号、标题和功能归属。
- `CMakeLists.txt`、`cmake/`、`tools/`、`.github/workflows/` 与
  `.github/scripts/` 的真实入口、选项和触发关系。
- `.agents/skills/*/SKILL.md`、`agents/openai.yaml`、引用文件及
  `.agents/README.md` 的完整 skill 集合。
- 当前分支、HEAD、暂存区、工作树和未跟踪文件;增量任务还要检查指定
  base 到 HEAD 的变化。

代码签名、目录和控制流以代码为事实;产品定位、能力边界和架构意图以
`doc/` 为事实。两者冲突时不得擅自选择一侧,应报告待确认项。

## 3. 同步各派生面

### 3.1 根规则

`AGENTS.md` 只保留跨任务、稳定且不可协商的强制规则与路由指向。新增
规则必须有真实重复风险或维护者明确要求,不得塞入模块清单、API 说明、
命令示例或临时实现细节。检查编号、引用和分册职责是否连续一致。

### 3.2 路由与索引

- `.agents/README.md`:任务到分册的路由、分册清单、skill 清单和安装
  说明与实际文件一致。
- `REPO-REFERENCE.md`:目录职责、文档章节、工程设施和相关仓库仍存在且
  语义准确,不复制专题文档内容。
- `FEATURE-INDEX.md`:每个功能的代码入口和 `doc/` 小节真实存在;新增、
  删除、改名或拆分功能后同步入口,但不在索引解释完整机制。

### 3.3 规则分册

语言、测试、文档、CI/PR 和 AI 政策只记录仓库特有约束。规则变更需核对
上位边界与引用方,避免在 `AGENTS.md`、分册和 skill 中重复维护或互相
冲突。无代码证据或维护者决策时不得把个人偏好写成强制规则。
`.github/copilot-instructions.md` 只保留指向根规则、当前任务分册和
GitHub 原生智能体边界的最小入口,不得复制产品知识或整套 Agent 规则。

### 3.4 Skills

对每个 `.agents/skills/<name>/` 检查:

- 目录名、frontmatter `name` 和触发型 `description` 一致。
- `SKILL.md` 的授权边界、工具命令、引用路径和实际工作流有效。
- `agents/openai.yaml` 的 `display_name`、`short_description` 与
  `default_prompt` 匹配 skill,且 prompt 明确包含 `$<name>`。
- `.agents/README.md` 有且仅有一条对应入口;新增 skill 不附带无用的
  README、模板或资源目录。
- 公开写入、发布、Issue、Discussion、PR、Release 和 CI dispatch 均有
  明确授权与回读步骤,不得伪装身份、扩大权限或隐藏未执行验证。

### 3.5 Agent CI

`.github/workflows/ci-agent-skills.yml` 与校验脚本应覆盖 `AGENTS.md`、
`AI-POLICY.md`、`.github/copilot-instructions.md`、`.agents/**`、skill
安装入口、Issue/Discussion 回复授权契约及自身变化,权限保持
`contents: read`。
只校验结构、元数据、路由、关键授权/回读契约和占位符,不得自动发帖、
创建 Issue/Discussion、修改仓库设置或安装网络依赖。

## 4. 最小修改

先列出事实侧与过时派生面,再按依赖顺序修改:根规则 → 分册 → 路由/索引
→ skill → CI 说明。只修当前证据支持的差异,不借同步任务重写整套规则、
重排无关内容或统一个人偏好。路径、标题和小节变化必须同时修复直接引用。

## 5. 验证

在用户授权范围内执行只读或 Agent 专用校验:

```bash
bash -n tools/install_skills.sh
bash -n .github/scripts/test-validate-agent-skills.sh
bash -n .github/scripts/validate-agent-skills.sh
PYTHONPYCACHEPREFIX="$PWD/build-ai/sync-agent/__pycache__" \
    bash .github/scripts/test-validate-agent-skills.sh
PYTHONPYCACHEPREFIX="$PWD/build-ai/sync-agent/__pycache__" \
    bash .github/scripts/validate-agent-skills.sh
git diff --check
```

新增或修改 skill 时,再用 skill-creator 的 `quick_validate.py` 校验目标
目录,并为该 Python 调用设置同一任务专属 `PYTHONPYCACHEPREFIX`;修改
workflow 时优先运行已安装的 `actionlint`,缺失时不得擅自安装,改用现有
YAML 解析器检查语法并如实报告未执行项。

最终逐文件回读 diff,说明同步依据、修改范围、无需变化的派生面、未执行
验证和仍待维护者确认的冲突。确认没有修改版本、产品行为或外部状态。
