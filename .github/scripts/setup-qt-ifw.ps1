param(
  [Parameter(Mandatory = $true)][string]$QtHost,
  [Parameter(Mandatory = $true)][string]$QtArch
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $PSCommandPath
. (Join-Path $ScriptDir "github-progress.ps1")

if ($QtHost -ne "windows" -or $QtArch -ne "win64_msvc2022_64") {
  throw "Unsupported Qt package: $QtHost $QtArch"
}

$SetupRoot = if ($env:VLINK_SETUP_ROOT) { $env:VLINK_SETUP_ROOT } else { Join-Path $env:USERPROFILE ".vlink-ci" }
$QtRoot = Join-Path $SetupRoot "qt"
$QtVersion = "6.8.3"
$DownloadRoot = Join-Path $SetupRoot "downloads"
$QtDir = Join-Path $QtRoot "$QtVersion\msvc2022_64"
$IfwDir = Join-Path $QtRoot "Tools\QtInstallerFramework\4.11"
$QtRepo = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_683/qt6_683/qt.qt6.683.win64_msvc2022_64"
$IfwRepo = "https://download.qt.io/online/qtsdkrepository/windows_x86/ifw/tools_ifw_411/qt.tools.ifw.411"
$QtPrefix = "6.8.3-0-202503201308"
$IfwPrefix = "4.11.0-0-202603231357"

function Get-FileWithRetry {
  param([string]$Uri, [string]$Path)
  for ($I = 1; $I -le 5; $I++) {
    try {
      Invoke-WebRequest -Uri $Uri -OutFile $Path
      return
    } catch {
      Remove-Item -Force -ErrorAction SilentlyContinue $Path
      if ($I -eq 5) { throw }
      Start-Sleep -Seconds ($I * 5)
    }
  }
}

function Expand-QtArchive {
  param([string]$Repo, [string]$Prefix, [string]$Archive, [string]$Destination)
  $FileName = "$Prefix$Archive"
  $ArchivePath = Join-Path $DownloadRoot $FileName
  $ShaPath = "$ArchivePath.sha1"
  Get-FileWithRetry "$Repo/$FileName" $ArchivePath
  Get-FileWithRetry "$Repo/$FileName.sha1" $ShaPath
  $Expected = ((Get-Content $ShaPath -Raw).Trim() -split "\s+")[0].ToLowerInvariant()
  $Actual = (Get-FileHash $ArchivePath -Algorithm SHA1).Hash.ToLowerInvariant()
  if ($Actual -ne $Expected) {
    throw "SHA1 mismatch: $FileName"
  }
  New-Item -ItemType Directory -Force -Path $Destination | Out-Null
  & $SevenZip x -y "-o$Destination" $ArchivePath | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to extract $FileName"
  }
  Remove-Item -Force $ArchivePath, $ShaPath
}

$Qmake = Join-Path $QtDir "bin\qmake.exe"
$BinaryCreator = Join-Path $IfwDir "bin\binarycreator.exe"

$ProgressSteps = @("Prepare download cache")
if (!(Test-Path $Qmake)) {
  $ProgressSteps += @(
    "Reset Qt cache",
    "Install Qt base archive",
    "Install Qt SVG archive",
    "Install D3D compiler archive",
    "Install OpenGL software archive"
  )
} else {
  $ProgressSteps += "Use cached Qt"
}
if (!(Test-Path $BinaryCreator)) {
  $ProgressSteps += @(
    "Reset QtIFW cache",
    "Install QtIFW archive"
  )
} else {
  $ProgressSteps += "Use cached QtIFW"
}
$ProgressSteps += @("Verify Qt tools", "Export Qt environment")

Initialize-GitHubProgress "Setup Qt and QtIFW" $ProgressSteps

Invoke-GitHubProgressStep "Prepare download cache" {
  New-Item -ItemType Directory -Force -Path $DownloadRoot | Out-Null
  $script:SevenZip = (Get-Command 7z.exe -ErrorAction Stop).Source
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
}

if (!(Test-Path $Qmake)) {
  Invoke-GitHubProgressStep "Reset Qt cache" {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -Path (Join-Path $QtRoot $QtVersion)
  }
  Invoke-GitHubProgressStep "Install Qt base archive" {
    Expand-QtArchive $QtRepo $QtPrefix "qtbase-Windows-Windows_11_23H2-MSVC2022-Windows-Windows_11_23H2-X86_64.7z" $QtDir
  }
  Invoke-GitHubProgressStep "Install Qt SVG archive" {
    Expand-QtArchive $QtRepo $QtPrefix "qtsvg-Windows-Windows_11_23H2-MSVC2022-Windows-Windows_11_23H2-X86_64.7z" $QtDir
  }
  Invoke-GitHubProgressStep "Install D3D compiler archive" {
    Expand-QtArchive $QtRepo $QtPrefix "d3dcompiler_47-x64.7z" (Join-Path $QtDir "bin")
  }
  Invoke-GitHubProgressStep "Install OpenGL software archive" {
    Expand-QtArchive $QtRepo $QtPrefix "opengl32sw-64-mesa_11_2_2-signed_sha256.7z" (Join-Path $QtDir "bin")
  }
} else {
  Invoke-GitHubProgressStep "Use cached Qt" {
    if (!(Test-Path $Qmake)) {
      throw "qmake.exe not found: $Qmake"
    }
  }
}

if (!(Test-Path $BinaryCreator)) {
  Invoke-GitHubProgressStep "Reset QtIFW cache" {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -Path $IfwDir
  }
  Invoke-GitHubProgressStep "Install QtIFW archive" {
    Expand-QtArchive $IfwRepo $IfwPrefix "ifw-win-x64.7z" $IfwDir
  }
} else {
  Invoke-GitHubProgressStep "Use cached QtIFW" {
    if (!(Test-Path $BinaryCreator)) {
      throw "binarycreator.exe not found: $BinaryCreator"
    }
  }
}

Invoke-GitHubProgressStep "Verify Qt tools" {
  if (!(Test-Path $Qmake)) {
    throw "qmake.exe not found: $Qmake"
  }
  if (!(Test-Path $BinaryCreator)) {
    throw "binarycreator.exe not found: $BinaryCreator"
  }
}

Invoke-GitHubProgressStep "Export Qt environment" {
  "QT_DIR=$($QtDir.Replace("\", "/"))" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
  "QTIFW_DIR=$($IfwDir.Replace("\", "/"))" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
}
