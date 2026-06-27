$script:GitHubProgressTitle = ""
$script:GitHubProgressSteps = @()
$script:GitHubProgressStatus = @()
$script:GitHubProgressGroupOpen = $false

function Test-GitHubProgressSummary {
  if (-not $env:GITHUB_STEP_SUMMARY) { return $false }
  $Parent = Split-Path -Parent $env:GITHUB_STEP_SUMMARY
  return (Test-Path $Parent)
}

function ConvertTo-GitHubProgressMarkdown {
  param([string]$Value)
  return $Value.Replace("\", "\\").Replace("|", "\|")
}

function Get-GitHubProgressBar {
  param([int]$Done, [int]$Total)
  $Width = 20
  $Filled = 0
  $Percent = 0
  if ($Total -gt 0) {
    $Filled = [int][Math]::Floor($Done * $Width / $Total)
    $Percent = [int][Math]::Floor($Done * 100 / $Total)
  }
  return ("[{0}{1}] {2}%" -f ("#" * $Filled), ("-" * ($Width - $Filled)), $Percent)
}

function Write-GitHubProgressSummary {
  param([string]$Current = "Idle")
  if (-not (Test-GitHubProgressSummary)) { return }

  $Done = @($script:GitHubProgressStatus | Where-Object { $_ -eq "Done" -or $_ -eq "Skipped" }).Count
  $Total = $script:GitHubProgressSteps.Count
  $Lines = @(
    "### $(ConvertTo-GitHubProgressMarkdown $script:GitHubProgressTitle)",
    "",
    "| Progress | Current |",
    "| --- | --- |",
    "| ``$(Get-GitHubProgressBar $Done $Total)`` | $(ConvertTo-GitHubProgressMarkdown $Current) |",
    "",
    "| Step | Status |",
    "| --- | --- |"
  )

  for ($I = 0; $I -lt $Total; $I++) {
    $Lines += "| $(ConvertTo-GitHubProgressMarkdown ($script:GitHubProgressSteps[$I])) | $(ConvertTo-GitHubProgressMarkdown ($script:GitHubProgressStatus[$I])) |"
  }

  $Lines | Set-Content -Path $env:GITHUB_STEP_SUMMARY -Encoding utf8
}

function Write-GitHubProgressLog {
  param([string]$Current, [string]$State)
  $Done = @($script:GitHubProgressStatus | Where-Object { $_ -eq "Done" -or $_ -eq "Skipped" }).Count
  $Total = $script:GitHubProgressSteps.Count
  Write-Host ("vlink-progress {0} {1}/{2} {3}: {4}" -f (Get-GitHubProgressBar $Done $Total), $Done, $Total, $State, $Current)
}

function Get-GitHubProgressStepIndex {
  param([string]$Name)
  for ($I = 0; $I -lt $script:GitHubProgressSteps.Count; $I++) {
    if ($script:GitHubProgressSteps[$I] -eq $Name) { return $I }
  }
  return -1
}

function Set-GitHubProgressStatus {
  param([string]$Name, [string]$Status)
  $Index = Get-GitHubProgressStepIndex $Name
  if ($Index -ge 0) {
    $script:GitHubProgressStatus[$Index] = $Status
  }
}

function Start-GitHubProgressGroup {
  param([string]$Name)
  if ($script:GitHubProgressGroupOpen) {
    Stop-GitHubProgressGroup
  }
  if ($env:GITHUB_ACTIONS) {
    Write-Host "::group::$Name"
  } else {
    Write-Host "==> $Name"
  }
  $script:GitHubProgressGroupOpen = $true
}

function Stop-GitHubProgressGroup {
  if ($script:GitHubProgressGroupOpen) {
    if ($env:GITHUB_ACTIONS) {
      Write-Host "::endgroup::"
    }
    $script:GitHubProgressGroupOpen = $false
  }
}

function Initialize-GitHubProgress {
  param([string]$Title, [string[]]$Steps)
  $script:GitHubProgressTitle = $Title
  $script:GitHubProgressSteps = @($Steps)
  $script:GitHubProgressStatus = @($Steps | ForEach-Object { "Pending" })
  Write-GitHubProgressSummary "Pending"
}

function Start-GitHubProgressStep {
  param([string]$Name)
  Set-GitHubProgressStatus $Name "Running"
  Write-GitHubProgressSummary $Name
  Write-GitHubProgressLog $Name "running"
  Start-GitHubProgressGroup $Name
}

function Complete-GitHubProgressStep {
  param([string]$Name)
  Stop-GitHubProgressGroup
  Set-GitHubProgressStatus $Name "Done"
  Write-GitHubProgressSummary $Name
  Write-GitHubProgressLog $Name "done"
}

function Skip-GitHubProgressStep {
  param([string]$Name)
  Stop-GitHubProgressGroup
  Set-GitHubProgressStatus $Name "Skipped"
  Write-GitHubProgressSummary $Name
  Write-GitHubProgressLog $Name "skipped"
}

function Fail-GitHubProgressStep {
  param([string]$Name)
  Stop-GitHubProgressGroup
  Set-GitHubProgressStatus $Name "Failed"
  Write-GitHubProgressSummary $Name
  Write-GitHubProgressLog $Name "failed"
}

function Invoke-GitHubProgressStep {
  param([string]$Name, [scriptblock]$ScriptBlock)
  Start-GitHubProgressStep $Name
  try {
    & $ScriptBlock
    Complete-GitHubProgressStep $Name
  } catch {
    Fail-GitHubProgressStep $Name
    throw
  }
}

function Invoke-CheckedCommand {
  param(
    [Parameter(Mandatory = $true, Position = 0)][string]$FilePath,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ArgumentList
  )
  & $FilePath @ArgumentList
  if ($LASTEXITCODE -ne 0) {
    throw "$FilePath failed with exit code $LASTEXITCODE"
  }
}
