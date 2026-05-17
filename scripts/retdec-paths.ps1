# Dot-source for shared RetDec repo / build paths (Windows PowerShell 5.1+ / PowerShell 7+).
# Usage: . (Join-Path $PSScriptRoot "retdec-paths.ps1")

$script:RetDecRepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Get-RetDecRepoRoot { $script:RetDecRepoRoot }

function Get-RetDecBuildDir {
    param([string]$Preset = $(if ($env:RETDEC_CMAKE_PRESET) { $env:RETDEC_CMAKE_PRESET } else { "full-windows-release" }))
    # Preset name kept for API compatibility; CMakePresets use a single build\windows tree on Windows.
    Join-Path $script:RetDecRepoRoot "build\windows"
}

function Get-RetDecInstallDir {
    param([string]$Preset = $(if ($env:RETDEC_CMAKE_PRESET) { $env:RETDEC_CMAKE_PRESET } else { "full-windows-release" }))
    Join-Path $script:RetDecRepoRoot "install\windows"
}

function Enter-RetDecVsDevShell {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) { return $false }
    $inst = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if (-not $inst) { $inst = & $vswhere -latest -products * -property installationPath 2>$null }
    if (-not $inst) { return $false }
    $dll = Join-Path $inst "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    if (-not (Test-Path -LiteralPath $dll)) { return $false }
    Import-Module $dll
    Enter-VsDevShell -VsInstallPath $inst -SkipAutomaticLocation -DevCmdArguments "-arch=x64"
    return $true
}
