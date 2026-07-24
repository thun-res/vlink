---
name: release
description: >-
  在 VLink master 分支校验工作树、远端同步、SemVer 版本镜像、CHANGELOG、
  既有 tag/release 与 CI 状态,为 version.txt 当前版本创建 annotated tag,
  推送 tag 并发布云端人工填写内容均为简体中文的 GitHub Release,随后
  跟踪 release workflow 和产物.用户要求"打 tag"、"发布 release"或
  "从 master 发版"时使用.
---

# 在 master 发布 Release

本 skill 只发布已经通过 PR 合入 `thun-res/vlink` `master` 的正式版本.
不修改版本文件、不提交代码、不 push master、不覆盖 tag、不删除或重建
既有 Release.先读仓库根 `AGENTS.md`、`.agents/CI-AND-PR.md`、
`doc/15-contributing.md` §15.17/§15.19.3 和
`.github/workflows/release.yml`.

本 skill 写入远端的 tag 注释、Release 标题、Release Notes 及其他人工
填写内容必须使用简体中文。产品名、版本、tag、代码标识、路径、命令和
原始错误信息保持原文。

## 1. 发布前门禁

逐项满足后才能继续:

1. 用 `gh auth status` 与 `gh repo view --json nameWithOwner` 确认权限和
   仓库;不要打印可能含凭据的 remote URL.
2. 当前分支严格为 `master`,工作树和 index 完全干净,不存在 untracked
   文件或冲突.
3. 执行 `git fetch origin master --tags`,确认本地 `HEAD` 与
   `origin/master` 完全相同.不得在 master 上 commit、merge、rebase、
   reset 或 push.
4. 从 `version.txt` 读取版本,必须严格匹配 `X.Y.Z`,每段为 0–255;tag
   固定为 `vX.Y.Z`.
5. 逐项核对 `include/vlink/version.h`、`conanfile.py`、可选
   `tools/vcpkg/vcpkg.json`、README/README.en.md 版本徽章与
   `CHANGELOG.md` 顶部正式版本段均等于该版本.发现不一致时停止,要求
   回 release 分支运行 `tools/update_version.sh` 并经 PR 合入.
6. 新版本必须高于除当前 tag 外的最高正式 SemVer tag.分别识别本地 tag、
   远端 tag 和 GitHub Release 状态:
   - tag 不存在:按首次发布继续.
   - tag 已存在:必须是 annotated tag 且最终指向当前 HEAD;本地与远端
     同名 tag 同时存在时还必须指向相同对象.全部一致才允许复用,任一
     不一致立即停止.
   - Release 不存在:后续创建 draft.
   - 同 tag Release 为 draft:核对 target/title/notes 后从发布 draft
     继续.
   - 同 tag Release 已 published:跳过创建和发布,直接核对 workflow 与
     资产.

   查询失败必须区分"确实不存在"与认证/网络/API 错误;后者直接停止.
7. 分别用
   `gh run list --workflow ci-lint.yml --commit <HEAD_SHA>` 和
   `gh run list --workflow ci-test.yml --commit <HEAD_SHA>` 核对该
   master commit 的两个工作流均已完成且成功.缺失、排队、运行中、
   cancelled、skipped 或失败时停止;若因 `paths-ignore` 未触发,先由
   用户显式调用 `/cicd` 为当前 master dispatch 两个工作流,成功后重新
   执行门禁,不得绕过 CI 发版.

显式调用本 skill 授权在全部门禁通过后创建并推送当前版本 tag、发布
Release;版本不明确或任何门禁失败时不得继续.

## 2. 准备中文 Release Notes

从 `CHANGELOG.md` 当前 `## vX.Y.Z (日期)` 标题起,提取到下一
`## v...` 标题之前:

- Release 标题使用 `VLink vX.Y.Z 正式发布`.
- 正文使用中文,保留"新增功能/改进/修复"等现有结构和重要兼容提示.
- 不包含下一个版本或旧版本内容,不凭空补充未记录特性.
- 删除空章节和重复标题,但不把重要行为压缩成无信息量的一句话.
- 用 `mktemp` 保存 notes,发布流程结束后删除;不在仓库写临时文件.

CHANGELOG 当前版本段为空、含占位内容或与实际 diff 明显不符时停止.

## 3. 创建或复用 Tag

1. 创建或推送 tag 前再次执行 `git fetch origin master --tags`,确认
   当前 `HEAD` 仍等于先前通过 CI 的 `HEAD_SHA` 且
   `origin/master` 仍等于该 SHA;任一变化都停止,重新从发布前门禁开始。
2. 最后一次核对 `HEAD_SHA`、版本、tag 和 notes.
3. 本地 tag 不存在且远端同名 tag 也不存在时创建 annotated tag:

   ```bash
   git tag -a "vX.Y.Z" -m "发布 VLink vX.Y.Z"
   ```

4. 本地不存在但远端存在一致 tag 时,只 fetch 该 tag 后核对,不重新创建.
5. 确认 tag 指向已验证的 `HEAD_SHA` 且类型为 annotated tag.
6. 远端 tag 不存在时只推送该 tag:

   ```bash
   git push origin "refs/tags/vX.Y.Z"
   ```

   远端已存在一致 tag 时跳过 push.

禁止 `git push --tags`、force、移动或复用不一致 tag.推送失败时保留
本地 tag 并报告状态,不得自动删除后重试.

## 4. 创建草稿并正式发布

远端 tag 已确认后,按 Release 当前状态继续:

1. Release 不存在时先创建 draft,降低 notes/title 错误直接对外发布的
   风险:

   ```bash
   gh release create "vX.Y.Z" --repo thun-res/vlink --verify-tag --draft \
     --target "<HEAD_SHA>" --title "VLink vX.Y.Z 正式发布" \
     --notes-file "<notes-file>"
   ```

2. 已存在 draft 时不重复创建.用 `gh release view` 核对 tag、target、
   中文标题、中文正文和 draft 状态;若内容不一致则停止,不得静默覆盖.
3. draft 核对无误后用
   `gh release edit "vX.Y.Z" --draft=false --latest` 正式发布.
   `release: published` 将自动触发 `.github/workflows/release.yml`.
4. Release 已 published 时核对其 tag/target/title/notes 及人工填写内容
   的语言;一致则直接进入 workflow 与资产检查,不再次 edit.

如果 draft 创建或正式发布失败,不得删除远端 tag、Release 或重建同名
对象;报告当前阶段,下次从已存在对象继续幂等恢复.

## 5. 跟踪 Workflow 与产物

1. 定位本次 `release` 事件触发的 `release.yml` run,避免误看其他分支或
   手动 dispatch.
2. 等待到终态并持续报告进度.失败时查看 failed log,不得把构建失败写成
   发布成功,也不得因代码性失败自动重跑.
3. workflow 成功后核对 Release 仍为 published;再查询未指定 tag 的
   latest Release,确认其 tag 为本次版本.检查发布资产和 `SHA256SUMS`
   已上传,资产缺失即报告为未完成.

发布页面建立与发布产物完成是两个阶段;只有 tag、GitHub Release、
workflow 和资产全部核对通过后才能宣称发版完成.

## 6. 完成后报告

返回版本、tag、tag 指向 SHA、Release URL、workflow URL/结论和资产摘要.
明确确认 master 未被 push、未 force、未覆盖/删除既有 tag 或 Release;
如停在中间阶段,给出已完成状态和安全的继续入口.
