#Requires -Version 5.1
<#
.SYNOPSIS
  Trigger algorithm-recovery-nightly GitHub workflow (requires gh auth on Windows).

  This is the only supported path for GitHub CLI — do not use WSL gh in parallel.
  See docs/internal/MAINTAINER_SCOPE.md

.EXAMPLE
  .\scripts\dispatch_algorithm_recovery_nightly.ps1
  .\scripts\dispatch_algorithm_recovery_nightly.ps1 -FullCorpus
#>
[CmdletBinding()]
param([switch]$FullCorpus)

$gh = Get-Command gh -ErrorAction SilentlyContinue
if (-not $gh) {
    Write-Error "gh CLI not found — install from https://cli.github.com/"
    exit 1
}

& gh auth status 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Error "Not authenticated — run: gh auth login"
    exit 1
}

if ($FullCorpus) {
    Write-Host "Dispatching algorithm-recovery-nightly (full 216-binary corpus)..."
    & gh workflow run algorithm-recovery-nightly -f "full_corpus=true"
} else {
    Write-Host "Dispatching algorithm-recovery-nightly (CI core subset)..."
    & gh workflow run algorithm-recovery-nightly
}

Write-Host "Monitor with: gh run list --workflow=algorithm-recovery-nightly"
