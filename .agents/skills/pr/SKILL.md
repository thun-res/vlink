---
name: pr
description: >-
  整理 VLink 当前 dev 分支的全部改动,必要时按模块和功能调用 /commit
  创建本地提交,安全推送 dev,并向 master 创建或更新 GitHub PR.所有
  云端人工填写内容使用简体中文.用户要求"提交 PR"、"创建 PR"、
  "dev 合并到 master"或"填写 PR 标题和内容"时使用.
---

# 创建 dev 到 master 的 PR

本 skill 只处理 `thun-res/vlink` 的 `dev` → `master` PR,不切换分支、
不 force-push、不合并 PR.先读仓库根 `AGENTS.md`、
`.agents/skills/commit/SKILL.md`、`.agents/CI-AND-PR.md`、
`.github/PULL_REQUEST_TEMPLATE.md` 与 `doc/15-contributing.md`
§15.9/§15.11/§15.12.

这是维护者的长期集成分支提交入口,不替代贡献文档中普通功能分支到
`master` 的 PR 流程。

本 skill 写入 GitHub 的 PR 标题、正文及其他人工填写内容必须使用
简体中文。`type`/`scope`、代码标识、路径、分支、命令和原始错误信息
保持原文;模板固有标题不视为填写内容。

## 1. 检查本地状态

1. 用 `gh auth status` 和 `gh repo view --json nameWithOwner` 确认账号可用
   且仓库为 `thun-res/vlink`;不要打印可能含凭据的 remote URL.
2. 当前分支必须严格为 `dev`;detached HEAD 或其他分支直接停止,不得为了
   省事携带工作树改动切分支.
3. 存在 staged、unstaged 或 untracked 改动时,完整执行 `/commit` 的
   分组、message 和逐组提交流程.提交后重新检查工作树;除 ignored 文件外
   仍有改动时不得创建 PR.
4. 执行 `git fetch origin master dev`,确认:
   - `origin/master` 是当前 `HEAD` 的祖先;
   - `origin/dev` 也是当前 `HEAD` 的祖先.

任一条件不满足都停止并说明分叉关系;不得自动 merge、rebase、reset 或
force-push.

## 2. 分析 PR 内容

以 `origin/master...HEAD` 为唯一 PR diff,同时读取
`origin/master..HEAD` 的 commit 列表:

- 按模块和功能归纳重要变化,不逐文件复述.
- 明确用户可见行为、重要缺陷修复、性能变化、兼容影响和必要的同步项.
- 区分实际执行的验证与未执行项;不得根据 commit message 推断测试通过.
- 检查 diff 是否混入构建产物、缓存、敏感信息或与本 PR 无关的改动.
- diff 为空时停止;改动明显包含多个互不相关主题且无法形成可评审 PR 时
  停止并报告,不得用宽泛标题掩盖.

## 3. 生成中文云端内容

标题沿用最重要改动的 Conventional Commit 形式:
`type(scope): 中文动宾短语`.其中 `type`/`scope` 保留英文标识,其余使用
简体中文;不加句号、不使用"更新"、"调整"、"若干修复"等空泛表述,
总长不超过 72 字符.

正文严格沿用 `.github/PULL_REQUEST_TEMPLATE.md` 的五节,内容使用中文:

1. **Summary / 概述**:3–6 条,按模块/功能说明做了什么以及为什么.
2. **Type of change / 变更类型**:只勾选实际类型.
3. **Related issues / 关联 Issue**:只写已知 `Closes`/`Refs`;没有则写
   "无",不猜 issue 编号.
4. **How was this tested? / 如何测试**:逐条列出实际执行的命令、平台和
   结果;未构建或未测试必须如实写明.
5. **Checklist / 检查项**:只有证据确认通过的项目写 `[x]`,其余保持
   `[ ]` 并在下方简述原因,不得为了 PR 好看虚假勾选.

正文保持紧凑,重要内容不能遗漏,也不粘贴完整 commit/file 清单.

## 4. 推送并创建或更新 PR

1. 用 `git push origin dev:dev` 正常推送;禁止 `--force` 和
   `--force-with-lease`.
2. 查询 `head=dev`、`base=master` 的 open PR:
   - 不存在时,用 `gh pr create --repo thun-res/vlink --head dev
     --base master --title ... --body-file ...` 创建.
   - 恰有一个时,核对它确属当前分支后用 `gh pr edit` 更新标题和正文,
     不创建重复 PR.
   - 存在多个匹配项或 PR base/head 异常时停止.
3. 正文临时文件用 `mktemp` 创建,命令结束后删除;不得在仓库内留下草稿.
4. 创建/更新后用 `gh pr view` 核对 URL、title、base/head、状态和正文,
   并确认所有人工填写内容均为简体中文;不自动添加 reviewer、label、
   assignee,不自动 merge.

显式调用本 skill 已授权本次正常 push 和创建/更新 PR,无需重复确认;
任何 force、历史改写、换 base 或合并操作仍必须另行确认.

## 5. 完成后报告

返回 PR 编号、URL、中文标题、base/head、包含的 commit 摘要和已触发的
CI.同时列出未执行验证与剩余工作树改动;确认未 force-push、未合并 PR.
