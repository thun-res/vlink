---
name: asan
description: >-
  以 AddressSanitizer(ENABLE_TEST_SANITIZE=ON)构建并运行 vlink-test
  单元测试,复现 CI 的 ASan 门禁。用户要求"跑 asan"、"内存检测测试"、
  排查 ci-test 的 sanitize 失败时使用。
---

# ASan 单元测试

`ENABLE_TEST_SANITIZE=ON` 时,根 `CMakeLists.txt` 对 `vlink` 库目标调用
`vlink_test_sanitize()`(`cmake/functions/common.cmake:45`),追加
`-fsanitize=address` 编译与链接选项;随后构建并运行 `vlink-test` 触发
被测库代码。支持 Windows/QNX 之外的 GCC/Clang 目标;不满足条件时会
告警跳过。CI 中只有 Linux job 开启此项
(`.github/workflows/ci-test.yml`)。

## 1. 执行

先从仓库任意子目录解析根目录:

```bash
REPO_ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR_REL=build-ai/skill_asan
BUILD_DIR="$REPO_ROOT/$BUILD_DIR_REL"
export PYTHONPYCACHEPREFIX="$BUILD_DIR/__pycache__"
```

1. 配置(AI 专用构建目录):

   ```bash
   cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
     -DCMAKE_BUILD_TYPE=Release \
     -DENABLE_CXX_STD_20=OFF \
     -DENABLE_TEST=ON \
     -DENABLE_TEST_WARN=ON \
     -DENABLE_TEST_SANITIZE=ON \
     -DENABLE_TEST_COVERAGE=OFF \
     -DENABLE_PROXY=ON \
     -DENABLE_EXAMPLES=OFF
   ```

   配置前确认 Fast DDS 或 Cyclone DDS 可被 CMake 找到。CMake 将
   `ENABLE_PROXY` 改回 OFF 或未生成 `vlink-proxy` target 均为配置
   失败,不得继续测试。

2. 构建:

   ```bash
   cmake --build "$BUILD_DIR" --target vlink-test vlink-proxy --parallel
   ```

3. 运行测试。复用 CI 脚本,确保先启动测试依赖的 `vlink-proxy`,并沿用
   CI 的超时、并行度和排除规则:

   ```bash
   export ASAN_OPTIONS=detect_odr_violation=0
   cd "$REPO_ROOT"
   BUILD_DIR="$BUILD_DIR_REL" bash .github/scripts/run-posix-ci-tests.sh
   ```

## 2. 结果处理

- `detect_odr_violation=0` 是 CI 既有约定(共享库场景误报),不要再额外
  放宽 `ASAN_OPTIONS`。
- 默认 `BUILD_DIR_REL` 为 `build-ai/skill_asan`;正被其他任务使用时先改为
  `build-ai/skill_asan_<task_name>`,再派生 `BUILD_DIR`,不得共用或清理
  其他构建目录。
- ASan 报告(heap-use-after-free、leak、stack-buffer-overflow 等)一律
  按真实缺陷报告;用户同时要求修复时再定位并修改源码,不通过
  suppression 文件消音。
- 只在用户显式调用本 skill 时才执行构建与测试;日常改码流程仍遵循
  `AGENTS.md` 强制规则第 3 条(不主动构建)。
