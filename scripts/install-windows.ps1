#Requires -Version 5.1
<#
.SYNOPSIS
  Install RetDec on Windows from a built tree, portable bundle, or NSIS installer.

.DESCRIPTION
  User-facing helper that either:
  - Runs retdec-*-windows-x64-setup.exe when -SetupExe is provided, or
  - Copies a portable bundle / install tree into Program Files (default: C:\Program Files\RetDec), or
  - Prints instructions when no source is specified.

.PARAMETER SourceDir
  Portable bundle (dist\windows-bundle), cmake install tree (install\windows), or dist\windows staging dir.

.PARAMETER Destination
  Install root. Default: C:\Program Files\RetDec

.PARAMETER SetupExe
  Path to retdec-*-windows-x64-setup.exe (recommended for PATH + uninstaller).

.PARAMETER AddToPath
  Append Destination\bin to the current user's PATH (portable / copy install only).

.PARAMETER Force
  Overwrite an existing Destination directory.

.EXAMPLE
  .\scripts\install-windows.ps1 -SetupExe dist\retdec-5.0-windows-x64-setup.exe

.EXAMPLE
  .\scripts\install-windows.ps1 -SourceDir dist\windows-bundle -AddToPath

.EXAMPLE
  .\scripts\install-windows.ps1
#>

[CmdletBinding()]
param(
    [string]$SourceDir = "",
    [string]$Destination = "",
    [string]$SetupExe = "",
    [switch]$AddToPath,
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "retdec-paths.ps1")

$RepoRoot = Get-RetDecRepoRoot

function Resolve-RepoRelativePath {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return (Resolve-Path -LiteralPath $Path).Path
    }
    return (Resolve-Path -LiteralPath (Join-Path $RepoRoot $Path)).Path
}

function Find-DefaultSource {
    $candidates = @(
        (Join-Path $RepoRoot "dist\windows-bundle"),
        (Join-Path $RepoRoot "install\windows"),
        (Join-Path $RepoRoot "dist\windows")
    )
    foreach ($candidate in $candidates) {
        $bin = Join-Path $candidate "bin"
        if (-not (Test-Path -LiteralPath $bin)) {
            $bin = $candidate
        }
        $decompiler = Join-Path $bin "retdec-decompiler.exe"
        if (Test-Path -LiteralPath $decompiler) {
            return $candidate
        }
    }
    return $null
}

function Get-SourceLayout {
    param([string]$Root)
    if (Test-Path -LiteralPath (Join-Path $Root "bin\retdec-decompiler.exe")) {
        return @{
            Root = $Root
            Bin = Join-Path $Root "bin"
            Share = Join-Path $Root "share"
            Platforms = Join-Path $Root "bin\platforms"
            HasNestedBin = $true
        }
    }
    if (Test-Path -LiteralPath (Join-Path $Root "retdec-decompiler.exe")) {
        return @{
            Root = $Root
            Bin = $Root
            Share = Join-Path (Split-Path $Root -Parent) "share"
            Platforms = Join-Path $Root "platforms"
            HasNestedBin = $false
        }
    }
    throw "No retdec-decompiler.exe found under $Root"
}

function Install-RetDecCopy {
    param(
        [string]$SourceRoot,
        [string]$DestRoot,
        [switch]$AllowOverwrite
    )

    $layout = Get-SourceLayout -Root $SourceRoot
    if ((Test-Path -LiteralPath $DestRoot) -and -not $AllowOverwrite) {
        throw "Destination already exists: $DestRoot (use -Force to overwrite)."
    }

    Write-Host "==> Installing RetDec to $DestRoot"
    New-Item -ItemType Directory -Force -Path (Join-Path $DestRoot "bin") | Out-Null

    Get-ChildItem -LiteralPath $layout.Bin -File |
        Copy-Item -Destination (Join-Path $DestRoot "bin") -Force

    if ($layout.HasNestedBin) {
        foreach ($subdir in @("platforms", "imageformats", "styles", "iconengines", "tls", "translations")) {
            $src = Join-Path $layout.Bin $subdir
            if (Test-Path -LiteralPath $src) {
                Copy-Item -LiteralPath $src -Destination (Join-Path $DestRoot "bin\$subdir") -Recurse -Force
            }
        }
    } else {
        $topPlatforms = Join-Path (Split-Path $layout.Bin -Parent) "platforms"
        if (Test-Path -LiteralPath $topPlatforms) {
            Copy-Item -LiteralPath $topPlatforms -Destination (Join-Path $DestRoot "bin\platforms") -Recurse -Force
        }
    }

    $shareSrc = Join-Path $layout.Root "share"
    if (-not (Test-Path -LiteralPath $shareSrc)) {
        $shareSrc = Join-Path (Split-Path $layout.Root -Parent) "share"
    }
    if (Test-Path -LiteralPath $shareSrc) {
        Copy-Item -LiteralPath $shareSrc -Destination $DestRoot -Recurse -Force
    }

    Write-Host "Installed:"
    Write-Host "  $($DestRoot)\bin\retdec-decompiler.exe"
    if (Test-Path -LiteralPath (Join-Path $DestRoot "bin\retdec-gui.exe")) {
        Write-Host "  $($DestRoot)\bin\retdec-gui.exe"
    }
}

