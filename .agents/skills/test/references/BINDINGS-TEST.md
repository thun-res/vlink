# C 与 Python 绑定测试

本分册覆盖未注册到 CTest 的独立绑定测试:

- `vlink-c-test`;
- `languages/python_api/test/test_vlink.py`;
- `languages/python_api/test/test_vlink_full.py`;
- `languages/python_api/test/test_vlink_coverage.py`。

运行前必须按
[CONFIGURE-AND-BUILD.md](CONFIGURE-AND-BUILD.md) 显式开启并构建
`ENABLE_C_API`、`ENABLE_PYTHON_API`、`vlink-c-test` 与
`_vlink_nanobind`。四项相互独立;一项失败后继续运行其余项,最终返回
首个非零状态并逐项报告。

## Linux 与 macOS

```bash
REPO_ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR_REL=build-ai/skill_test
BUILD_DIR="$REPO_ROOT/$BUILD_DIR_REL"
export PYTHONPYCACHEPREFIX="$BUILD_DIR/__pycache__"
export PYTHONPATH="$BUILD_DIR/output/lib"
export VLINK_DDS_IP=127.0.0.1
RUNTIME_DIRS="$BUILD_DIR/output/bin:$BUILD_DIR/output/lib:$BUILD_DIR/output/external/lib"
case "$(uname -s)" in
  Darwin)
    export DYLD_LIBRARY_PATH="$RUNTIME_DIRS:${DYLD_LIBRARY_PATH:-}"
    ;;
  *)
    export LD_LIBRARY_PATH="$RUNTIME_DIRS:${LD_LIBRARY_PATH:-}"
    ;;
esac

BINDING_STATUS=0
if "$BUILD_DIR/output/bin/vlink-c-test"; then
  :
else
  TEST_STATUS=$?
  if [ "$BINDING_STATUS" -eq 0 ]; then
    BINDING_STATUS=$TEST_STATUS
  fi
fi
for TEST_FILE in \
  test_vlink.py \
  test_vlink_full.py \
  test_vlink_coverage.py; do
  if python3 "$REPO_ROOT/languages/python_api/test/$TEST_FILE"; then
    :
  else
    TEST_STATUS=$?
    if [ "$BINDING_STATUS" -eq 0 ]; then
      BINDING_STATUS=$TEST_STATUS
    fi
  fi
done
exit "$BINDING_STATUS"
```

## Windows

```powershell
$ErrorActionPreference = "Stop"
$RepoRoot = git rev-parse --show-toplevel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$BuildDirRel = "build-ai/skill_test"
$BuildDir = Join-Path $RepoRoot $BuildDirRel
$PythonExecutable = (Get-Command python -ErrorAction Stop).Source
$env:PYTHONPYCACHEPREFIX = Join-Path $BuildDir "__pycache__"
$env:PYTHONPATH = Join-Path $BuildDir "output\lib"
$env:VLINK_DDS_IP = "127.0.0.1"
$RuntimeDirs = @(
    (Join-Path $BuildDir "output\bin"),
    (Join-Path $BuildDir "output\lib"),
    (Join-Path $BuildDir "output\external\bin"),
    (Join-Path $BuildDir "output\external\lib")
)
$env:Path = ($RuntimeDirs -join ";") + ";$env:Path"

$BindingStatus = 0
& (Join-Path $BuildDir "output\bin\vlink-c-test.exe")
if ($LASTEXITCODE -ne 0 -and $BindingStatus -eq 0) {
    $BindingStatus = $LASTEXITCODE
}
foreach ($TestFile in @(
    "test_vlink.py",
    "test_vlink_full.py",
    "test_vlink_coverage.py"
)) {
    & $PythonExecutable (Join-Path $RepoRoot "languages\python_api\test\$TestFile")
    if ($LASTEXITCODE -ne 0 -and $BindingStatus -eq 0) {
        $BindingStatus = $LASTEXITCODE
    }
}
exit $BindingStatus
```

## 结果判定

- `vlink-c-test` 返回 0 才算 C API 测试通过;预期告警不等于失败。
- 三个 Python 脚本必须分别返回 0,不得用其中一项通过代表全部绑定。
- 动态库或 `_vlink_nanobind` 导入失败属于测试链路失败,不得标为环境跳过。
- 完整 CTest 与四项绑定测试都通过后,才可报告普通测试链路全量通过。
