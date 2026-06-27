$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $PSCommandPath
. (Join-Path $ScriptDir "github-progress.ps1")

$ProgressSteps = @()
if ($env:CMAKE_GENERATOR) {
  $ProgressSteps += "Configure Conan generator"
}
$ProgressSteps += @(
  "Build Conan package",
  "Check runtime closure"
)

Initialize-GitHubProgress "Release portable Windows" $ProgressSteps

if (-not $env:CONAN_HOME) {
  $env:CONAN_HOME = Join-Path $env:USERPROFILE ".conan2"
}
if (-not $env:CPM_SOURCE_CACHE) {
  $env:CPM_SOURCE_CACHE = Join-Path $env:USERPROFILE ".vlink-cpm-cache"
}

if ($env:CMAKE_GENERATOR) {
  Invoke-GitHubProgressStep "Configure Conan generator" {
    New-Item -ItemType Directory -Force -Path $env:CONAN_HOME | Out-Null
    $GlobalConf = Join-Path $env:CONAN_HOME "global.conf"
    $Utf8NoBom = New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false
    [System.IO.File]::AppendAllText(
      $GlobalConf,
      "&:tools.cmake.cmaketoolchain:generator=$env:CMAKE_GENERATOR`r`n",
      $Utf8NoBom
    )
  }
}

Invoke-GitHubProgressStep "Build Conan package" {
  & cmd.exe /d /s /c "packup\build-conan.bat ."
  if ($LASTEXITCODE -ne 0) {
    throw "packup\build-conan.bat failed with exit code $LASTEXITCODE"
  }
}

Invoke-GitHubProgressStep "Check runtime closure" {
  & (Join-Path $ScriptDir "check-windows-runtime-closure.ps1")
  if ($LASTEXITCODE -ne 0) {
    throw "Windows runtime closure check failed with exit code $LASTEXITCODE"
  }
}
