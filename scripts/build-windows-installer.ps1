#Requires -Version 5.1
<#
.SYNOPSIS
  Build, stage, zip, and optionally compile an NSIS installer for RetDec on Windows.

.DESCRIPTION
  1. Optionally builds release targets (retdec-decompiler, retdec-gui, retdec-fileinfo).
  2. Runs cmake --install into install/windows (or -InstallDir).
  3. Stages a portable bundle (dist/windows-bundle by default) with binaries, share/retdec,
     Qt6 DLLs, platform plugins, and MSVC runtime DLLs when available.
  4. Always produces retdec-{version}-windows-x64-portable.zip.
  5. If makensis is on PATH, produces retdec-{version}-windows-x64-setup.exe via packaging/nsis/retdec.nsi.

.PARAMETER BuildDir
  CMake build tree. Default: build\windows

.PARAMETER InstallDir
  CMake install prefix. Default: install\windows

.PARAMETER OutDir
  Directory for bundle, zip, and NSIS output. Default: dist

.PARAMETER BundleDir
  Staging directory for NSIS BUNDLE_DIR layout. Default: dist\windows-bundle

.PARAMETER SkipBuild
  Skip cmake --build; assume InstallDir is already populated.

.PARAMETER Version
  Package version string (passed to NSIS /DVERSION). Default: read from root CMakeLists.txt.

.PARAMETER QtRoot
  Qt kit root for supplemental DLL copy (e.g. C:\Qt\6.11.0\msvc2022_64). Auto-detected when omitted.

.EXAMPLE
  .\scripts\build-windows-installer.ps1 -SkipBuild

.EXAMPLE
  .\scripts\build-windows-installer.ps1 -Version 5.0.0
#>

[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$InstallDir = "",
    [string]$OutDir = "",
    [string]$BundleDir = "",
    [switch]$SkipBuild,
    [string]$Version = "",
    [string]$QtRoot = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "retdec-paths.ps1")

$RepoRoot = Get-RetDecRepoRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $RepoRoot "build\windows"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $RepoRoot $BuildDir
}
if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    $InstallDir = Join-Path $RepoRoot "install\windows"
} elseif (-not [System.IO.Path]::IsPathRooted($InstallDir)) {
    $InstallDir = Join-Path $RepoRoot $InstallDir
}
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $RepoRoot "dist"
} elseif (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $RepoRoot $OutDir
}
if ([string]::IsNullOrWhiteSpace($BundleDir)) {
    $BundleDir = Join-Path $OutDir "windows-bundle"
} elseif (-not [System.IO.Path]::IsPathRooted($BundleDir)) {
    $BundleDir = Join-Path $RepoRoot $BundleDir
}

function Get-RetDecPackageVersion {
    param([string]$Explicit)
    if (-not [string]::IsNullOrWhiteSpace($Explicit)) {
        return $Explicit.Trim()
    }
    $cmakeLists = Join-Path $RepoRoot "CMakeLists.txt"
    if (Test-Path -LiteralPath $cmakeLists) {
        $content = Get-Content -LiteralPath $cmakeLists -Raw
        if ($content -match 'project\s*\([^)]*?\bVERSION\s+([0-9]+(?:\.[0-9]+)*)') {
            return $Matches[1]
        }
    }
    return "0.0.0"
}

function Resolve-QtKitRoot {
    param([string]$Explicit)
    if (-not [string]::IsNullOrWhiteSpace($Explicit) -and (Test-Path -LiteralPath $Explicit)) {
        return (Resolve-Path -LiteralPath $Explicit).Path
    }
    $qt6Dir = $env:Qt6_DIR
    if ($qt6Dir -and (Test-Path -LiteralPath $qt6Dir)) {
        $candidate = Split-Path (Split-Path (Split-Path $qt6Dir -Parent) -Parent) -Parent
        if (Test-Path (Join-Path $candidate "bin\Qt6Core.dll")) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    foreach ($root in @("C:\Qt\6.11.0\msvc2022_64", "C:\Qt", "$env:USERPROFILE\Qt")) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        if (Test-Path (Join-Path $root "bin\Qt6Core.dll")) {
            return (Resolve-Path -LiteralPath $root).Path
        }
        $found = Get-ChildItem "$root\*\msvc*_64" -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName "bin\Qt6Core.dll") } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    return $null
}

function Reset-Directory {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

function Copy-IfExists {
    param(
        [string]$Source,
        [string]$DestinationDir,
        [switch]$Recurse
    )
    if (-not (Test-Path -LiteralPath $Source)) { return $false }
    $destParent = Split-Path $DestinationDir -Parent
    if ($destParent -and -not (Test-Path -LiteralPath $destParent)) {
        New-Item -ItemType Directory -Force -Path $destParent | Out-Null
    }
    if ($Recurse) {
        Copy-Item -LiteralPath $Source -Destination $DestinationDir -Recurse -Force
    } else {
        Copy-Item -LiteralPath $Source -Destination $DestinationDir -Force
    }
    return $true
}

function Copy-MsvcRuntime {
    param([string]$DestinationBin)
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vsWhere)) { return 0 }
    $vsPath = & $vsWhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if (-not $vsPath) { return 0 }
    $redistDir = Get-ChildItem "$vsPath\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $redistDir) { return 0 }
    $copied = 0
    foreach ($dll in @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140_1.dll", "concrt140.dll")) {
        $src = Join-Path $redistDir $dll
        if (Test-Path -LiteralPath $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $DestinationBin $dll) -Force
            $copied++
        }
    }
    return $copied
}

