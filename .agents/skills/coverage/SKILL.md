---
name: coverage
description: >-
  以 ENABLE_TEST_COVERAGE=ON 构建、运行测试并生成 lcov 代码覆盖率报告,
  复现 CI 的 coverage 流水线。用户要求"跑覆盖率"、"生成 coverage
  报告"、查某模块覆盖情况时使用。
---

# 代码覆盖率

本 skill 只在 Linux 的 GCC/Clang + lcov 环境执行。Windows helper
不会创建当前 `coverage` target,macOS 与 QNX 也不是仓库 CI 覆盖率
基线;不满足条件时停止并报告未验证,不得继续宣称已生成覆盖率。

`ENABLE_TEST_COVERAGE=ON` 时,根 `CMakeLists.txt` 对 `vlink` 库目标调用
`vlink_test_coverage(... TYPE lcov EXCLUDES ...)`(定义于
`cmake/functions/common.cmake`):注入 `-O0 -g -fprofile-arcs
-ftest-coverage`;构建并运行 `vlink-test` 产生被测库 profile 数据,
并生成 `coverage` 构建目标。排除目录由根 CMakeLists 的
`VLINK_TEST_COVERAGE_EXCLUDES` 统一维护(`c_api/`、`cli/`、`exprtk/`、
`proxy/`、`test/`、`thirdparty/` 等)。

当前覆盖率流程不同时启用 ASan;CI 的 coverage job 固定
`ENABLE_TEST_SANITIZE=OFF`(`.github/workflows/ci-coverage.yml`)。

## 1. 执行

以下步骤与 `.github/scripts/ci-coverage.sh` 的构建、测试和报告入口
一致;如需完全复现 CI 的依赖组合与编译选项,还需采用
`.github/workflows/ci-coverage.yml` 中的 `VLINK_CI_CMAKE_ARGS`。

```bash
REPO_ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR_REL=build-ai/skill_coverage
BUILD_DIR="$REPO_ROOT/$BUILD_DIR_REL"
export PYTHONPYCACHEPREFIX="$BUILD_DIR/__pycache__"
```

1. 配置:

   ```bash
   cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
     -DCMAKE_BUILD_TYPE=Debug \
     -DENABLE_CXX_STD_20=OFF \
     -DENABLE_TEST=ON \
     -DENABLE_TEST_WARN=ON \
     -DENABLE_TEST_COVERAGE=ON \
     -DENABLE_TEST_SANITIZE=OFF \
     -DENABLE_PROXY=ON \
     -DENABLE_EXAMPLES=OFF
   ```

   配置前确认 Fast DDS 或 Cyclone DDS 可被 CMake 找到。CMake 将
   `ENABLE_PROXY` 改回 OFF 或未生成 `vlink-proxy` target 均为配置
   失败,不得继续测试。

2. 构建并运行测试(必须先跑测试才有 profile 数据)。复用 CI 脚本以启动
   `vlink-proxy`,并沿用 CI 的测试参数:

   ```bash
   cmake --build "$BUILD_DIR" --target vlink-test vlink-proxy --parallel
   cd "$REPO_ROOT"
   BUILD_DIR="$BUILD_DIR_REL" bash .github/scripts/run-posix-ci-tests.sh
   ```

3. 生成报告:

   ```bash
   cmake --build "$BUILD_DIR" --target coverage
   ```

   HTML 报告位于 `$BUILD_DIR/coverage/index.html`,汇总文本可重定向保存
   (CI 存为 `coverage-summary.txt` 并取 `tail -80` 查看)。

## 2. 结果处理

- 根工程当前固定使用 `lcov`;底层 helper 也支持 `gcovr`,但本配置未
  选用。缺少 `lcov` 时 CMake 只告警、`coverage` 目标不可用。
- 默认 `BUILD_DIR_REL` 为 `build-ai/skill_coverage`;正被其他任务使用时
  先改为 `build-ai/skill_coverage_<task_name>`,再派生 `BUILD_DIR`,不得
  共用或清理其他构建目录。
- 覆盖率不足时先报告;用户同时要求提升覆盖率时优先补 `test/` 用例,
  而不是调整 EXCLUDES 把目录排除掉;修改 EXCLUDES 需与维护者确认。
- 对依赖罕见系统失败、平台专属分支、不可稳定触发的防御性兜底等确实
  难以覆盖的代码,可沿用仓库标记跳过统计:
  - 优先使用成对的 `LCOV_EXCL_START GCOVR_EXCL_START` 与
    `LCOV_EXCL_STOP GCOVR_EXCL_STOP`;
  - 明确只需排除单行时使用 `LCOV_EXCL_LINE GCOVR_EXCL_LINE`;
  - 仅需排除分支统计时使用
    `LCOV_EXCL_BR_LINE GCOVR_EXCL_BR_LINE`。
  标记必须缩到最小范围并保留;正常业务分支、可通过 mock 或输入构造
  覆盖的路径不得排除。添加前先说明为何无法稳定测试,不得为了提高
  数字而使用排除标记。
- 只在用户显式调用本 skill 时才执行构建与测试;日常改码流程仍遵循
  `AGENTS.md` 强制规则第 3 条(不主动构建)。
