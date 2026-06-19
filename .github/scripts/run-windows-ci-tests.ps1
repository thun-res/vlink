$ErrorActionPreference = "Stop"

$BuildDir = if ($env:BUILD_DIR) { $env:BUILD_DIR } else { "build-test" }
$BaseExclude = if ($env:CTEST_EXCLUDE_BASE) { $env:CTEST_EXCLUDE_BASE } else { "^(xmltest|ucunit)$|^CUnit_" }
$RunPythonTests = if ($env:RUN_PYTHON_TESTS) { $env:RUN_PYTHON_TESTS } else { "1" }
$SplitShmTests = if ($env:SPLIT_SHM_TESTS) { $env:SPLIT_SHM_TESTS } else { "1" }

$RuntimeDirs = @(
  "$PWD\$BuildDir\output\bin",
  "$PWD\$BuildDir\output\lib",
  "$PWD\$BuildDir\output\external\bin",
  "$PWD\$BuildDir\output\external\lib"
)

if ($env:OPENSSL_ROOT_DIR) {
  $RuntimeDirs += Join-Path $env:OPENSSL_ROOT_DIR "bin"
}
if ($env:VCToolsRedistDir) {
  $RuntimeDirs += Join-Path $env:VCToolsRedistDir "x64\Microsoft.VC143.CRT"
}

$ExistingRuntimeDirs = $RuntimeDirs | Where-Object { Test-Path $_ } | Sort-Object -Unique
$env:Path = ($ExistingRuntimeDirs -join ";") + ";$env:Path"

$ProxyExe = Join-Path $PWD "$BuildDir\output\bin\vlink-proxy.exe"
if (!(Test-Path $ProxyExe)) {
  throw "Missing vlink-proxy before unit tests: $ProxyExe"
}

$script:Proxy = $null
$script:ProxyOut = $null
$script:ProxyErr = $null

function Invoke-CheckedNative {
  param(
    [Parameter(Mandatory = $true)]
    [string] $FilePath,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Arguments
  )

  & $FilePath @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "$FilePath failed with exit code $LASTEXITCODE"
  }
}

function Stop-VLinkProxy {
  if ($null -eq $script:Proxy -or $script:Proxy.HasExited) {
    $script:Proxy = $null
    return
  }

  try {
    if (-not $script:Proxy.CloseMainWindow()) {
      $ProxyId = $script:Proxy.Id
      & taskkill.exe /PID $ProxyId /T | Out-Null
    }
  } catch {
    Write-Host "vlink-proxy graceful stop request failed: $($_.Exception.Message)"
  }

  if (-not $script:Proxy.WaitForExit(10000)) {
    Write-Host "vlink-proxy did not exit gracefully; forcing shutdown"
    if ($script:ProxyOut -and (Test-Path $script:ProxyOut)) { Get-Content $script:ProxyOut -Tail 100 }
    if ($script:ProxyErr -and (Test-Path $script:ProxyErr)) { Get-Content $script:ProxyErr -Tail 100 }
    $ProxyId = $script:Proxy.Id
    Stop-Process -Id $ProxyId -Force -ErrorAction SilentlyContinue
    Wait-Process -Id $ProxyId -ErrorAction SilentlyContinue
  }

  $script:Proxy = $null
}

