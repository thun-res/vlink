param(
  [Parameter(Mandatory = $true)][string]$PlatformDir
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $PSCommandPath
. (Join-Path $ScriptDir "github-progress.ps1")

$PlatformRoot = ($PlatformDir -split '[\\/]', 2)[0]
$Log = Join-Path $env:RUNNER_TEMP "vlink-osg-sdk.log"
$SetupRoot = if ($env:VLINK_SETUP_ROOT) { $env:VLINK_SETUP_ROOT } else { Join-Path $env:USERPROFILE ".vlink-ci" }
$Root = Join-Path $SetupRoot "osg_sdk"

$RepoStep = if (Test-Path (Join-Path $Root ".git")) {
  "Update OSG SDK repository"
} else {
  "Clone OSG SDK repository"
}

Initialize-GitHubProgress "Setup OSG SDK" @(
  "Prepare SDK cache",
  $RepoStep,
  "Install Git LFS hooks",
  "Pull $PlatformRoot SDK files",
  "Export OSG environment"
)

Invoke-GitHubProgressStep "Prepare SDK cache" {
  New-Item -ItemType Directory -Force -Path $SetupRoot | Out-Null
}

Invoke-GitHubProgressStep $RepoStep {
  if (Test-Path (Join-Path $Root ".git")) {
    Invoke-CheckedCommand git -C $Root remote set-url origin https://github.com/thun-res/osg_sdk.git
    git -C $Root fetch --quiet --depth 1 origin master *> $Log
    if ($LASTEXITCODE -ne 0) {
      Get-Content $Log -Tail 100
      throw "git fetch failed with exit code $LASTEXITCODE"
    }
    git -C $Root reset --quiet --hard FETCH_HEAD *> $Log
    if ($LASTEXITCODE -ne 0) {
      Get-Content $Log -Tail 100
      throw "git reset failed with exit code $LASTEXITCODE"
    }
  } else {
    if (Test-Path $Root) {
      Remove-Item -Recurse -Force $Root
    }
    $env:GIT_LFS_SKIP_SMUDGE = "1"
    try {
      git clone --quiet --depth 1 --branch master https://github.com/thun-res/osg_sdk.git $Root *> $Log
      if ($LASTEXITCODE -ne 0) {
        Get-Content $Log -Tail 100
        throw "git clone failed with exit code $LASTEXITCODE"
      }
    } finally {
      Remove-Item Env:GIT_LFS_SKIP_SMUDGE -ErrorAction SilentlyContinue
    }
  }
}

Invoke-GitHubProgressStep "Install Git LFS hooks" {
  git -C $Root lfs install --local *> $null
  if ($LASTEXITCODE -ne 0) {
    throw "git lfs install failed with exit code $LASTEXITCODE"
  }
}

Invoke-GitHubProgressStep "Pull $PlatformRoot SDK files" {
  git -C $Root lfs pull --include="$PlatformRoot/**" --exclude="" *> $Log
  if ($LASTEXITCODE -ne 0) {
    Get-Content $Log -Tail 100
    throw "git lfs pull failed with exit code $LASTEXITCODE"
  }
}

Invoke-GitHubProgressStep "Export OSG environment" {
  "OSG_DIR=$(Join-Path $Root $PlatformDir)" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
}
