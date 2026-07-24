---
name: bench
description: >-
  构建并运行 vlink-bench 性能基准(showcase/quick/full 预设),生成
  HTML/JSON 报告。用户要求"跑 bench"、"性能测试"、对比后端吞吐/延迟、
  验证性能回归时使用。
---

# 性能基准测试

`vlink-bench` 是仓库自带的基准 CLI(`cli/bench/`),由默认开启的
`ENABLE_CLI_BENCH=ON` 构建,二进制位于
`build-ai/skill_bench/output/bin/vlink-bench`。
官方 Wiki 的基准页即来自 `quick` 预设的一次运行
(`.github/scripts/release-bench.sh`)。

## 1. 构建

Linux / macOS:

```bash
REPO_ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR="$REPO_ROOT/build-ai/skill_bench"
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
  -DENABLE_CXX_STD_20=OFF \
  -DENABLE_CLI_BENCH=ON
cmake --build "$BUILD_DIR" --target vlink-bench --parallel "$BUILD_JOBS"
```

Windows PowerShell:

```powershell
$RepoRoot = git rev-parse --show-toplevel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$BuildDir = Join-Path (Join-Path $RepoRoot "build-ai") "skill_bench"
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
& cmake -S $RepoRoot -B $BuildDir `
  -DENABLE_CXX_STD_20=OFF `
  -DENABLE_CLI_BENCH=ON
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $BuildDir --target vlink-bench --parallel $BuildJobs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

若 `build-ai/skill_bench` 已配置过、配置仍适用且未被其他任务使用,
可直接执行第二条;否则使用 `skill_bench_<task_name>`。配置失败必须原样
报告,不得继续使用陈旧构建目录或清理其他构建目录。

## 2. 运行

与 CI 发布报告一致的跑法:

Linux / macOS:

```bash
BENCH_REPORT_DIR="$(mktemp -d)"
"$BUILD_DIR/output/bin/vlink-bench" run \
  --preset quick \
  --report html,json \
  --silent \
  -o "$BENCH_REPORT_DIR/vlink-bench-report"
```

Windows PowerShell:

```powershell
$BenchReportDir = Join-Path ([System.IO.Path]::GetTempPath()) (
  "vlink-bench-" + [guid]::NewGuid()
)
New-Item -ItemType Directory -Path $BenchReportDir | Out-Null
$Bench = Join-Path $BuildDir "output\bin\vlink-bench.exe"
$Output = Join-Path $BenchReportDir "vlink-bench-report"
& $Bench run --preset quick --report html,json --silent -o $Output
```

- `--preset`:`showcase`(默认,演示)/ `quick`(CI 用,较快)/
  `full`(完整矩阵,耗时长)。
- `--mode`:运行形态 `local-direct`、`local-loop` 或 `process`;
  传输后端通过 `--url` 选择。
- `-o/--output`:报告文件前缀(不含扩展名),生成 `.html` / `.json`。
- 每次使用独立 `mktemp` 目录,避免并发运行或重复执行覆盖报告;完成后报告
  该目录,由用户决定保留或删除。
- 其他子命令:`plot`(由 JSON 重绘报告)、`pub`/`sub`(跨进程手动
  压测端)。完整参数见 `vlink-bench run --help`。

## 3. 结果处理

- 基准数据受宿主机负载影响大:对比性能回归时,before/after 必须在同一台
  空闲机器、同一预设下运行;正式对比使用 `full --repeat 3`。
- `$BUILD_JOBS` / `BuildJobs` 必须严格等于
  `max(真实物理核心数 - 1, 1)`。禁止改用逻辑 CPU 数、裸
  `--parallel`/`-j` 或固定高并行度;无法可靠获取时固定单核,且同一
  时刻只运行一个本地构建,防止编译卡死或耗尽内存。
- 涉及性能结论时,引用 JSON 报告中的具体指标(吞吐/延迟分位数),不凭
  单次 HTML 观感下结论。
- 退出码 `2` 表示运行和报告生成完成,但存在失败 case;仍需读取 JSON
  定位失败项,不得误报为命令执行故障。其他非零退出码按执行失败处理。
- 只在用户显式调用本 skill 时才执行构建与运行;日常改码流程仍遵循
  `AGENTS.md` 强制规则第 3 条(不主动构建)。
