$ErrorActionPreference = "Stop"

$BuildDir = if ($env:BUILD_DIR) { $env:BUILD_DIR } else { "build-test" }
$RuntimeDirs = @(
  "$PWD\$BuildDir\output\bin",
  "$PWD\$BuildDir\output\lib",
  "$PWD\$BuildDir\output\external\bin",
  "$PWD\$BuildDir\output\external\lib"
)

$ExistingRuntimeDirs = $RuntimeDirs | Where-Object { Test-Path $_ }
$env:Path = ($ExistingRuntimeDirs -join ";") + ";$env:Path"

$ProxyExe = Join-Path $PWD "$BuildDir\output\bin\vlink-proxy.exe"
if (!(Test-Path $ProxyExe)) {
  throw "Missing vlink-proxy before unit tests: $ProxyExe"
}

$ProxyOut = Join-Path $PWD "$BuildDir\vlink-proxy.out.log"
$ProxyErr = Join-Path $PWD "$BuildDir\vlink-proxy.err.log"
$Proxy = $null

function Stop-VLinkProxy {
  if ($null -eq $script:Proxy -or $script:Proxy.HasExited) {
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
    if (Test-Path $ProxyOut) { Get-Content $ProxyOut -Tail 100 }
    if (Test-Path $ProxyErr) { Get-Content $ProxyErr -Tail 100 }
    $ProxyId = $script:Proxy.Id
    Stop-Process -Id $ProxyId -Force -ErrorAction SilentlyContinue
    Wait-Process -Id $ProxyId -ErrorAction SilentlyContinue
  }
}

$Proxy = Start-Process -FilePath $ProxyExe -ArgumentList @("-c", "-n", "-m", "off") `
  -PassThru -RedirectStandardOutput $ProxyOut -RedirectStandardError $ProxyErr

try {
  Start-Sleep -Seconds 2
  if ($Proxy.HasExited) {
    if (Test-Path $ProxyOut) { Get-Content $ProxyOut }
    if (Test-Path $ProxyErr) { Get-Content $ProxyErr }
    throw "vlink-proxy exited before unit tests"
  }

  ctest --test-dir $BuildDir --output-on-failure --timeout 180 --parallel 1 --exclude-regex '^(xmltest|ucunit)$|^CUnit_'

  $Module = Get-ChildItem -Path "$PWD\$BuildDir" -Recurse -Filter "_vlink_nanobind*.pyd" | Select-Object -First 1
  if (-not $Module) {
    throw "Missing _vlink_nanobind*.pyd under $BuildDir"
  }

  $DllDirs = Get-ChildItem -Path "$PWD\$BuildDir" -Recurse -Include "*.dll","*.pyd" |
    ForEach-Object { $_.DirectoryName } |
    Sort-Object -Unique
  $DllSearchDirs = $ExistingRuntimeDirs + $DllDirs | Where-Object { Test-Path $_ } | Sort-Object -Unique

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

print("VLink Python extension:", os.environ.get("VLINK_NANOBIND_MODULE", ""))
print("VLink DLL search directories:")
for path in _dll_dirs:
    print("  " + path)

runpy.run_path(os.path.join(os.environ["GITHUB_WORKSPACE"], "python_api", "test", "test_vlink.py"),
               run_name="__main__")
'@
  $BootstrapSource | Set-Content -Path $Bootstrap -Encoding utf8
  python $Bootstrap
} finally {
  Stop-VLinkProxy
}
