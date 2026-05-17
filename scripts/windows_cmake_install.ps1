#Requires -Version 5.1
<#
.SYNOPSIS
  Run cmake --install on build\windows (needs MSVC env for some generators; safe to run after full build).
#>
param([string]$BuildDir = "")
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $Repo "build\windows"
}
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath 2>$null
if (-not $vsPath) { Write-Error "vswhere: no MSVC."; exit 1 }
$DevShellDll = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $DevShellDll
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"
cmake --install $BuildDir
exit $LASTEXITCODE
