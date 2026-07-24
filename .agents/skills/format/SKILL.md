---
name: format
description: >-
  运行 tools/format.sh 对整个仓库做 clang-format、cmake-format 与行尾符
  统一。用户要求"格式化代码"、"跑 format"、提交前统一格式时使用。
---

# 代码格式化

对仓库执行统一格式化,入口与参数同 CI 的 lint 门禁
(`.github/scripts/ci-lint.sh`)中 "Check formatting" 步骤。本地工具版本
不一定与 CI 镜像固定版本相同。

## 1. 执行

先检查 `clang-format` 与 `cmake-format`。任一缺失时停止并询问是否允许
`pip install --user`;未经确认不得直接运行会自动安装依赖的
`tools/format.sh`。

```bash
REPO_ROOT="$(git rev-parse --show-toplevel)"
export PYTHONPYCACHEPREFIX="$REPO_ROOT/build-ai/skill_format/__pycache__"
command -v clang-format || exit 1
command -v cmake-format || exit 1
FORMAT_PATH="$PATH"
if [ "$(uname -s)" = Darwin ]; then
    command -v brew || exit 1
    GNU_SED_BIN="$(brew --prefix gnu-sed)/libexec/gnubin" || exit 1
    [ -x "$GNU_SED_BIN/sed" ] || exit 1
    FORMAT_PATH="$GNU_SED_BIN:$PATH"
fi
PATH="$FORMAT_PATH" bash "$REPO_ROOT/tools/format.sh" "$REPO_ROOT"
```

三个阶段共用 `tools/format.sh` 中同一组目录排除条件:
`thirdparty/`、`build/`、`build-*/`、`builtin/`、`prebuilt/` 与
`android-bp/`。这是 `/format` 的唯一执行范围基准,语言分册不另行复制
维护路径集合。

脚本行为:

1. **clang-format**:按仓库根 `.clang-format` 原地格式化所有
   `.h/.hpp/.cc/.cpp`。
2. **cmake-format**:原地格式化所有 `CMakeLists.txt`、`*.cmake`、
   `*.cmake.in`。
3. **EOL-FORMAT**:按 `.gitattributes` 的 `eol` 属性统一脚本文件行尾符
   (crlf/lf)。
4. 最后执行 `git diff --quiet`:有未暂存的已跟踪差异时退出码为 1 并
   打印 diff;仅暂存或未跟踪文件不在该命令的判断范围。干净工作树中可
   据此判断格式化产生了改动;脏工作树中不能把全部差异归因于本次格式化。

## 2. 注意事项

- `tools/format.sh` 在依赖缺失时会自动 `pip install --user`,因此必须
  完成上述确认。
- 执行依赖检查、安装和格式化期间保持上述 `PYTHONPYCACHEPREFIX`,
  不得在源码树生成 `__pycache__`。
- 当前脚本使用 GNU `sed -i`。Linux 直接执行;macOS 必须先确认 Homebrew
  `gnu-sed` 可用并按上例注入 PATH。缺失时先询问是否允许安装,不得先跑
  前两个原地格式化阶段。
- 执行前记录 `clang-format --version` 与 `cmake-format --version`;
  需要精确复现 CI 时使用仓库 CI 镜像中的固定版本。
- 工作区有未提交改动时也可运行,但 diff 会混入格式化改动;建议先确认
  `git status` 再执行。共享工作区出现并发变化时停止,不得让全仓格式化
  覆盖或混入其他 Agent 正在修改的内容。
- 只做格式化,不做静态检查;静态检查用 `/check`(cpplint)或
  `/clang-tidy`。
