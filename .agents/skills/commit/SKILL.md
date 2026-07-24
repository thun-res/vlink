---
name: commit
description: >-
  提交前强制执行 VLink 的 format 与 check skill，再分析当前工作树的全部
  staged、unstaged 与 untracked 改动，按模块、功能和依赖关系拆分为可独立
  评审的 Conventional Commits，生成简洁且覆盖重要行为的英文 commit
  message 并逐组提交。用户要求“提交当前改动”、“按功能拆 commit”、
  “自动写 commit msg”或“整理提交历史”时使用。
---

# 按模块和功能提交

只创建本地 commit，不 push、不创建 PR、不改写已有历史。先读仓库根
`AGENTS.md`、`.agents/CI-AND-PR.md` 与 `doc/15-contributing.md`
§15.9/§15.11。

## 1. 强制前置门禁

创建任何 commit、调整暂存区或制定最终分组前,必须完整加载并依次执行
`.agents/skills/format/SKILL.md` 与 `.agents/skills/check/SKILL.md`:

1. 先记录 `git status --short` 和完整 diff,根据用户请求确认本次授权
   覆盖的文件及其中既有改动是否允许格式化。存在归属不明、其他 Agent
   改动或未授权 WIP 时停止询问;`/commit` 不自动授权格式化这些文件。
2. 再按 `/format` 检查依赖与平台并执行 `tools/format.sh`;macOS 使用
   该 skill 规定的 Homebrew GNU sed 路径。缺少工具且需要安装依赖时先
   征得用户确认;不允许跳过格式化后继续提交。
3. 脏工作树中 `tools/format.sh` 会在格式化阶段全部成功后,因检测到
   tracked diff 打印 `Files have changed.` 并返回 1;这是预期的差异
   状态,不能误判为格式化失败。未完成全部阶段、工具报错或出现其他
   非零退出时停止,不得提交。
4. 格式化后对照执行前 diff 重新检查工作树。若触及授权范围外的干净或
   已脏文件,报告精确范围并停止,不得提交或回退维护者内容。
5. 再按 `/check` 检查 `cpplint` 依赖并执行 `tools/check.sh`。任何告警
   或非零退出都必须停止提交;不得追加过滤、使用 `NOLINT` 消音或绕过
   检查。只有用户另行要求修复时才修改对应源码。
6. 两项门禁都完成后运行 `git diff --check`,然后才允许进入改动盘点与
   分组。最终报告必须列出实际工具版本、命令和结果;不得把缺失、跳过
   或失败写成通过。

只要 `/format` 或 `/check` 未执行、无法执行或未通过,本 skill 就不得
创建任何 commit。

## 2. 盘点全部改动

1. 确认位于 VLink 仓库且当前不是 detached HEAD；存在 unresolved
   conflict 时停止。
2. 同时检查 `git status --short`、staged/unstaged diff、文件状态和
   untracked 文件内容。忽略 ignored 文件，不提交构建产物、缓存、密钥、
   token 或与任务无关的个人配置。
3. 结合 `.agents/REPO-REFERENCE.md` 判断模块职责，并从实际 diff 识别
   功能目的；不得只按顶层目录或扩展名机械分组。
4. 已有 staged 内容也必须纳入分析。需要重组 index 时只调整暂存区，
   不改写或丢弃工作树内容，并在每一步后核对 staged/unstaged 差异。

## 3. 制定提交分组

每个 commit 只表达一个可独立说明、独立评审的逻辑变化：

- 不同模块或互不依赖的功能分别提交。
- 同一功能的实现、测试、公开注释、Python/C 导出、示例和文档同步放在
  同一 commit，不为追求目录整齐拆散必要同步项。
- 公共基础设施先于依赖它的功能提交；生成文件与其源文件成对提交。
- 行为修复、纯重构、格式整理、CI/构建、Agent 指引分别提交，除非其中
  一项是另一项不可分割的组成部分。
- 同一文件含多个独立改动时可按 hunk 分拆暂存；改动相互交叠、无法安全
  分拆时保持同组，不修改源码制造拆分。
- 不为减少 commit 数量混合无关内容，也不把一个完整功能切成逐文件碎片。

开始提交前先形成有序分组清单，列出每组目的、type/scope 和文件/hunk。
用户已明确要求自动提交时直接执行；只有改动意图无法从代码判断、发现
疑似敏感信息或无法无损分组时才停止询问。

## 4. 编写 Commit Message

遵循 `type(scope): subject`。`type`、`scope`、subject 与必要的 body
全部使用英文;代码标识、路径、API 名和错误信息保持原文,与
`doc/15-contributing.md` §15.11 一致。

- type 从 `feat`、`fix`、`refactor`、`perf`、`docs`、`test`、`build`、
  `ci`、`chore`、`style`、`revert` 中选择。
- scope 使用仓库现有模块名，如 `base`、`extension`、`proxy`、
  `module-dds`、`cli-bag`、`cmake`、`agents`；不可拆分的跨 scope
  改动可省略 scope 或使用 `*`,但可拆分时不得用 `*` 掩盖混合提交。
- subject 使用简洁明确的英文祈使句，不加句号且不超过 72 字符；准确
  写出行为，不使用 `update`、`adjust`、`misc fixes`、`cleanup` 等
  空泛表述。
- subject 已足够时不写 body。重要行为无法在 subject 中讲清时添加一个
  简短英文 body，优先说明原因、用户可见影响、性能/兼容变化或关键约束，
  不逐文件复述 diff；每行不超过 72 字符。
- 仅在确有破坏性变更或 issue 时写 `BREAKING CHANGE:`、`Closes:`、
  `Refs:` footer，不推测编号。

示例：

```text
ci: standardize GitHub Actions script style
fix(packup): prevent empty config from leaking local scope
docs(agents): add module-aware commit skill
```

## 5. 逐组暂存和提交

对每一组依次执行：

1. 暂存前重新读取 `git status`、目标文件和相关 diff;出现不在分组清单
   内的新变化时停止并重新分析,不得覆盖、回退或顺手纳入其他 Agent 的
   工作。只暂存该组的明确路径或 hunk，禁止无检查地使用 `git add -A`、
   `git add .` 或通配整个仓库。
2. 查看完整 `git diff --cached` 和 `--stat`，确认没有混入下一组、
   ignored/生成垃圾或敏感信息；运行 `git diff --cached --check`。
3. `git commit` 前再次确认工作树和 index 未发生并发变化。staged diff
   非空且单独成立时再用最终 message 提交。
4. commit hook 失败时不得使用 `--no-verify`。检查 hook 产生的变化，
   修正分组或报告阻塞后再提交。
5. 用 `git show --stat --oneline HEAD` 核对结果，再处理下一组。不得
   amend、rebase、squash 或 reset 已创建的 commit，除非用户明确要求。

本 skill 只额外授权上述强制 `/format` 与 `/check`;不要自行执行
`cmake`、`ninja`、`clang-tidy` 或测试二进制,也不要在 commit message
中声称未执行的验证已通过。

## 6. 完成后报告

输出按顺序排列的 commit hash、subject 和每组核心范围；同时报告剩余
未提交改动及原因,并单列 `/format`、`/check` 的实际结果。最后确认未
push、未创建 PR、未改写历史。
