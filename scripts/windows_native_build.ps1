#Requires -Version 5.1
<#
.SYNOPSIS
    Build RetDec natively on Windows (MSVC; GUI on FuseUI; CUDA toolkit optional) and stage
    a self-contained deployment in dist\windows\.

.DESCRIPTION
    1. Runs cmake --build on build\windows\ (from windows_native_configure.ps1 or cmake --preset).
    2. Runs cmake --install into install\windows\ (CMake preset installDir).
    3. Copies the MSVC runtime DLLs and share\retdec (including the GUI's share\retdec\gui
       resources) into dist\windows\ so the result is portable. The GUI has no toolkit DLLs.
       CUDA runtime DLLs are copied if a toolkit is installed; cuda_accel stays OFF.

.PARAMETER Preset
    Same preset as configure (default: full-windows-release). Build: build\windows\, install: install\windows\.

.PARAMETER DistDir
    Output staging directory. Default: dist\windows (last build wins if you switch Debug/Release).

.PARAMETER Jobs
    Parallel build jobs passed to cmake --build --parallel. Default: 32.

.PARAMETER SkipBuild
    Skip the cmake --build step (only re-run staging).

.PARAMETER PackageInstallers
    After staging, run scripts\build-windows-installer.ps1 -SkipBuild to produce
    dist\*.zip, setup.exe, and refresh releases\windows\ for git/CI publish.

.PARAMETER BuildDir
    CMake binary directory (must match configure). Default: build\windows under the repo.

.PARAMETER InstallPrefix
    Install root used by cmake --install (must match CMAKE_INSTALL_PREFIX from configure). Default: install\windows.

.EXAMPLE
    # From Developer PowerShell for VS 2022:
    .\scripts\windows_native_build.ps1

.EXAMPLE
    .\scripts\windows_native_build.ps1 -Preset full-windows-release -Jobs 32
#>

param(
    [string]$Preset    = "full-windows-release",
    [string]$DistDir   = "",
    [string]$BuildDir  = "",
    [string]$InstallPrefix = "",
    [int]   $Jobs      = 32,
    [switch]$SkipBuild,
    [switch]$PackageInstallers
)

$ErrorActionPreference = "Continue"

if ([string]::IsNullOrWhiteSpace($DistDir)) {
    $DistDir = "dist\windows"
}

$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDirFull = Join-Path $RepoRoot "build\windows"
} else {
    $BuildDirFull = $BuildDir
}
$DistDirFull = Join-Path $RepoRoot $DistDir
if ([string]::IsNullOrWhiteSpace($InstallPrefix)) {
    $InstallDir = Join-Path $RepoRoot "install\windows"
} else {
    $InstallDir = $InstallPrefix
}

Push-Location $RepoRoot

