#Requires -Version 5.1
<#
.SYNOPSIS
  Run locally automatable MASTER-UPGRADE-PLAN checks on Windows.

.EXAMPLE
  .\scripts\run_all_automation.ps1
  .\scripts\run_all_automation.ps1 -SkipMigration
#>
[CmdletBinding()]
param(
    [switch]$SkipMigration,
    [switch]$SkipSupportRegen
)

$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent $PSScriptRoot
$Bash = "C:\Program Files\Git\bin\bash.exe"

if (-not (Test-Path -LiteralPath $Bash)) {
    Write-Error "Git Bash not found at $Bash"
    exit 1
}

$args = @("scripts/run_all_automation.sh")
if ($SkipMigration) { $args += "--skip-migration" }
if ($SkipSupportRegen) { $args += "--skip-support-regen" }

Push-Location $Root
try {
    & $Bash @args
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
