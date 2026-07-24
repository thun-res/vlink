---
name: check
description: >-
  运行 tools/check.sh 对整个仓库执行 cpplint 静态检查(120 列,项目定制
  过滤集)。用户要求"跑 check"、"cpplint 检查"、排查 CI lint 门禁失败
  时使用。
---

# cpplint 静态检查

对仓库执行 cpplint,入口与参数同 CI 的 lint 门禁
(`.github/scripts/ci-lint.sh`)中 "Run cpplint" 步骤。CI 镜像当前使用
`2.0.2`。

## 1. 执行

先检查 `cpplint`。缺失时停止并询问是否允许 `pip install --user`;未经
确认不得直接运行会自动安装依赖的 `tools/check.sh`。

```bash
REPO_ROOT="$(git rev-parse --show-toplevel)"
export PYTHONPYCACHEPREFIX="$REPO_ROOT/build-ai/skill_check/__pycache__"
command -v cpplint
cpplint --version
bash "$REPO_ROOT/tools/check.sh" "$REPO_ROOT"
```

脚本行为:

- 扫描所有 `.h/.hpp/.cc/.cpp`(自动排除 `thirdparty/`、`build/`、
  `build-*/`、`builtin/`、`prebuilt/`、`android-bp/`)。
- `cpplint --counting=detailed --linelength=120`,过滤集已禁用:
  `build/c++17`、`build/include*`、`runtime/references`、
  `readability/check`、`readability/braces`、`readability/nolint`、
  `whitespace/indent_namespace`。
- 末尾统计总代码行数。

## 2. 结果处理

- 有告警时逐条报告;用户同时要求修复时再修改源码。**禁止**通过在脚本
  过滤集里追加条目来消音(过滤集由维护者统一治理)。误报的处置遵循
  `.agents/languages/CPP.md` 第 11 节的既定习惯。
- `tools/check.sh` 在依赖缺失时会安装 `cpplint`,因此必须完成上述确认。
  本地版本与 CI 的 `2.0.2` 不一致时先报告差异,由维护者决定是否换用
  CI 版本或镜像;不得自行用版本差异产生的告警判定提交门禁。
- 执行依赖检查、安装和 cpplint 期间保持上述
  `PYTHONPYCACHEPREFIX`,不得在源码树生成 `__pycache__`。
- 执行前记录 `cpplint --version`。
- 本 skill 只跑 cpplint;clang-tidy 检查用 `/clang-tidy`,格式化用
  `/format`。