function Copy-QtSupplement {
    param(
        [string]$QtKit,
        [string]$BundleBin,
        [string]$BundlePlatforms,
        [string]$BundleImageFormats
    )
    if (-not $QtKit) { return }
    $qtBin = Join-Path $QtKit "bin"
    foreach ($dll in @(
        "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
        "Qt6Network.dll", "Qt6OpenGL.dll", "Qt6Charts.dll", "Qt6Svg.dll"
    )) {
        $src = Join-Path $qtBin $dll
        if ((Test-Path -LiteralPath $src) -and -not (Test-Path (Join-Path $BundleBin $dll))) {
            Copy-Item -LiteralPath $src -Destination $BundleBin -Force
        }
    }
    $qtPlugins = Join-Path $QtKit "plugins"
    foreach ($plug in @("qwindows.dll", "qoffscreen.dll")) {
        $src = Join-Path $qtPlugins "platforms\$plug"
        if ((Test-Path -LiteralPath $src) -and -not (Test-Path (Join-Path $BundlePlatforms $plug))) {
            Copy-Item -LiteralPath $src -Destination $BundlePlatforms -Force
        }
    }
    $imgSrc = Join-Path $qtPlugins "imageformats"
    if (Test-Path -LiteralPath $imgSrc) {
        Get-ChildItem -LiteralPath $imgSrc -Filter "*.dll" -ErrorAction SilentlyContinue |
            ForEach-Object {
                $dest = Join-Path $BundleImageFormats $_.Name
                if (-not (Test-Path -LiteralPath $dest)) {
                    Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
                }
            }
    }
}

function Invoke-RetDecWindowsBundle {
    param(
        [string]$InstallRoot,
        [string]$StageRoot,
        [string]$QtKit
    )

    $installBin = Join-Path $InstallRoot "bin"
    if (-not (Test-Path -LiteralPath $installBin)) {
        throw "Install bin directory missing: $installBin (run build/install first or omit -SkipBuild)."
    }

    $bundleBin = Join-Path $StageRoot "bin"
    $bundlePlatforms = Join-Path $StageRoot "platforms"
    $bundleImageFormats = Join-Path $StageRoot "imageformats"
    $bundleShare = Join-Path $StageRoot "share\retdec"

    Reset-Directory $StageRoot
    New-Item -ItemType Directory -Force -Path $bundleBin, $bundlePlatforms, $bundleImageFormats, $bundleShare | Out-Null

    Write-Host "==> Staging RetDec binaries from $installBin"
    Get-ChildItem -LiteralPath $installBin -Filter "*.exe" -File |
        Copy-Item -Destination $bundleBin -Force

    Get-ChildItem -LiteralPath $installBin -Filter "*.dll" -File -ErrorAction SilentlyContinue |
        Copy-Item -Destination $bundleBin -Force

    $installShare = Join-Path $InstallRoot "share\retdec"
    if (Test-Path -LiteralPath $installShare) {
        Copy-Item -LiteralPath $installShare -Destination (Split-Path $bundleShare -Parent) -Recurse -Force
    } else {
        Write-Warning "share/retdec not found under $InstallRoot; NSIS/core tools may be incomplete."
    }

    $installPlatforms = Join-Path $installBin "platforms"
    if (Test-Path -LiteralPath $installPlatforms) {
        Get-ChildItem -LiteralPath $installPlatforms -Filter "*.dll" -File |
            Copy-Item -Destination $bundlePlatforms -Force
    }

    $installImageFormats = Join-Path $installBin "imageformats"
    if (Test-Path -LiteralPath $installImageFormats) {
        Get-ChildItem -LiteralPath $installImageFormats -Filter "*.dll" -File |
            Copy-Item -Destination $bundleImageFormats -Force
    }

    Copy-QtSupplement -QtKit $QtKit -BundleBin $bundleBin -BundlePlatforms $bundlePlatforms -BundleImageFormats $bundleImageFormats

    $msvcCopied = Copy-MsvcRuntime -DestinationBin $bundleBin
    if ($msvcCopied -gt 0) {
        Write-Host "  [OK] Copied $msvcCopied MSVC runtime DLL(s)."
    } else {
        Write-Warning "MSVC runtime DLLs not found; target machines need VC++ Redistributable."
    }

    Write-Host "==> Bundle staged at $StageRoot"
}