function Add-UserPathEntry {
    param([string]$Entry)
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $parts = @()
    if ($userPath) {
        $parts = $userPath -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    }
    if ($parts -contains $Entry) {
        Write-Host "PATH already contains: $Entry"
        return
    }
    $newPath = ($parts + $Entry) -join ';'
    [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
    Write-Host "Added to user PATH: $Entry (open a new terminal to use retdec-decompiler from anywhere)"
}

function Show-InstallInstructions {
    Write-Host ""
    Write-Host "RetDec Windows installation options"
    Write-Host "====================================="
    Write-Host ""
    Write-Host "Recommended - graphical installer (PATH, shortcuts, uninstaller):"
    Write-Host "  1. Build:  .\scripts\build-windows-installer.ps1"
    Write-Host "  2. Run:    .\scripts\install-windows.ps1 -SetupExe dist\retdec-<version>-windows-x64-setup.exe"
    Write-Host ""
    Write-Host "Portable - no admin required:"
    Write-Host "  1. Build:  .\scripts\build-windows-installer.ps1 -SkipBuild"
    Write-Host "  2. Extract dist\retdec-<version>-windows-x64-portable.zip anywhere, or:"
    Write-Host "     .\scripts\install-windows.ps1 -SourceDir dist\windows-bundle -Destination `"$env:LOCALAPPDATA\RetDec`" -AddToPath"
    Write-Host ""
    Write-Host "Manual copy from cmake install tree:"
    Write-Host "  .\scripts\install-windows.ps1 -SourceDir install\windows -Destination `"C:\Program Files\RetDec`" -Force"
    Write-Host ""
    Write-Host "Project: https://github.com/odin-loki/RetDec-Decompiler"
    Write-Host "Docs:    docs\INSTALL_WINDOWS.md"
}

if (-not [string]::IsNullOrWhiteSpace($SetupExe)) {
    $setupPath = Resolve-RepoRelativePath $SetupExe
    if (-not (Test-Path -LiteralPath $setupPath)) {
        throw "Setup executable not found: $setupPath"
    }
    Write-Host "==> Launching installer: $setupPath"
    Write-Host "    (UAC prompt expected - installs to Program Files and updates system PATH)"
    Start-Process -FilePath $setupPath -Wait
    exit 0
}

if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path ${env:ProgramFiles} "RetDec"
}

if ([string]::IsNullOrWhiteSpace($SourceDir)) {
    $SourceDir = Find-DefaultSource
    if (-not $SourceDir) {
        Show-InstallInstructions
        exit 0
    }
    Write-Host "Using detected source: $SourceDir"
} else {
    $SourceDir = Resolve-RepoRelativePath $SourceDir
}

if ($Force -and (Test-Path -LiteralPath $Destination)) {
    Remove-Item -LiteralPath $Destination -Recurse -Force
}

Install-RetDecCopy -SourceRoot $SourceDir -DestRoot $Destination -AllowOverwrite:$Force

if ($AddToPath) {
    Add-UserPathEntry -Entry (Join-Path $Destination "bin")
}

Write-Host ""
Write-Host "Run: `"$(Join-Path $Destination 'bin\retdec-decompiler.exe')`" --help"
