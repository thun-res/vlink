# 配置与构建普通测试

## POSIX

Linux 与 macOS 从仓库任意子目录执行:

```bash
REPO_ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR_REL=build-ai/skill_test
BUILD_DIR="$REPO_ROOT/$BUILD_DIR_REL"
export PYTHONPYCACHEPREFIX="$BUILD_DIR/__pycache__"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_CXX_STD_20=OFF \
  -DENABLE_TEST=ON \
  -DENABLE_TEST_WARN=ON \
  -DENABLE_TEST_SANITIZE=OFF \
  -DENABLE_TEST_COVERAGE=OFF \
  -DENABLE_PROXY=ON \
  -DENABLE_EXAMPLES=OFF
cmake --build "$BUILD_DIR" \
  --target vlink-test vlink-proxy --parallel
```

macOS 未自动找到 Homebrew OpenSSL 时,使用本机实际路径补充
`-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`,不要硬编码架构目录。
配置前确认 Fast DDS 或 Cyclone DDS 可被 CMake 找到。CMake 将
`ENABLE_PROXY` 改回 OFF 或未生成 `vlink-proxy` target 时停止,不得
继续运行依赖 Proxy 的测试链路。

## Windows

在 PowerShell 中使用 CI 同款 Ninja 单配置生成器:

```powershell
$RepoRoot = git rev-parse --show-toplevel
$BuildDirRel = "build-ai/skill_test"
$BuildDir = Join-Path $RepoRoot $BuildDirRel
$env:PYTHONPYCACHEPREFIX = Join-Path $BuildDir "__pycache__"
cmake -S $RepoRoot -B $BuildDir -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DENABLE_CXX_STD_20=OFF `
  -DENABLE_TEST=ON `
  -DENABLE_TEST_WARN=ON `
  -DENABLE_TEST_SANITIZE=OFF `
  -DENABLE_TEST_COVERAGE=OFF `
  -DENABLE_PROXY=ON `
  -DENABLE_EXAMPLES=OFF
cmake --build $BuildDir `
  --target vlink-test vlink-proxy --parallel
```

## 构建目录

- AI 创建的构建目录只能位于仓库根 `build-ai/` 下。普通测试固定使用
  `build-ai/skill_test`;生成器或关键开关不兼容时,不得删除已有目录,
  先将 `BUILD_DIR_REL` 或 `$BuildDirRel` 改为描述具体任务的
  `build-ai/skill_test_<task_name>`,再派生绝对路径。
- 配置或构建失败后不得继续运行旧二进制。
- 普通测试必须关闭 `ENABLE_TEST_SANITIZE` 和
  `ENABLE_TEST_COVERAGE`;对应验证分别交给 `/asan`、`/coverage`。
