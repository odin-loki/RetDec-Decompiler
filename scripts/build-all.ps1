#requires -Version 5.1
<#
.SYNOPSIS
  End-to-end Windows build: configure, compile, install, package installers.

.DESCRIPTION
  1. Optionally fetches large support files (first-time checkout).
  2. Runs cmake --preset (default full-windows-release).
  3. Builds, installs, and runs build-windows-installer.ps1 to refresh:
       dist\retdec-*-windows-x64-portable.zip
       dist\retdec-*-windows-x64-setup.exe  (when NSIS is installed)

.EXAMPLE
  .\scripts\build-all.ps1

.EXAMPLE
  .\scripts\build-all.ps1 -SkipConfigure -SkipFetch
#>
[CmdletBinding()]
param(
    [string]$Preset = "full-windows-release",
    [string]$Version = "",
    [switch]$SkipFetch,
    [switch]$SkipConfigure,
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "retdec-paths.ps1")
$repo = Get-RetDecRepoRoot

if (-not $SkipFetch) {
    $fetch = Join-Path $PSScriptRoot "fetch-large-files.ps1"
    if (Test-Path -LiteralPath $fetch) {
        Write-Host "==> Fetching large support files"
        & $fetch
    }
}

if (-not (Enter-RetDecVsDevShell)) {
    Write-Warning "VS Dev Shell not loaded; build may fail without Developer PowerShell."
}

Push-Location $repo
try {
    if (-not $SkipConfigure) {
        Write-Host "==> cmake --preset $Preset"
        & cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw "cmake --preset failed: $LASTEXITCODE" }
    }

    $installerArgs = @{
        SkipConfigure = $true
        SkipRun       = $true
        Preset        = $Preset
        PackageInstallers = $true
    }
    if ($SkipBuild) { $installerArgs.SkipBuild = $true }

    & (Join-Path $PSScriptRoot "build-install-run-windows.ps1") @installerArgs
    if ($LASTEXITCODE -ne 0) { throw "build-install-run-windows.ps1 failed: $LASTEXITCODE" }

    if ($Version) {
        Write-Host "==> Re-packaging with explicit version $Version"
        & (Join-Path $PSScriptRoot "build-windows-installer.ps1") -SkipBuild -Version $Version
    }
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "=== build-all.ps1 complete ==="
Write-Host "  installers: $repo\releases\windows"
