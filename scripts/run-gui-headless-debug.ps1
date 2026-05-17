#requires -Version 5.1
<#
.SYNOPSIS
  Run retdec-gui with Qt offscreen (no visible windows) for CI / automated debugging.

.DESCRIPTION
  Sets RETDEC_GUI_HEADLESS=1 (unless already set) and optional RETDEC_QWEN3_TRACE*.
  Loads Visual Studio Dev Shell when available so a build-tree or install-tree exe runs reliably.

  Requires platforms/qoffscreen.dll next to retdec-gui.exe on Windows (CMake POST_BUILD / install).

.EXAMPLE
  .\scripts\run-gui-headless-debug.ps1 -BuildPreset full-windows-release -ExitMs 8000

.EXAMPLE
  .\scripts\run-gui-headless-debug.ps1 -InstallBin "C:\retdec\install\full-windows-release\bin" -Trace -TraceVerbose
#>

[CmdletBinding()]
param(
    [string] $SourceDir = "",
    [string] $BuildPreset = "full-windows-release",
    [string] $GuiExe = "",
    [string] $InstallBin = "",
    [int] $ExitMs = 0,
    [switch] $Trace,
    [switch] $TraceVerbose,
    [string[]] $ExtraArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "retdec-paths.ps1")

if (-not $SourceDir) { $SourceDir = Get-RetDecRepoRoot }
$SourceDir = (Resolve-Path -LiteralPath $SourceDir).Path

if (-not $GuiExe) {
    $candidates = @()
    if ($InstallBin) {
        $candidates += (Join-Path $InstallBin "retdec-gui.exe")
    }
    $candidates += @(
        (Join-Path $SourceDir "build\$BuildPreset\src\gui\retdec-gui.exe"),
        (Join-Path $SourceDir "install\$BuildPreset\bin\retdec-gui.exe")
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { $GuiExe = $c; break }
    }
}

if (-not $GuiExe -or -not (Test-Path -LiteralPath $GuiExe)) {
    Write-Error "retdec-gui.exe not found. Build retdec-gui or pass -GuiExe / -InstallBin."
}

if (-not (Enter-RetDecVsDevShell)) {
    Write-Warning "VS Dev Shell not loaded; run from Developer PowerShell for VS if startup fails."
}

if ($Trace) { $env:RETDEC_QWEN3_TRACE = "1" } else { Remove-Item env:RETDEC_QWEN3_TRACE -ErrorAction SilentlyContinue }
if ($TraceVerbose) { $env:RETDEC_QWEN3_TRACE_VERBOSE = "1" } else { Remove-Item env:RETDEC_QWEN3_TRACE_VERBOSE -ErrorAction SilentlyContinue }

if (-not $env:RETDEC_GUI_HEADLESS) { $env:RETDEC_GUI_HEADLESS = "1" }

$run = @("--headless")
if ($ExitMs -gt 0) {
    $run += @("--headless-exit-ms", "$ExitMs")
}
$run += $ExtraArgs

Write-Host "==> & `"$GuiExe`" $($run -join ' ')"
& $GuiExe @run
exit $LASTEXITCODE