function New-PortableZip {
    param(
        [string]$BundleRoot,
        [string]$ZipPath
    )
    if (Test-Path -LiteralPath $ZipPath) {
        Remove-Item -LiteralPath $ZipPath -Force
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($BundleRoot, $ZipPath)
    Write-Host "==> Portable ZIP: $ZipPath"
}

function Invoke-NsisInstaller {
    param(
        [string]$PackageVersion,
        [string]$StageRoot,
        [string]$OutputDir
    )
    $makensis = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if (-not $makensis) {
        $makensis = Get-Command makensis -ErrorAction SilentlyContinue
    }
    if (-not $makensis) {
        Write-Host ""
        Write-Host "NSIS (makensis) not found on PATH - skipping setup.exe." -ForegroundColor Yellow
        $pfNSIS = Join-Path ${env:ProgramFiles(x86)} "NSIS"
        Write-Host ""
        Write-Host "To build the graphical installer:"
        Write-Host "  1. Install NSIS 3.x from https://nsis.sourceforge.io/Download"
        Write-Host "  2. Install the EnVar plug-in (required for PATH updates):"
        Write-Host "       https://nsis.sourceforge.io/EnVar_plug-in"
        Write-Host "     Copy EnVar.dll into:  $pfNSIS\Plugins\x86-unicode\"
        Write-Host "     Copy EnVar.nsh into:  $pfNSIS\Include\"
        Write-Host "  3. Re-run this script, or compile manually:"
        Write-Host "       makensis /DVERSION=$PackageVersion /DBUNDLE_DIR=`"$StageRoot`" packaging\nsis\retdec.nsi"
        Write-Host ""
        Write-Host "Portable ZIP is ready at: $OutputDir\retdec-$PackageVersion-windows-x64-portable.zip"
        return $null
    }

    $nsi = Join-Path $RepoRoot "packaging\nsis\retdec.nsi"
    if (-not (Test-Path -LiteralPath $nsi)) {
        throw "NSIS script not found: $nsi"
    }

    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
    Push-Location (Split-Path $nsi -Parent)
    try {
        Write-Host "==> Running makensis ($($makensis.Source))"
        & $makensis.Source "/DVERSION=$PackageVersion" "/DBUNDLE_DIR=$StageRoot" (Split-Path $nsi -Leaf)
        if ($LASTEXITCODE -ne 0) {
            throw "makensis failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }

    $setupExe = Join-Path (Split-Path $nsi -Parent) "retdec-$PackageVersion-windows-x64-setup.exe"
    if (-not (Test-Path -LiteralPath $setupExe)) {
        throw "Expected NSIS output not found: $setupExe"
    }

    $destSetup = Join-Path $OutputDir "retdec-$PackageVersion-windows-x64-setup.exe"
    Move-Item -LiteralPath $setupExe -Destination $destSetup -Force
    Write-Host "==> NSIS installer: $destSetup"
    return $destSetup
}

$PackageVersion = Get-RetDecPackageVersion -Explicit $Version
$QtKitRoot = Resolve-QtKitRoot -Explicit $QtRoot

Write-Host "=== RetDec Windows Installer Workflow ==="
Write-Host "  version:    $PackageVersion"
Write-Host "  build dir:  $BuildDir"
Write-Host "  install:    $InstallDir"
Write-Host "  bundle:     $BundleDir"
Write-Host "  output:     $OutDir"
$qtRootLabel = if ($QtKitRoot) { $QtKitRoot } else { "(not detected)" }
Write-Host "  qt root:    $qtRootLabel"

if (-not $SkipBuild) {
    if (-not (Enter-RetDecVsDevShell)) {
        Write-Warning "VS Dev Shell not loaded; cmake --build may fail without Developer PowerShell."
    }

    if (-not (Test-Path -LiteralPath (Join-Path $BuildDir "CMakeCache.txt"))) {
        throw "No CMake cache at $BuildDir - configure first (cmake --preset full-windows-release)."
    }

    foreach ($target in @("retdec-decompiler", "retdec-gui", "retdec-fileinfo")) {
        Write-Host "==> cmake --build --target $target"
        & cmake --build $BuildDir --target $target --parallel
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Target '$target' was not built (may be absent in this configuration)."
        }
    }

    Write-Host "==> cmake --install $BuildDir"
    & cmake --install $BuildDir
    if ($LASTEXITCODE -ne 0) {
        throw "cmake --install failed with exit code $LASTEXITCODE"
    }
} else {
    if (-not (Test-Path -LiteralPath $InstallDir)) {
        throw "-SkipBuild specified but install directory does not exist: $InstallDir"
    }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Invoke-RetDecWindowsBundle -InstallRoot $InstallDir -StageRoot $BundleDir -QtKit $QtKitRoot

$zipPath = Join-Path $OutDir "retdec-$PackageVersion-windows-x64-portable.zip"
New-PortableZip -BundleRoot $BundleDir -ZipPath $zipPath

$setupPath = Invoke-NsisInstaller -PackageVersion $PackageVersion -StageRoot $BundleDir -OutputDir $OutDir

Write-Host ""
Write-Host "=== Done ==="
Write-Host "  bundle:  $BundleDir"
Write-Host "  zip:     $zipPath"
if ($setupPath) {
    Write-Host "  setup:   $setupPath"
}
