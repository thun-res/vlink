param(
  [string]$Root = "build-conan/packup/win32/vlink"
)

$ErrorActionPreference = "Stop"

# Verify the staged Windows portable tree is self-contained. Every imported DLL
# of each .exe/.dll under bin/ must be satisfied by a DLL bundled in the tree, a
# Windows API set (api-ms-win-* / ext-ms-*), or a DLL present in System32.
# Anything else would be missing on a user's machine. Mirrors
# check-linux-runtime-closure.sh.

$binDir = Join-Path $Root "bin"
if (-not (Test-Path $binDir)) {
  Write-Host "::error::Bundle bin directory not found: $binDir"
  exit 1
}

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbin) {
  Write-Host "::error::dumpbin.exe not found on PATH (run inside the MSVC developer environment)"
  exit 1
}

$images = @(Get-ChildItem -Path $binDir -Recurse -File -Include *.dll, *.exe)
$bundled = @{}
foreach ($image in $images) {
  $bundled[$image.Name.ToLowerInvariant()] = $true
}

$system32 = Join-Path $env:SystemRoot "System32"
$status = 0

foreach ($image in $images) {
  $output = & $dumpbin.Source /DEPENDENTS $image.FullName 2>&1
  if ($LASTEXITCODE -ne 0) {
    Write-Host "::error::dumpbin failed for $($image.Name)"
    $output | ForEach-Object { Write-Host $_ }
    $status = 1
    continue
  }
  $deps = $output |
    Select-String -Pattern '^\s{2,}([A-Za-z0-9_.+\-]+\.dll)\s*$' |
    ForEach-Object { $_.Matches[0].Groups[1].Value }
  foreach ($dep in $deps) {
    $depLower = $dep.ToLowerInvariant()
    if ($bundled.ContainsKey($depLower)) { continue }
    if ($depLower -like 'api-ms-win-*' -or $depLower -like 'ext-ms-*') { continue }
    if (Test-Path (Join-Path $system32 $dep)) { continue }
    Write-Host "::error::$($image.Name): missing runtime dependency $dep"
    $status = 1
  }
}

exit $status
