# 配置与构建普通测试

## POSIX

Linux 与 macOS 从仓库任意子目录执行:

```bash
REPO_ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR_REL=build-ai/skill_test
BUILD_DIR="$REPO_ROOT/$BUILD_DIR_REL"
export PYTHONPYCACHEPREFIX="$BUILD_DIR/__pycache__"
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
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_CXX_STD_20=OFF \
  -DENABLE_C_API=ON \
  -DENABLE_PYTHON_API=ON \
  -DPython_EXECUTABLE="$(command -v python3)" \
  -DENABLE_TEST=ON \
  -DENABLE_TEST_WARN=ON \
  -DENABLE_TEST_SANITIZE=OFF \
  -DENABLE_TEST_COVERAGE=OFF \
  -DENABLE_PROXY=ON \
  -DENABLE_EXAMPLES=OFF
cmake --build "$BUILD_DIR" \
  --target vlink-test vlink-proxy vlink-c-test _vlink_nanobind \
  --parallel "$BUILD_JOBS"
```

macOS 未自动找到 Homebrew OpenSSL 时,使用本机实际路径补充
`-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`,不要硬编码架构目录。
完整或绑定测试包含固定使用 `dds://` 的 `vlink-c-test`,配置前必须确认
Fast DDS 后端 `vlink::dds` 可用;仅不含 `vlink-c-test` 的定向 CTest
才可只依赖 Cyclone DDS。CMake 将 `ENABLE_PROXY` 改回 OFF 或未生成
`vlink-proxy` target 时停止,不得继续运行依赖 Proxy 的测试链路。

## Windows

在 PowerShell 中使用 CI 同款 Ninja 单配置生成器:

```powershell
$RepoRoot = git rev-parse --show-toplevel
$BuildDirRel = "build-ai/skill_test"
$BuildDir = Join-Path $RepoRoot $BuildDirRel
$PythonExecutable = (Get-Command python -ErrorAction Stop).Source
$env:PYTHONPYCACHEPREFIX = Join-Path $BuildDir "__pycache__"
try {
    $Processors = @(Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop)
    if ($Processors.Count -eq 0) {
        throw "No processor information"
    }
    $PhysicalCores = 0
    foreach ($Processor in $Processors) {
        $Cores = [int]$Processor.NumberOfCores
        if ($Cores -lt 1) {
            throw "Invalid physical core count"
        }
        $PhysicalCores += $Cores
    }
} catch {
    $PhysicalCores = 1
}
$BuildJobs = [Math]::Max($PhysicalCores - 1, 1)
cmake -S $RepoRoot -B $BuildDir -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DENABLE_CXX_STD_20=OFF `
  -DENABLE_C_API=ON `
  -DENABLE_PYTHON_API=ON `
  -DPython_EXECUTABLE=$PythonExecutable `
  -DENABLE_TEST=ON `
  -DENABLE_TEST_WARN=ON `
  -DENABLE_TEST_SANITIZE=OFF `
  -DENABLE_TEST_COVERAGE=OFF `
  -DENABLE_PROXY=ON `
  -DENABLE_EXAMPLES=OFF
cmake --build $BuildDir `
  --target vlink-test vlink-proxy vlink-c-test _vlink_nanobind `
  --parallel $BuildJobs
```

## 构建目录

- AI 创建的构建目录只能位于仓库根 `build-ai/` 下。普通测试固定使用
  `build-ai/skill_test`;生成器或关键开关不兼容时,不得删除已有目录,
  先将 `BUILD_DIR_REL` 或 `$BuildDirRel` 改为描述具体任务的
  `build-ai/skill_test_<task_name>`,再派生绝对路径。
- 配置或构建失败后不得继续运行旧二进制。
- 普通测试必须关闭 `ENABLE_TEST_SANITIZE` 和
  `ENABLE_TEST_COVERAGE`;对应验证分别交给 `/asan`、`/coverage`。
- 完整链路必须显式开启 `ENABLE_C_API` 与 `ENABLE_PYTHON_API`,并使用
  配置阶段找到的同一 Python 解释器运行绑定测试。
- `BUILD_JOBS` / `$BuildJobs` 必须严格等于
  `max(真实物理核心数 - 1, 1)`。禁止使用逻辑 CPU 数、裸
  `--parallel`/`-j` 或固定高并行度;探测失败时固定单核,且同一时刻只
  运行一个本地构建,防止编译卡死或耗尽内存。
