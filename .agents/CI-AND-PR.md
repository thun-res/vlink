# 评审、PR 与 CI

所有评审与提交先遵循根 `AGENTS.md` 的强制规则和
`AI-POLICY.md` 的 AI 贡献政策;本分册只补 GitHub、PR 和 CI 的
具体流程。

## 1. 代码评审规则

### 1.1 评审语言

- 所有 GitHub 代码评审输出使用简体中文(`zh-CN`),包括评审摘要、
  问题标题、解释说明、影响描述与修复建议。
- 代码标识符、文件路径、命令、API 名称、日志与引用的错误信息保持
  原文,但解释部分用简体中文。

### 1.2 评审重点

- 每条发现必须先对照实际代码核实再报告;不报告 `.clang-format`/
  `.clang-tidy` 已接受的风格问题。
- 优先级排序:`zerocopy/` 的线格式/ABI 破坏、热路径上的隐藏拷贝
  (含 `deep_copy`)、`base/` 并发原语的线程安全与内存序配对、
  `modules/*` 各后端行为的一致性。
- 新增/改动代码对照 `.agents/languages/CPP.md` 逐条检查,指出偏差时
  附上具体规则条目;"克制"违例(过度防御/过度封装/死代码)同样是
  正式发现。
- 重度全仓评审用 `/deep-review`;报告只写 `.agents/cache-report/`,不修改
  源码与文档。

## 2. PR 与 Commit 规范

- 分支模型、commit 前缀(`feat:`/`fix:`/`ci:`/`test:`/`docs:` 等
  小写)见 `doc/15-contributing.md` §15.11/§15.12;PR 描述以
  `.github/PULL_REQUEST_TEMPLATE.md` 的 Summary、Type of change、
  Related issues、How was this tested、Checklist 五节为准。提交前
  核对清单见 `doc/15-contributing.md` §15.9。
- commit subject/body 使用英文,PR 标题/正文使用简体中文;type/scope、
  代码标识、路径、API 名和错误信息保持原文.按模块/功能提交用
  `/commit`,`dev` → `master` PR 用 `/pr`,master 正式发版用 `/release`.
- `/pr` 与 `/release` 写入 GitHub 的标题、正文、notes、tag 注释等人工
  内容使用简体中文;产品名、版本、tag、代码标识和原始信息保持原文.
- 按 `doc/15-contributing.md` §15.11 从最新 `master` 创建规范命名的
  功能分支,PR 目标为 `master`;push 与创建 PR 前必须征得用户确认。
  用户显式调用 `/pr` 即视为授权该流程中的正常 push 和创建/更新 PR.
  `/pr` 是维护者将长期集成分支 `dev` 提交到 `master` 的专用流程,
  不改变普通功能分支的贡献模型。
- 发版前以 `version.txt` 为版本源,用 `tools/update_version.sh` 同步
  `include/vlink/version.h`、`conanfile.py`、可选 vcpkg 清单、README
  徽章与 `CHANGELOG.md`;发布经 GitHub Release 或 dispatch
  `release.yml`(正式发版见 `/release`,只调度工作流见 `/cicd`)。

## 3. CI

- 工作流:`ci-agent-skills.yml`(Agent skill 结构与元数据)、
  `ci-lint.yml`(format + cpplint + clang-tidy)、
  `ci-test.yml`(Linux+ASan / macOS / Windows)、`ci-coverage.yml`、
  `release.yml` 等;完整触发矩阵见 `/cicd`,实际执行脚本在
  `.github/scripts/`,本地复现以脚本为准。
- 触发矩阵、dispatch 命令、失败日志查看与重跑口径见 `/cicd` skill;
  本地复现:lint 失败 → `/format` `/check` `/clang-tidy`;Linux ASan
  或内存错误 → `/asan`;普通单元测试或 macOS/Windows 非 ASan 失败
  → `/test`,并按对应 job 日志在同平台复现。
- 代码性失败先修代码再 push;仅环境抖动(网络/runner 超时)才直接
  重跑。
- PR 代码审查只使用 GitHub 内置 Copilot code review,不由仓库
  GitHub Actions 调用模型。单个 PR 在 Reviewers 中手动请求 Copilot。
- 个人账户的 Automatic Copilot code review 仅自动审查本人创建的 PR;
  仓库或组织范围通过 branch ruleset 的
  `Automatically request Copilot code review` 配置。需要每次推送复审时,
  在该 ruleset 中启用 `Review new pushes`。
- Copilot review 只发表评论,不得配置为智能体必需状态检查;额度、认证或
  服务暂不可用不能变成 CI 门禁失败。Issue 与 Discussion 不设自动回复,
  仓库不得维护模型供应商 secret、模型 action 或 mention handler。
- `.github/copilot-instructions.md` 是 Copilot 的仓库级入口,继续要求
  读取根 `AGENTS.md` 和本分册。Automatic Copilot code review 属于
  GitHub 账户/仓库外部设置,无法仅由仓库提交证明已启用,核对时必须如实
  区分文件配置与实时设置状态。

## 4. Workflow 与 Dockerfile

- 修改 workflow 时先核对触发器、`permissions`、`needs`、`if`、
  matrix、inputs、secrets 和 reusable workflow 调用链;保持相邻文件
  的表达方式,不得擅自扩大权限、触发范围或发布条件。
- 修改 Dockerfile 时保持镜像标签、构建参数、目标平台、缓存和
  workflow 调用接口一致;不顺手升级基础镜像或依赖。
- Shell/PowerShell 片段同时遵守对应 languages 分册。只有维护者明确
  要求时才运行 `actionlint`、镜像构建或 workflow dispatch。