function Start-VLinkProxy {
  param(
    [Parameter(Mandatory = $true)]
    [string] $Name
  )

  Stop-VLinkProxy

  $SafeName = $Name -replace "[^A-Za-z0-9_.-]", "_"
  $script:ProxyOut = Join-Path $PWD "$BuildDir\$SafeName.out.log"
  $script:ProxyErr = Join-Path $PWD "$BuildDir\$SafeName.err.log"
  $script:Proxy = Start-Process -FilePath $ProxyExe -ArgumentList @("-c", "-n", "-m", "off") `
    -PassThru -RedirectStandardOutput $script:ProxyOut -RedirectStandardError $script:ProxyErr

  Start-Sleep -Seconds 2
  if ($script:Proxy.HasExited) {
    if (Test-Path $script:ProxyOut) { Get-Content $script:ProxyOut }
    if (Test-Path $script:ProxyErr) { Get-Content $script:ProxyErr }
    throw "vlink-proxy exited before unit tests"
  }
}

function Invoke-VLinkCTest {
  param(
    [string[]] $ExtraArgs
  )

  $Args = @("--test-dir", $BuildDir, "--output-on-failure", "--timeout", "180", "--parallel", "1") + $ExtraArgs
  Invoke-CheckedNative "ctest" @Args
}

function Get-ShmCTestSuites {
  $Suites = @()
  $ListOutput = & ctest --test-dir $BuildDir -N
  if ($LASTEXITCODE -ne 0) {
    throw "ctest -N failed with exit code $LASTEXITCODE"
  }

  foreach ($Line in $ListOutput) {
    if ($Line -match "^\s*Test\s+#[0-9]+:\s+(\S+)") {
      $Name = $Matches[1]
      if ($Name -match "^shm2?-") {
        $Suites += $Name
      }
    }
  }

  return $Suites
}

function Get-PythonRuntimeDirs {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.FileInfo] $Module
  )

  $DllDirs = Get-ChildItem -Path "$PWD\$BuildDir" -Recurse -Include "*.dll","*.pyd" -ErrorAction SilentlyContinue |
    ForEach-Object { $_.DirectoryName }

  $PythonExe = Get-Command python -ErrorAction Stop
  $Dirs = @($ExistingRuntimeDirs, $DllDirs, $Module.DirectoryName, (Split-Path $PythonExe.Source -Parent)) |
    ForEach-Object { $_ } |
    Where-Object { $_ -and (Test-Path $_) } |
    Sort-Object -Unique

  return $Dirs
}

function Invoke-VLinkPythonTests {
  if ($RunPythonTests -ne "1") {
    return
  }

  $Module = Get-ChildItem -Path "$PWD\$BuildDir" -Recurse -Filter "_vlink_nanobind*.pyd" | Select-Object -First 1
  if (-not $Module) {
    throw "Missing _vlink_nanobind*.pyd under $BuildDir"
  }

  $DllSearchDirs = Get-PythonRuntimeDirs -Module $Module
  $env:PYTHONPATH = (@("$PWD\python_api", "$PWD\$BuildDir\output\lib", $Module.DirectoryName) |
    Sort-Object -Unique) -join ";"
  $env:Path = ($DllSearchDirs -join ";") + ";$env:Path"
  $env:VLINK_WINDOWS_DLL_DIRS = $DllSearchDirs -join ";"
  $env:VLINK_NANOBIND_MODULE = $Module.FullName

  $Bootstrap = Join-Path $env:RUNNER_TEMP "vlink-python-test.py"
  $BootstrapSource = @'
import os
import runpy

_dll_handles = []
_dll_dirs = [path for path in os.environ.get("VLINK_WINDOWS_DLL_DIRS", "").split(os.pathsep) if path]
for path in _dll_dirs:
    _dll_handles.append(os.add_dll_directory(path))

runpy.run_path(os.path.join(os.environ["GITHUB_WORKSPACE"], "python_api", "test", "test_vlink.py"),
               run_name="__main__")
'@
  $BootstrapSource | Set-Content -Path $Bootstrap -Encoding utf8
  Invoke-CheckedNative "python" $Bootstrap
}

try {
  if ($SplitShmTests -eq "1") {
    Start-VLinkProxy "vlink-proxy-main"
    Invoke-VLinkCTest @("--exclude-regex", "$BaseExclude|^shm2?-")

    foreach ($Suite in (Get-ShmCTestSuites)) {
      Invoke-VLinkCTest @("--tests-regex", "^$Suite$")
    }

    Invoke-VLinkPythonTests
    Stop-VLinkProxy
  } else {
    Start-VLinkProxy "vlink-proxy-main"
    Invoke-VLinkCTest @("--exclude-regex", $BaseExclude)
    Invoke-VLinkPythonTests
    Stop-VLinkProxy
  }
} finally {
  Stop-VLinkProxy
}
