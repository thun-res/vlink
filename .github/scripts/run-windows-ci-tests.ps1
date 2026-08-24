$ErrorActionPreference = "Stop"

$BuildDir = if ($env:BUILD_DIR) { $env:BUILD_DIR } else { "build-test" }
$CTestExclude = "^(xmltest|ucunit)$|^CUnit_"
$env:VLINK_DDS_IP = "127.0.0.1"
$CTestParallel = if ($env:CMAKE_BUILD_PARALLEL_LEVEL) {
  [int]$env:CMAKE_BUILD_PARALLEL_LEVEL
} else {
  4
}

$CTestAllowedFailures = if ($null -ne $env:WINDOWS_CTEST_ALLOWED_FAILURES -and $env:WINDOWS_CTEST_ALLOWED_FAILURES -ne "") {
  [int]$env:WINDOWS_CTEST_ALLOWED_FAILURES
} else {
  0
}

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

function Invoke-CTestWithTolerance {
  param(
    [Parameter(Mandatory = $true)]
    [string[]] $Arguments,
    [int] $AllowedTestFailures = 0
  )

  $PreviousErrorActionPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  $global:LASTEXITCODE = 1
  try {
    & ctest @Arguments 2>&1 | Tee-Object -Variable CTestOutput
    $CTestExit = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $PreviousErrorActionPreference
  }

  if ($CTestExit -eq 0) {
    return
  }

  $OutputText = ($CTestOutput | Out-String)

  $SummaryMatch = [regex]::Matches($OutputText, '(?m)^\s*\d+% tests passed,\s+(\d+)\s+tests failed out of\s+(\d+)\s*$') |
    Select-Object -Last 1
  if (-not $SummaryMatch) {
    throw "ctest failed with exit code $CTestExit (unable to determine the failed test count)"
  }

  $FailedCount = [int]$SummaryMatch.Groups[1].Value
  $TotalCount = [int]$SummaryMatch.Groups[2].Value

  if ($FailedCount -lt 1 -or $TotalCount -lt 1) {
    throw "ctest failed with exit code $CTestExit (unexpected summary line: $($SummaryMatch.Value.Trim()))"
  }

  if ($FailedCount -gt $AllowedTestFailures) {
    throw "ctest failed with exit code $CTestExit ($FailedCount failed tests exceed the tolerance of $AllowedTestFailures)"
  }

  $FailedNames = ([regex]::Matches($OutputText, '(?m)^\s*\d+\s+-\s+(.+?)\s+\(.+?\)\s*$') |
    ForEach-Object { $_.Groups[1].Value }) -join ", "

  $WarningMessage = "Windows unit tests had $FailedCount failing test(s) within the tolerance of $AllowedTestFailures and are treated as PASSED: $FailedNames. This MUST be investigated and fixed."

  $EscapedMessage = $WarningMessage -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'

  Write-Warning $WarningMessage
  Write-Host "::warning title=Windows unit tests passed with $FailedCount tolerated failure(s)::$EscapedMessage"

  $global:LASTEXITCODE = 0
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
  Stop-VLinkProxy

  $script:ProxyOut = Join-Path $PWD "$BuildDir\vlink-proxy.out.log"
  $script:ProxyErr = Join-Path $PWD "$BuildDir\vlink-proxy.err.log"
  $script:Proxy = Start-Process -FilePath $ProxyExe -ArgumentList @("-l", "3", "-n", "-m", "off") `
    -PassThru -RedirectStandardOutput $script:ProxyOut -RedirectStandardError $script:ProxyErr

  Start-Sleep -Seconds 2
  if ($script:Proxy.HasExited) {
    if (Test-Path $script:ProxyOut) { Get-Content $script:ProxyOut -Tail 100 }
    if (Test-Path $script:ProxyErr) { Get-Content $script:ProxyErr -Tail 100 }
    throw "vlink-proxy exited before unit tests"
  }
}

try {
  Start-VLinkProxy
  Invoke-CTestWithTolerance -Arguments @("--test-dir", $BuildDir, "--output-on-failure", "--timeout", "180", "--parallel", "$CTestParallel", "--exclude-regex", $CTestExclude) -AllowedTestFailures $CTestAllowedFailures
} finally {
  Stop-VLinkProxy
}

exit 0
