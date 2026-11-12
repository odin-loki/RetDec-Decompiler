#Requires -Version 5.1
<#
.SYNOPSIS
    Configure RetDec for a full native Windows build (MSVC with C++23; RETDEC_ENABLE_CUDA_ACCEL=OFF).
    retdec-gui is built from the vendored FuseUI and Forge (third_party/); no GUI toolkit is installed.
    Self-bootstrapping: automatically finds and sources vcvars64.bat.

.PARAMETER Preset
    CMake preset (default: full-windows-release). Binary dir: build\windows\.

.PARAMETER CudaPath
    Path to CUDA Toolkit root. Auto-detected from CUDA_PATH env or registry.

.PARAMETER NoCuda
    Disable CUDA acceleration in CMake (RETDEC_ENABLE_CUDA_ACCEL=OFF; full presets do not default CUDA ON).

.PARAMETER NoGui
    Configure without retdec-gui (-DRETDEC_GUI=OFF; CLI-only).

.PARAMETER BuildDir
    Override CMake binary directory (default: build\windows under the repo). Use a path outside OneDrive if Ninja fails with "recompaction: Permission denied".

.PARAMETER InstallPrefix
    Override CMAKE_INSTALL_PREFIX (default: install\windows under the repo). Should match the -InstallPrefix passed to windows_native_build.ps1.

.EXAMPLE
    .\scripts\windows_native_configure.ps1

.EXAMPLE
    .\scripts\windows_native_configure.ps1 -Preset full-windows-debug
#>

param(
    [string]$Preset     = "full-windows-release",
    [string]$CudaPath   = "",
    [string]$BuildDir   = "",
    [string]$InstallPrefix = "",
    [switch]$NoCuda,
    [switch]$NoGui
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot

# ── Helper: import vcvars64.bat env into current PowerShell session ────────
# (Must be outside try { }: nested function definitions break the PS parser.)
function Import-VsEnv([string]$vcvarsPath) {
    Write-Host "  Sourcing $vcvarsPath ..." -ForegroundColor Gray
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

# ── Step 1: Find and source MSVC (vcvars64.bat) ───────────────────────────
Write-Host ""
Write-Host "--- MSVC ---" -ForegroundColor White

# Check if cl.exe is already on PATH
if (-not (Get-Command "cl.exe" -ErrorAction SilentlyContinue)) {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vsWhere)) {
        Write-Error "vswhere.exe not found and cl.exe is not on PATH. Install Visual Studio Build Tools."
        exit 1
    }
    # Get the latest VS with VC++ tools
    $vsPath = & $vsWhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if (-not $vsPath) {
        Write-Error "No Visual Studio with C++ tools found. Install Visual Studio Build Tools 2022."
        exit 1
    }
    $vcvarsPath = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvarsPath)) {
        Write-Error "vcvars64.bat not found at $vcvarsPath"
        exit 1
    }
    Import-VsEnv $vcvarsPath
}

if (Get-Command "cl.exe" -ErrorAction SilentlyContinue) {
    # cl.exe prints the version banner to stderr; avoid terminating on stderr under $ErrorActionPreference = Stop
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    $clVer = (cl.exe 2>&1 | Select-String "Version" | Select-Object -First 1).Line
    $ErrorActionPreference = $prevEap
    Write-Host "  [OK] MSVC: $clVer" -ForegroundColor Green
} else {
    Write-Error "cl.exe still not found after sourcing vcvars64.bat."
    exit 1
}

# ── Step 2: Add VS Ninja to PATH if standalone ninja not found ────────────
Write-Host ""
Write-Host "--- Ninja ---" -ForegroundColor White
if (-not (Get-Command "ninja" -ErrorAction SilentlyContinue)) {
    # Ninja ships inside VS Build Tools
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath2 = & $vsWhere -latest -products * -property installationPath 2>$null
    $vsNinja = Join-Path $vsPath2 "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    if (Test-Path $vsNinja) {
        $env:PATH = (Split-Path $vsNinja) + ";" + $env:PATH
        Write-Host "  [OK] Ninja (VS bundled): $vsNinja" -ForegroundColor Green
    } else {
        Write-Error "ninja not found. Install Ninja (winget install Ninja-build.Ninja) or install VS CMake tools."
        exit 1
    }
} else {
    Write-Host "  [OK] Ninja: $(ninja --version)" -ForegroundColor Green
}

# ── Step 3: CMake ─────────────────────────────────────────────────────────
Write-Host ""
Write-Host "--- CMake ---" -ForegroundColor White
if (-not (Get-Command "cmake" -ErrorAction SilentlyContinue)) {
    Write-Error "cmake not found. Install from https://cmake.org or winget install Kitware.CMake"
    exit 1
}
Write-Host "  [OK] $(cmake --version | Select-Object -First 1)" -ForegroundColor Green

