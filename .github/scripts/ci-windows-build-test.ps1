$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $PSCommandPath
. (Join-Path $ScriptDir "github-progress.ps1")

Initialize-GitHubProgress "CI Windows build/test" @(
  "Install Python packages",
  "Resolve Python executable",
  "Configure CMake",
  "Build",
  "Run tests"
)

Invoke-GitHubProgressStep "Install Python packages" {
  Invoke-CheckedCommand python -m pip install -q pip==26.1.2
  Invoke-CheckedCommand python -m pip install -q nanobind==2.13.0
}

Invoke-GitHubProgressStep "Resolve Python executable" {
  $script:PythonExecutable = (python -c "import sys; print(sys.executable)")
  if ($LASTEXITCODE -ne 0) {
    throw "python executable lookup failed with exit code $LASTEXITCODE"
  }
}
$PythonExecutable = $PythonExecutable.Replace("\", "/")
$ArgsList = @("-S", ".", "-B", "build-test")
$ArgsList += $env:VLINK_CI_CMAKE_ARGS -split '\s+'
if ($env:VLINK_CI_EXTRA_CMAKE_ARGS) {
  $ArgsList += $env:VLINK_CI_EXTRA_CMAKE_ARGS -split '\s+'
}
$ArgsList += @("-DPython_EXECUTABLE=$PythonExecutable")

Invoke-GitHubProgressStep "Configure CMake" {
  & cmake @ArgsList
  if ($LASTEXITCODE -ne 0) {
    throw "cmake configure failed with exit code $LASTEXITCODE"
  }
}

Invoke-GitHubProgressStep "Build" {
  Invoke-CheckedCommand cmake --build build-test --parallel
}

$env:BUILD_DIR = "build-test"
Invoke-GitHubProgressStep "Run tests" {
  & (Join-Path $ScriptDir "run-windows-ci-tests.ps1")
  if ($LASTEXITCODE -ne 0) {
    throw "Windows CI tests failed with exit code $LASTEXITCODE"
  }
}
