# 完整单元测试

## Linux 与 macOS

```bash
REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"
BUILD_DIR_REL=build-ai/skill_test
export PYTHONPYCACHEPREFIX="$REPO_ROOT/$BUILD_DIR_REL/__pycache__"
BUILD_DIR="$BUILD_DIR_REL" bash .github/scripts/run-posix-ci-tests.sh
```

POSIX runner 会设置 `VLINK_DDS_IP=127.0.0.1` 和平台动态库路径,启动并
清理 `vlink-proxy`,以默认 4 路并行、单 suite 180 秒超时运行 CTest,
并排除仓库固定的第三方测试。

## Windows

```powershell
$RepoRoot = git rev-parse --show-toplevel
Set-Location $RepoRoot
$BuildDirRel = "build-ai/skill_test"
$BuildDir = Join-Path $RepoRoot $BuildDirRel
$env:BUILD_DIR = $BuildDirRel
$env:PYTHONPYCACHEPREFIX = Join-Path $BuildDir "__pycache__"
$env:WINDOWS_CTEST_ALLOWED_FAILURES = "0"
& "$RepoRoot/.github/scripts/run-windows-ci-tests.ps1"
if ($LASTEXITCODE -ne 0) {
  throw "Unit tests failed with exit code $LASTEXITCODE"
}
```

Windows runner 会补充运行库搜索路径,设置测试 IP,管理
`vlink-proxy.exe`,并采用与 POSIX 一致的并行度和超时。必须把
`WINDOWS_CTEST_ALLOWED_FAILURES` 设为 `0`,禁止继承外部非零值。

## 完整性判定

- `BUILD_DIR` 必须是 `build-ai/skill_test` 或其细分任务形式的仓库根相对路径,
  并与配置、构建阶段的 `BUILD_DIR_REL` 或 `$BuildDirRel` 一致。
- `vlink-proxy` 缺失或提前退出属于测试链路失败,不得绕过。
- 只有 runner 正常结束且没有失败 suite 时,才能报告普通单元测试全量
  通过。
- 需要改变并行度时使用 `CMAKE_BUILD_PARALLEL_LEVEL`,不要修改 runner
  默认值。