# ── MSVC + Ninja on PATH (cmake --build needs the same env as configure) ───
# (Must be outside try { }: nested function definitions break the PS parser.)
function Import-VsEnv([string]$vcvarsPath) {
    $cmdExe = Join-Path $env:SystemRoot "System32\cmd.exe"
    $cmdLine = "`"$vcvarsPath`" && set"
    $output = & $cmdExe /c $cmdLine 2>&1
    foreach ($line in $output) {
        if ($line -match "^([^=]+)=(.*)$") {
            $name  = $Matches[1].Trim()
            $value = $Matches[2].Trim()
            Set-Item -Path "env:$name" -Value $value -ErrorAction SilentlyContinue
        }
    }
}

try {
# Always load vcvars when available: cl.exe may already be on PATH (incomplete env)
# while Windows SDK tools (rc.exe, mt.exe) are not — nested ExternalProject CMake runs
# need the full developer layout.
$vsWhereDev = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vsWhereDev) {
    $vsPathDev = & $vsWhereDev -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if ($vsPathDev) {
        $vcvarsPath = Join-Path $vsPathDev "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $vcvarsPath) { Import-VsEnv $vcvarsPath }
    }
}
if (-not (Get-Command "ninja" -ErrorAction SilentlyContinue)) {
    $vsWhere2 = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere2) {
        $vsPath2 = & $vsWhere2 -latest -products * -property installationPath 2>$null
        $vsNinja = Join-Path $vsPath2 "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
        if (Test-Path $vsNinja) {
            $env:PATH = (Split-Path $vsNinja) + ";" + $env:PATH
        }
    }
}

# CUDA_PATH for runtime DLL copy (fresh shells do not inherit configure.ps1 env)
if (-not $env:CUDA_PATH) {
    $regKey = Get-ChildItem "HKLM:\SOFTWARE\NVIDIA Corporation\GPU Computing Toolkit\CUDA" -ErrorAction SilentlyContinue |
              Sort-Object Name -Descending | Select-Object -First 1
    if ($regKey) { $env:CUDA_PATH = $regKey.GetValue("InstallDir") }
}
if (-not $env:CUDA_PATH -or -not (Test-Path "$env:CUDA_PATH\bin\nvcc.exe")) {
    $nvccSearch = Get-ChildItem "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA" -Filter "nvcc.exe" -Recurse -ErrorAction SilentlyContinue |
                  Select-Object -First 1
    if ($nvccSearch) { $env:CUDA_PATH = Split-Path (Split-Path $nvccSearch.FullName) }
}

# ── Validate build dir ────────────────────────────────────────────────────
if (-not (Test-Path (Join-Path $BuildDirFull "CMakeCache.txt"))) {
    Write-Error "Build directory '$BuildDirFull' has no CMakeCache.txt. Run windows_native_configure.ps1 or cmake --preset $Preset."
    exit 1
}

Write-Host ""
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host "  RetDec Windows Native Build                          " -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host "  Build dir : $BuildDirFull" -ForegroundColor White
Write-Host "  Dist dir  : $DistDirFull" -ForegroundColor White
Write-Host "  Jobs      : $Jobs" -ForegroundColor White
Write-Host ""

# ── cmake --build ─────────────────────────────────────────────────────────
if (-not $SkipBuild) {
    Write-Host "--- Building (cmake --build) ---" -ForegroundColor White
    cmake --build $BuildDirFull --parallel $Jobs
    if ($LASTEXITCODE -ne 0) {
        Write-Error "cmake --build FAILED (exit code $LASTEXITCODE)."
        exit $LASTEXITCODE
    }
    Write-Host "[OK] Build succeeded." -ForegroundColor Green
    Write-Host ""
}

# ── cmake --install ───────────────────────────────────────────────────────
Write-Host "--- Installing (cmake --install -> $InstallDir) ---" -ForegroundColor White
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
cmake --install $BuildDirFull
if ($LASTEXITCODE -ne 0) {
    Write-Warning "cmake --install returned $LASTEXITCODE - falling back to manual copy."
    # Direct copy fallback: grab all .exe files from the build tree
    Get-ChildItem -Recurse -Path $BuildDirFull -Filter "*.exe" |
        Where-Object { $_.DirectoryName -match "\\src\\" -or $_.DirectoryName -match "/src/" } |
        ForEach-Object {
            Write-Host "  Copying $($_.Name) ..." -ForegroundColor Gray
            Copy-Item $_.FullName (Join-Path $DistDirFull $_.Name) -Force
        }
}

# ── Copy main executables to DistDir root ─────────────────────────────────
Write-Host ""
Write-Host "--- Staging binaries to $DistDirFull ---" -ForegroundColor White
New-Item -ItemType Directory -Force -Path $DistDirFull | Out-Null

$exesToCopy = @(
    "retdec-decompiler.exe",
    "retdec-unpacker.exe",
    "retdec-gui.exe",
    "retdec-fileinfo.exe"
)

foreach ($exe in $exesToCopy) {
    # Check install dir first, then build tree
    $src = Join-Path $InstallDir "bin\$exe"
    if (-not (Test-Path $src)) {
        $src = Get-ChildItem -Recurse -Path $BuildDirFull -Filter $exe -ErrorAction SilentlyContinue |
               Select-Object -First 1 -ExpandProperty FullName
    }
    if ($src -and (Test-Path $src)) {
        Copy-Item $src (Join-Path $DistDirFull $exe) -Force
        Write-Host "  [OK] $exe" -ForegroundColor Green
    } else {
        Write-Host "  [--] $exe (not built)" -ForegroundColor Gray
    }
}

# ── Copy support data (signatures, yara rules, decompiler-config, docs) ───
$shareRetdec = Join-Path $InstallDir "share\retdec"
$destShare   = Join-Path $DistDirFull "share\retdec"
if (Test-Path $shareRetdec) {
    New-Item -ItemType Directory -Force -Path $destShare | Out-Null
    Copy-Item "$shareRetdec\*" $destShare -Recurse -Force
    Write-Host "  [OK] share/retdec data (from install)" -ForegroundColor Green
} else {
    Write-Warning "install/share/retdec missing - copying decompiler-config and support from repo."
    New-Item -ItemType Directory -Force -Path $destShare | Out-Null
    $cfgSrc = Join-Path $RepoRoot "src\retdec-decompiler\decompiler-config.json"
    if (Test-Path $cfgSrc) {
        Copy-Item $cfgSrc (Join-Path $destShare "decompiler-config.json") -Force
        Write-Host "  [OK] decompiler-config.json (repo fallback)" -ForegroundColor Green
    }
    $supportSrc = Join-Path $RepoRoot "support"
    if (Test-Path $supportSrc) {
        $supportDest = Join-Path $destShare "support"
        Copy-Item $supportSrc $supportDest -Recurse -Force
        Write-Host "  [OK] support/ (repo fallback)" -ForegroundColor Green
    }
    $docSrc = Join-Path $RepoRoot "docs\WINDOWS_NATIVE_BUILD.md"
    if (Test-Path $docSrc) {
        $docDir = Join-Path $destShare "doc"
        New-Item -ItemType Directory -Force -Path $docDir | Out-Null
        Copy-Item $docSrc (Join-Path $docDir "WINDOWS_NATIVE_BUILD.md") -Force
        Write-Host "  [OK] doc/WINDOWS_NATIVE_BUILD.md (repo fallback)" -ForegroundColor Green
    }
}

# ── retdec-gui resources ──────────────────────────────────────────────────
# retdec-gui is FuseUI drawing into a Win32 window (user32/gdi32 only): there
# are no toolkit DLLs or platform plugins to deploy. What it does need is its
# resource tree (FuseUI themes, fonts, LLVM pass presets), installed to
# share\retdec\gui and copied above with the rest of share\retdec.
$guiExe = Join-Path $DistDirFull "retdec-gui.exe"
$guiRes = Join-Path $destShare "gui"
if ((Test-Path $guiExe) -and -not (Test-Path $guiRes)) {
    $guiResSrc = Join-Path $RepoRoot "src\gui\resources"
    if (Test-Path $guiResSrc) {
        New-Item -ItemType Directory -Force -Path $guiRes | Out-Null
        Copy-Item "$guiResSrc\*" $guiRes -Recurse -Force
        Write-Host "  [OK] share/retdec/gui (repo fallback)" -ForegroundColor Green
    } else {
        Write-Warning "retdec-gui.exe staged without share/retdec/gui resources."
    }
}

# ── CUDA runtime DLLs ─────────────────────────────────────────────────────
Write-Host ""
Write-Host "--- CUDA runtime DLLs ---" -ForegroundColor White

$cudaPath = $env:CUDA_PATH
if (-not $cudaPath) {
    $cudaPath = Get-ChildItem "HKLM:\SOFTWARE\NVIDIA Corporation\GPU Computing Toolkit\CUDA" -ErrorAction SilentlyContinue |
        Sort-Object Name | Select-Object -Last 1 |
        ForEach-Object { $_.GetValue("InstallDir") }
}

$cudaDlls = @("cudart64_*.dll", "cublas64_*.dll", "cublasLt64_*.dll")
$cudaCopied = 0
if ($cudaPath -and (Test-Path "$cudaPath\bin")) {
    foreach ($pattern in $cudaDlls) {
        Get-ChildItem "$cudaPath\bin" -Filter $pattern -ErrorAction SilentlyContinue |
            ForEach-Object {
                Copy-Item $_.FullName (Join-Path $DistDirFull $_.Name) -Force
                Write-Host "  [OK] $($_.Name)" -ForegroundColor Green
                $cudaCopied++
            }
    }
}
if ($cudaCopied -eq 0) {
    Write-Host "  (No CUDA runtime DLLs found - build may be CPU-only)" -ForegroundColor Gray
}

# ── MSVC runtime DLLs (vcredist approach) ────────────────────────────────
Write-Host ""
Write-Host "--- MSVC runtime DLLs ---" -ForegroundColor White

# Find the MSVC redist directory
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msvcRedistCopied = 0
if (Test-Path $vsWhere) {
    $vsPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($vsPath) {
        # Find the x64 redist directory
        $redistDir = Get-ChildItem "$vsPath\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
                     Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
        if ($redistDir -and (Test-Path $redistDir)) {
            $msvcRuntime = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140_1.dll")
            foreach ($dll in $msvcRuntime) {
                $src = Join-Path $redistDir $dll
                if (Test-Path $src) {
                    Copy-Item $src (Join-Path $DistDirFull $dll) -Force
                    Write-Host "  [OK] $dll" -ForegroundColor Green
                    $msvcRedistCopied++
                }
            }
        }
    }
}
if ($msvcRedistCopied -eq 0) {
    Write-Warning @"
  MSVC runtime DLLs could not be located automatically.
  Install them via 'Microsoft Visual C++ Redistributable' on the target machine,
  or copy msvcp140.dll, vcruntime140.dll, vcruntime140_1.dll manually.
"@
}

# ── Create a README in dist dir ───────────────────────────────────────────
$readmePath = Join-Path $DistDirFull "README.txt"
@"
RetDec - Windows Native Build (MSVC; GUI on FuseUI)
Copyright (c) 2025 Odin Loch Trading as Imortek

Contents:
  retdec-decompiler.exe   Main decompiler (CLI)
  retdec-gui.exe          GUI application (FuseUI; resources in share\retdec\gui)
  retdec-unpacker.exe     Archive unpacker
  retdec-fileinfo.exe     File-format inspector

Usage:
  retdec-decompiler.exe binary.exe -o output.c
  retdec-decompiler.exe script.luac -o output.lua
  retdec-decompiler.exe script.pyc  -o output.py
  retdec-gui.exe binary.exe

There is no retdec-qwen3-runner. Optional neural refinement is
RETDEC_NEURAL_REFINE + RETDEC_NEURAL_MODEL.

src/cuda_accel is parked research (RETDEC_ENABLE_CUDA_ACCEL defaults OFF).
Do not treat this folder as a GPU-accelerated decompiler.
"@ | Set-Content $readmePath -Encoding UTF8

# ── Final summary ─────────────────────────────────────────────────────────
Write-Host ""
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host "  Build complete!" -ForegroundColor Green
Write-Host ""
Write-Host "  Output: $DistDirFull" -ForegroundColor White

$exes = Get-ChildItem $DistDirFull -Filter "*.exe" | Select-Object -ExpandProperty Name
foreach ($e in $exes) { Write-Host "    $e" -ForegroundColor Gray }

Write-Host ""
Write-Host "  Test:" -ForegroundColor White
Write-Host "    .\$DistDir\retdec-decompiler.exe --help" -ForegroundColor Cyan
Write-Host "    .\$DistDir\retdec-gui.exe" -ForegroundColor Cyan
Write-Host "    .\scripts\Test-RetdecWindows.ps1 -DistDir $DistDir" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host ""

if ($PackageInstallers) {
    $installerScript = Join-Path $PSScriptRoot "build-windows-installer.ps1"
    Write-Host "--- Packaging installers (releases\windows\) ---" -ForegroundColor White
    & $installerScript -SkipBuild
    if ($LASTEXITCODE -ne 0) {
        Write-Error "build-windows-installer.ps1 failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
}

} finally {
    Pop-Location
}
