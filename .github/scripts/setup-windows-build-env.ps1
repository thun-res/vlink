param(
  [switch]$UseSccache
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $PSCommandPath
. (Join-Path $ScriptDir "github-progress.ps1")

$ProgressSteps = @()
if ($UseSccache) {
  $ProgressSteps += "Configure sccache environment"
}
$ProgressSteps += @(
  "Locate Visual Studio",
  "Load MSVC environment",
  "Export developer environment",
  "Configure compiler tools",
  "Prepare patch tool",
  "Resolve OpenSSL"
)
if ($UseSccache) {
  $ProgressSteps += "Start sccache server"
}

Initialize-GitHubProgress "Setup Windows build environment" $ProgressSteps

if ($UseSccache) {
  Start-GitHubProgressStep "Configure sccache environment"
  try {
    $env:SCCACHE_GHA_ENABLED = "true"
    $env:SCCACHE_CACHE_SIZE = "3G"
    $env:SCCACHE_IDLE_TIMEOUT = "0"
    "SCCACHE_GHA_ENABLED=$env:SCCACHE_GHA_ENABLED" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
    "SCCACHE_CACHE_SIZE=$env:SCCACHE_CACHE_SIZE" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
    "SCCACHE_IDLE_TIMEOUT=$env:SCCACHE_IDLE_TIMEOUT" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
    "SCCACHE_GHA_VERSION=$env:SCCACHE_GHA_VERSION" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
    Complete-GitHubProgressStep "Configure sccache environment"
  } catch {
    Fail-GitHubProgressStep "Configure sccache environment"
    throw
  }
}

Start-GitHubProgressStep "Locate Visual Studio"
try {
  $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (!(Test-Path $VsWhere)) {
    throw "vswhere.exe not found: $VsWhere"
  }

  $VsPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (!$VsPath) {
    throw "Visual Studio with MSVC x64 tools was not found"
  }

  $DevCmd = Join-Path $VsPath "Common7\Tools\VsDevCmd.bat"
  if (!(Test-Path $DevCmd)) {
    throw "VsDevCmd.bat not found: $DevCmd"
  }
  Complete-GitHubProgressStep "Locate Visual Studio"
} catch {
  Fail-GitHubProgressStep "Locate Visual Studio"
  throw
}

Start-GitHubProgressStep "Load MSVC environment"
try {
  $OriginalPath = New-Object -TypeName "System.Collections.Generic.HashSet[string]" -ArgumentList ([System.StringComparer]::OrdinalIgnoreCase)
  foreach ($Entry in ($env:Path -split ";")) {
    if ($Entry) {
      [void]$OriginalPath.Add($Entry)
    }
  }

  $DevEnv = & cmd.exe /s /c "`"$DevCmd`" -arch=x64 -host_arch=x64 && set"
  if ($LASTEXITCODE -ne 0) {
    throw "VsDevCmd.bat failed with exit code $LASTEXITCODE"
  }

  $Parsed = @{}
  foreach ($Line in $DevEnv) {
    if ($Line -notmatch "^[^=]+=") {
      continue
    }
    $Name, $Value = $Line -split "=", 2
    $Parsed[$Name] = $Value
  }
  Complete-GitHubProgressStep "Load MSVC environment"
} catch {
  Fail-GitHubProgressStep "Load MSVC environment"
  throw
}

Start-GitHubProgressStep "Export developer environment"
try {
  if ($Parsed.ContainsKey("Path")) {
    $env:Path = $Parsed["Path"]
    foreach ($Entry in ($Parsed["Path"] -split ";")) {
      if ($Entry -and (Test-Path $Entry) -and !$OriginalPath.Contains($Entry)) {
        $Entry | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
      }
    }
  }

  $EnvNames = @(
    "DevEnvDir",
    "ExtensionSdkDir",
    "Framework40Version",
    "FrameworkDir",
    "FrameworkDIR64",
    "FrameworkVersion",
    "FrameworkVersion64",
    "INCLUDE",
    "LIB",
    "LIBPATH",
    "NETFXSDKDir",
    "UCRTVersion",
    "UniversalCRTSdkDir",
    "VCIDEInstallDir",
    "VCINSTALLDIR",
    "VCToolsInstallDir",
    "VCToolsRedistDir",
    "VCToolsVersion",
    "VisualStudioVersion",
    "VSINSTALLDIR",
    "WindowsLibPath",
    "WindowsSdkBinPath",
    "WindowsSdkDir",
    "WindowsSDKLibVersion",
    "WindowsSDKVersion",
    "VSCMD_ARG_app_plat",
    "VSCMD_ARG_HOST_ARCH",
    "VSCMD_ARG_TGT_ARCH",
    "VSCMD_VER"
  )

  foreach ($Name in $EnvNames) {
    if ($Parsed.ContainsKey($Name)) {
      $Value = $Parsed[$Name]
      Set-Item -Path "Env:$Name" -Value $Value
      "$Name=$Value" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
    }
  }
  Complete-GitHubProgressStep "Export developer environment"
} catch {
  Fail-GitHubProgressStep "Export developer environment"
  throw
}

Start-GitHubProgressStep "Configure compiler tools"
try {
  $Cl = Get-Command cl.exe -ErrorAction Stop
  Get-Command link.exe -ErrorAction Stop | Out-Null
  $ClPath = $Cl.Source.Replace("\", "/")
  Set-Item -Path Env:CC -Value $ClPath
  Set-Item -Path Env:CXX -Value $ClPath
  "CC=$ClPath" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
  "CXX=$ClPath" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
  Complete-GitHubProgressStep "Configure compiler tools"
} catch {
  Fail-GitHubProgressStep "Configure compiler tools"
  throw
}

Start-GitHubProgressStep "Prepare patch tool"
try {
  $GitPatch = Join-Path $env:ProgramFiles "Git\usr\bin\patch.exe"
  if (!(Test-Path $GitPatch)) {
    throw "patch.exe not found: $GitPatch"
  }
  $ToolDir = Join-Path $env:RUNNER_TEMP "vlink-tools"
  New-Item -ItemType Directory -Force -Path $ToolDir | Out-Null
  Copy-Item -Force $GitPatch (Join-Path $ToolDir "patch.exe")
  $ToolDir | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
  Complete-GitHubProgressStep "Prepare patch tool"
} catch {
  Fail-GitHubProgressStep "Prepare patch tool"
  throw
}

function Find-OpenSslRoot {
  $Candidates = @(
    (Join-Path $env:ProgramFiles "OpenSSL"),
    (Join-Path $env:ProgramFiles "OpenSSL-Win64")
  )
  if (${env:ProgramFiles(x86)}) {
    $Candidates += Join-Path ${env:ProgramFiles(x86)} "OpenSSL-Win32"
  }
  $Candidates += Get-ChildItem -Path $env:ProgramFiles -Directory -Filter "OpenSSL*" -ErrorAction SilentlyContinue |
    ForEach-Object { $_.FullName }

  foreach ($Candidate in ($Candidates | Where-Object { $_ } | Select-Object -Unique)) {
    if (Test-Path (Join-Path $Candidate "include\openssl\ssl.h")) {
      return $Candidate
    }
  }
  return $null
}

Start-GitHubProgressStep "Resolve OpenSSL"
try {
  $OpenSslRoot = Find-OpenSslRoot
  if (-not $OpenSslRoot) {
    choco install openssl --no-progress --limit-output --yes
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1641 -and $LASTEXITCODE -ne 3010) {
      throw "choco install openssl failed with exit code $LASTEXITCODE"
    }
    $OpenSslRoot = Find-OpenSslRoot
  }
  if (-not $OpenSslRoot) {
    throw "OpenSSL headers not found under Program Files after Chocolatey install"
  }
  "OPENSSL_ROOT_DIR=$($OpenSslRoot.Replace("\", "/"))" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
  Complete-GitHubProgressStep "Resolve OpenSSL"
} catch {
  Fail-GitHubProgressStep "Resolve OpenSSL"
  throw
}

if ($UseSccache) {
  Start-GitHubProgressStep "Start sccache server"
  try {
    $Sccache = (Get-Command sccache.exe -ErrorAction Stop).Source
    Split-Path -Parent $Sccache | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
    & $Sccache --start-server *> $null
    if ($LASTEXITCODE -ne 0) {
      throw "sccache failed to start"
    }
    Complete-GitHubProgressStep "Start sccache server"
  } catch {
    Fail-GitHubProgressStep "Start sccache server"
    throw
  }
}