# ── Step 5: Resolve CUDA ─────────────────────────────────────────────────
Write-Host ""
Write-Host "--- CUDA ---" -ForegroundColor White
$enableCuda = $false
if (-not $NoCuda) {
    # Try env var first
    if ($CudaPath -eq "") { $CudaPath = $env:CUDA_PATH }
    # Try registry
    if (-not $CudaPath -or -not (Test-Path "$CudaPath\bin\nvcc.exe")) {
        $regKey = Get-ChildItem "HKLM:\SOFTWARE\NVIDIA Corporation\GPU Computing Toolkit\CUDA" -ErrorAction SilentlyContinue |
                  Sort-Object Name -Descending | Select-Object -First 1
        if ($regKey) { $CudaPath = $regKey.GetValue("InstallDir") }
    }
    # Walk the toolkit dir for any version
    if (-not $CudaPath -or -not (Test-Path "$CudaPath\bin\nvcc.exe")) {
        $nvccSearch = Get-ChildItem "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA" -Filter "nvcc.exe" -Recurse -ErrorAction SilentlyContinue |
                      Select-Object -First 1
        if ($nvccSearch) { $CudaPath = Split-Path (Split-Path $nvccSearch.FullName) }
    }
    if ($CudaPath -and (Test-Path "$CudaPath\bin\nvcc.exe")) {
        # Add nvcc to PATH if not already there
        if (-not (Get-Command "nvcc" -ErrorAction SilentlyContinue)) {
            $env:PATH = "$CudaPath\bin;" + $env:PATH
            $env:CUDA_PATH = $CudaPath
        }
        $prevEapNv = $ErrorActionPreference
        $ErrorActionPreference = "SilentlyContinue"
        $nvccVer = (nvcc --version 2>&1 | Select-String "release")
        $ErrorActionPreference = $prevEapNv
        Write-Host "  [OK] CUDA: $CudaPath  ($nvccVer)" -ForegroundColor Green
        $enableCuda = $true
    } else {
        Write-Warning "CUDA Toolkit not found in this shell; CMake will still probe for NVCC. RETDEC_ENABLE_CUDA_ACCEL stays OFF on full-windows presets. Pass -NoCuda to skip the probe, or -DRETDEC_ENABLE_CUDA_ACCEL=ON only for parked research builds."
    }
}
if ($NoCuda) {
    Write-Host "  [--] CUDA disabled by -NoCuda flag (RETDEC_ENABLE_CUDA_ACCEL=OFF)" -ForegroundColor Gray
}

# ── Step 7: CMake preset (default: build\windows\, install\windows\) ───────
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDirFull = Join-Path $RepoRoot "build\windows"
} else {
    $BuildDirFull = $BuildDir
}
New-Item -ItemType Directory -Force -Path $BuildDirFull | Out-Null

$cmakeArgs = @("--preset", $Preset)
if (-not [string]::IsNullOrWhiteSpace($BuildDir)) {
    $cmakeArgs += "-B", $BuildDirFull
}
if (-not [string]::IsNullOrWhiteSpace($InstallPrefix)) {
    $cmakeArgs += "-D", "CMAKE_INSTALL_PREFIX=$InstallPrefix"
}
if ($NoCuda) {
    $cmakeArgs += "-D", "RETDEC_ENABLE_CUDA_ACCEL=OFF"
}
if ($NoGui) {
    $cmakeArgs += "-D", "RETDEC_GUI=OFF"
}

Write-Host ""
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host "  Configuring RetDec (CMake preset)                   " -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host "  Preset    : $Preset" -ForegroundColor White
Write-Host "  Build dir : $BuildDirFull" -ForegroundColor White
$cudaDisplay = if ($enableCuda) { $CudaPath } else { "OFF" }
$guiDisplay  = if ($NoGui) { "OFF" } else { "ON (FuseUI, vendored)" }
Write-Host "  CUDA      : $cudaDisplay" -ForegroundColor White
Write-Host "  GUI       : $guiDisplay" -ForegroundColor White
Write-Host ""

cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configure FAILED (exit code $LASTEXITCODE)."
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "======================================================" -ForegroundColor Green
Write-Host "  Configure succeeded!" -ForegroundColor Green
$nextBuild = ".\scripts\windows_native_build.ps1 -Preset $Preset"
if (-not [string]::IsNullOrWhiteSpace($BuildDir)) { $nextBuild += " -BuildDir `"$BuildDirFull`"" }
if (-not [string]::IsNullOrWhiteSpace($InstallPrefix)) { $nextBuild += " -InstallPrefix `"$InstallPrefix`"" }
Write-Host "  Next: $nextBuild" -ForegroundColor Green
Write-Host "======================================================" -ForegroundColor Green

} finally {
    Pop-Location
}
