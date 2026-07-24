---
name: clang-tidy
description: >-
  对指定文件或全仓库运行 clang-tidy(WarningsAsErrors='*')。用户要求
  "跑 clang-tidy"、"tidy 检查某文件"、排查 CI tidy 门禁失败时使用。
---

# clang-tidy 静态检查

仓库根 `.clang-tidy` 已配置检查集(google-*/readability-*/performance-*/
modernize-* 等,豁免项集中管理)且 `WarningsAsErrors: '*'` —— 任何告警即
失败。头文件过滤:`HeaderFilterRegex` 只覆盖
`include/vlink|src|modules|cli|proxy`。

先从仓库任意子目录解析根目录:

```bash
REPO_ROOT="$(git rev-parse --show-toplevel)"
PHYSICAL_CORES=
case "$(uname -s 2>/dev/null)" in
  Linux)
    PHYSICAL_CORES="$(
      LC_ALL=C lscpu -p=CORE,SOCKET 2>/dev/null |
        awk -F, '
          $1 !~ /^#/ && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ {
            cores[$2 SUBSEP $1] = 1
          }
          END {
            for (core in cores) {
              count++
            }
            if (count > 0) {
              print count
            }
          }'
    )" || PHYSICAL_CORES=
    ;;
  Darwin)
    PHYSICAL_CORES="$(sysctl -n hw.physicalcpu 2>/dev/null)" || PHYSICAL_CORES=
    ;;
esac
case "$PHYSICAL_CORES" in
  '' | *[!0-9]* | 0) PHYSICAL_CORES=1 ;;
esac
if [ "$PHYSICAL_CORES" -gt 1 ]; then
  BUILD_JOBS=$((PHYSICAL_CORES - 1))
else
  BUILD_JOBS=1
fi
```

AI 创建的 clang-tidy 构建目录必须位于 `build-ai/` 下并使用
`skill_clang_tidy` 前缀;不得复用、覆盖或清理维护者、IDE、CI 的其他
构建目录。默认目录正被其他任务使用时改用
`skill_clang_tidy_<task_name>` 或 `skill_clang_tidy_file_<task_name>`。
执行以下任一路径前设置
`PYTHONPYCACHEPREFIX="$BUILD_DIR/__pycache__"`。

## 1. 检查指定文件

需要 `compile_commands.json`。使用独立目录生成(仅 configure,不编译),
避免改写维护者日常构建配置:

```bash
BUILD_DIR="$REPO_ROOT/build-ai/skill_clang_tidy_file"
export PYTHONPYCACHEPREFIX="$BUILD_DIR/__pycache__"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
  -DENABLE_CXX_STD_20=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

然后对改动过的 `.cc` 文件运行:

```bash
clang-tidy -p "$BUILD_DIR" <file1.cc> <file2.cc>
```

纯头文件改动:找到包含它的某个 `.cc` 作为入口来跑。

## 2. 检查全仓库

以下只展示 clang-tidy 随编译执行的机制,不是 CI 参数的完整复现。
完整门禁参数以 `.github/scripts/ci-tidy.sh` 和 workflow 提供的
`VLINK_CI_CMAKE_ARGS` 为准;该 CI 脚本使用 CI 自己的构建目录,AI 在
当前工作区不得直接执行,必须把所需参数用于以下 skill 专属目录:

```bash
BUILD_DIR="$REPO_ROOT/build-ai/skill_clang_tidy"
export PYTHONPYCACHEPREFIX="$BUILD_DIR/__pycache__"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
  -DENABLE_CXX_STD_20=OFF \
  -DCMAKE_CXX_CLANG_TIDY=clang-tidy
cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS"
```

耗时长(CI 限时 120 分钟),仅在需要完整复现 CI 门禁时使用。
`BUILD_JOBS` 必须严格等于 `max(真实物理核心数 - 1, 1)`。禁止改用逻辑
CPU 数、裸 `--parallel`/`-j` 或固定高并行度;无法可靠获取时固定单核,
且同一时刻只运行一个本地构建,防止 clang-tidy 随编译并发卡死或耗尽
内存。

## 3. 结果处理

- 有告警时先报告;用户同时要求修复时优先修复源码。确属误报或有意
  写法时优先在目标行前使用
  `// NOLINTNEXTLINE(<检查名>)`;只有不适合放在前一行时才使用同行
  `// NOLINT(<检查名>)`。必须带具体检查名并保持最小范围,见
  `.agents/languages/CPP.md` §11。
  **禁止修改 `.clang-tidy` 配置**——检查集由维护者统一治理。
- `test/**/*.cc` 已整体 `// NOLINTBEGIN`/`// NOLINTEND` 包裹,tidy 告警
  不适用于 test 目录。
- 修复方式须符合 `.agents/languages/CPP.md`(如 modernize 类告警按仓库惯用
  写法修,而非教科书写法)。
