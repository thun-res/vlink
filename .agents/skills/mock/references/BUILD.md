# Mock 本地构建并行度

只构建当前 mock 场景需要的 target,同一时刻只运行一个本地构建。编译
并行度必须显式等于 `max(真实物理核心数 - 1, 1)`;探测失败时固定单核。

## Linux 与 macOS

```bash
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
cmake --build "$BUILD_DIR" --target <target> --parallel "$BUILD_JOBS"
```

## Windows PowerShell

```powershell
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
& cmake --build $BuildDir --target <target> --parallel $BuildJobs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

禁止使用 `nproc`、`NUMBER_OF_PROCESSORS`、`ProcessorCount`、逻辑 CPU
字段、裸 `--parallel`/`-j`、固定高并行度、`CMAKE_BUILD_PARALLEL_LEVEL`
替代显式参数或透传 backend `-j`。
