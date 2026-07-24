---
name: cicd
description: >-
  用 gh CLI 触发/查看 GitHub CI/CD:手动 dispatch 工作流
  (release/coverage/docker)、查看运行状态与失败日志、重跑失败 job.
  用户要求"触发 CI"、"看流水线"、"重跑失败的 job"时使用;创建
  dev 到 master 的 PR 使用 /pr,正式发版使用 /release.
---

# GitHub CI/CD 操作

仓库:`thun-res/vlink`。普通功能分支 PR 目标为 `master`;维护者通过
`/pr` 处理长期集成分支 `dev` → `master`。所有操作用 `gh` CLI 完成。

## 1. 触发矩阵

| 工作流 | PR | push¹ | release | schedule | dispatch | workflow_call |
| ------ | :-: | :---: | :-----: | :------: | :------: | :-----------: |
| `ci-agent-skills.yml` | ✔ | ✔ | — | — | ✔ | — |
| `ci-lint.yml` | ✔ | ✔ | — | — | ✔ | — |
| `ci-test.yml` | ✔ | ✔ | — | — | ✔ | — |
| `ai-code-review.yml` | ✔ | — | — | — | — | — |
| `community-ai-reply.yml`² | — | — | — | — | — | — |
| `ci-coverage.yml` | — | — | — | 每周 | ✔ | ✔ |
| `docker-images.yml` | — | — | — | — | ✔ | ✔ |
| `release-portable-linux.yml` | — | — | — | — | — | ✔ |
| `release-portable-macos.yml` | — | — | — | — | — | ✔ |
| `release-portable-windows.yml` | — | — | — | — | — | ✔ |
| `release-linux-packages.yml` | — | — | — | — | — | ✔ |
| `release.yml` | — | — | published | — | ✔ | — |

¹ `ci-agent-skills.yml`、`ci-lint.yml` 与 `ci-test.yml` 的 push 分支为
`master`/`main`/`develop`/`dev`;PR 使用同一分支集合。`ci-lint.yml` 与
`ci-test.yml` 忽略纯 `**.md`、`doc/**`、`.github/wiki/**` 改动;
`ci-agent-skills.yml` 只处理 `AGENTS.md`、`.agents/**` 及对应安装、
校验入口和根 `AI-POLICY.md`。`ai-code-review.yml` 只处理 opened、
synchronize、ready_for_review、reopened。

² `community-ai-reply.yml` 由 `issues`、`issue_comment`、`discussion`
和 `discussion_comment` 的创建/编辑事件触发。Issue/Discussion 的
`@codex`/`@claude` 与 PR 普通评论的 `@codex` 会在触发者的
`author_association` 为 `OWNER`、`MEMBER`、`COLLABORATOR` 或
`CONTRIBUTOR` 时回复;PR `@claude` 不走该链路。该 workflow 必须位于
默认分支;PR 裸 `@codex` 只读取线程,官方 `@codex review` 与普通回复互斥;
`@codex` 需要 `OPENAI_API_KEY`,`@claude` 需要
`CLAUDE_CODE_OAUTH_TOKEN`,不支持手动 dispatch。

`release.yml` 复用 docker、coverage 与四个 `release-*` 子工作流;
四个子工作流只能由 `workflow_call` 调用,不能直接 dispatch。
coverage 的定时任务为每周日 18:30 UTC,并带最近提交门禁;手动 dispatch
或 release 传入 `force_run` 时不受该门禁。

## 2. PR 与 Release 分工

- 当前 `dev` 改动提交到 `master` 使用 `/pr`;它负责调用 `/commit`、
  正常 push、中文标题/正文和模板核对.
- `master` 正式版本的 tag 与 GitHub Release 使用 `/release`;它负责版本、
  CI、tag、发布工作流和资产核对.
- 本 skill 只负责工作流 dispatch、状态、日志和重跑,不重复实现 PR 或
  Release 发布流程.

## 3. 手动 dispatch 工作流

```bash
vlink_ref="<feature-branch>"
gh workflow run ci-lint.yml --repo thun-res/vlink --ref "${vlink_ref}"
gh workflow run ci-test.yml --repo thun-res/vlink --ref "${vlink_ref}"
gh workflow run ci-agent-skills.yml --repo thun-res/vlink --ref "${vlink_ref}"
gh workflow run ci-coverage.yml --repo thun-res/vlink --ref "${vlink_ref}"
gh workflow run docker-images.yml --repo thun-res/vlink --ref master
gh workflow run release.yml --repo thun-res/vlink --ref master \
  -f publish_pages=true -f build_portable=true -f build_linux_packages=true
```

前四条的 `vlink_ref` 应设为待验证的功能分支。发布类工作流固定从
`master` 触发。

`docker-images.yml` 会向 GHCR 推送镜像并移动
`vlink-ubuntu20:latest` / `vlink-ubuntu22:latest`;
`release.yml` 会构建发布产物并可部署 Pages。两者都属于发布写操作,
**执行前必须向用户复述 workflow、ref、参数和外部影响并确认**。

## 4. 查看状态与日志

```bash
gh run list --repo thun-res/vlink --branch dev --limit 10        # 最近运行
gh pr checks <PR号> --repo thun-res/vlink                        # PR 的检查项
gh run view <run-id> --repo thun-res/vlink                       # 单次运行概览
gh run view <run-id> --repo thun-res/vlink --log-failed          # 只看失败日志
gh run watch <run-id> --repo thun-res/vlink                      # 阻塞式等待完成
```

失败排查顺序:`--log-failed` 定位首个报错 → 本地用对应 skill 复现
(lint 失败 → `/format` `/check` `/clang-tidy`;Linux ASan/内存错误
→ `/asan`;普通单元测试或 macOS/Windows 非 ASan 失败 → `/test`)。

## 5. 重跑

```bash
gh run rerun <run-id> --repo thun-res/vlink --failed   # 只重跑失败 job
gh run rerun <run-id> --repo thun-res/vlink            # 全量重跑
```

仅在确认失败为环境抖动(网络、runner 超时)时才直接重跑;代码性失败先修
代码再 push,不要靠重跑碰运气。
