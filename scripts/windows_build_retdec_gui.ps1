#Requires -Version 5.1
<#
.SYNOPSIS
  Build only retdec-gui in build\windows after windows_native_configure.ps1.

.NOTES
  Uses Enter-VsDevShell (not vcvars + cmd /c) so very long PATH values do not hit
  cmd.exe "The input line is too long" when setting up MSVC.
#>
param([int]$Jobs = 12)
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Repo "build\windows"
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath 2>$null
if (-not $vsPath) {
    Write-Error "Visual Studio C++ tools not found (vswhere)."
    exit 1
}
$DevShellDll = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
if (-not (Test-Path $DevShellDll)) {
    Write-Error "DevShell DLL not found: $DevShellDll"
    exit 1
}
Import-Module $DevShellDll
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"
Push-Location $BuildDir
cmake --build . --target retdec-gui --parallel $Jobs
$code = $LASTEXITCODE
Pop-Location
exit $code
