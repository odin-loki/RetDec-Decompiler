#Requires -Version 5.1
<#
.SYNOPSIS
  Configure and build RetDec superbuild for native Windows (MSVC): Debug and Release.

.DESCRIPTION
  Uses cmake/superbuild presets. Binary directories:
    build/windows/superbuild-debug
    build/windows/superbuild-release
  Install prefixes (preset defaults):
    install/windows/superbuild-debug
    install/windows/superbuild-release

  Run from Developer PowerShell for Visual Studio (or ensure cl.exe is on PATH).

.PARAMETER Install
  After each build, run cmake --install for that tree.

.PARAMETER Presets
  Which configure presets to build (default: superbuild-debug, superbuild-release).
#>
param(
    [switch] $Install,
    [string[]] $Presets = @('superbuild-debug', 'superbuild-release')
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$SuperSrc = Join-Path $RepoRoot 'cmake\superbuild'

if (-not (Test-Path -LiteralPath $SuperSrc)) {
    Write-Error "Superbuild source not found: $SuperSrc"
}

$jobs = $env:NUMBER_OF_PROCESSORS
if (-not $jobs -or $jobs -lt 1) { $jobs = 4 }

Write-Host "Repository: $RepoRoot"
Write-Host "Superbuild: $SuperSrc"
Write-Host "Presets:    $($Presets -join ', ')"
Write-Host "Jobs:       $jobs"
Write-Host ""

foreach ($p in $Presets) {
    Write-Host "=== cmake --preset $p (configure) ===" -ForegroundColor Cyan
    cmake -S $SuperSrc --preset $p
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $buildDir = Join-Path $RepoRoot "build\windows\$p"
    Write-Host "=== cmake --build $buildDir ===" -ForegroundColor Cyan
    cmake --build $buildDir --parallel $jobs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    if ($Install) {
        Write-Host "=== cmake --install $buildDir ===" -ForegroundColor Cyan
        cmake --install $buildDir
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}

Write-Host ""
Write-Host "Done. Superbuild trees under build\windows\<preset>\" -ForegroundColor Green
