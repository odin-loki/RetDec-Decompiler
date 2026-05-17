#Requires -Version 5.1
<#
.SYNOPSIS
    Configure RetDec for a full native Windows build (MSVC + CUDA + Qt6 GUI).
    Self-bootstrapping: automatically finds and sources vcvars64.bat.

.PARAMETER Preset
    CMake preset (default: full-windows-release). Binary dir: build\windows\.

.PARAMETER QtDir
    Path to Qt6 CMake config dir, e.g. C:\Qt\6.11.0\msvc2022_64\lib\cmake\Qt6
    Auto-detected if not supplied.

.PARAMETER CudaPath
    Path to CUDA Toolkit root. Auto-detected from CUDA_PATH env or registry.

.PARAMETER NoCuda
    Disable CUDA acceleration in CMake (CPU-only; full presets default to CUDA ON when the toolkit is found).

.PARAMETER AllowOptionalQt
    Allow configure without Qt6 by passing -DRETDEC_REQUIRE_QT6=OFF (full-* presets normally require Qt for retdec-gui).

.PARAMETER BuildDir
    Override CMake binary directory (default: build\windows under the repo). Use a path outside OneDrive if Ninja fails with "recompaction: Permission denied".

.PARAMETER InstallPrefix
    Override CMAKE_INSTALL_PREFIX (default: install\windows under the repo). Should match the -InstallPrefix passed to windows_native_build.ps1.

.EXAMPLE
    .\scripts\windows_native_configure.ps1

.EXAMPLE
    .\scripts\windows_native_configure.ps1 -QtDir "C:\Qt\6.11.0\msvc2022_64\lib\cmake\Qt6"
#>

param(
    [string]$Preset     = "full-windows-release",
    [string]$QtDir      = "",
    [string]$CudaPath   = "",
    [string]$BuildDir   = "",
    [string]$InstallPrefix = "",
    [switch]$NoCuda,
    [switch]$AllowOptionalQt
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

# ── Step 3: Add Perl to PATH if not found ────────────────────────────────
Write-Host ""
Write-Host "--- Perl ---" -ForegroundColor White
if (-not (Get-Command "perl" -ErrorAction SilentlyContinue)) {
    $perlCandidates = @(
        "C:\Strawberry\perl\bin",
        "C:\Perl\bin",
        "C:\Perl64\bin"
    )
    $perlFound = $perlCandidates | Where-Object { Test-Path "$_\perl.exe" } | Select-Object -First 1
    if ($perlFound) {
        $env:PATH = $perlFound + ";" + $env:PATH
        Write-Host "  [OK] Perl: $(perl --version | Select-String 'v\d' | Select-Object -First 1)" -ForegroundColor Green
    } else {
        Write-Error "perl not found. Install Strawberry Perl from https://strawberryperl.com"
        exit 1
    }
} else {
    Write-Host "  [OK] Perl: $(perl --version | Select-String 'v\d' | Select-Object -First 1)" -ForegroundColor Green
}

# ── Step 4: CMake ─────────────────────────────────────────────────────────
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
        Write-Warning "CUDA Toolkit not found in this shell; CMake will still probe for NVCC. Install CUDA for GPU acceleration, or use -NoCuda for a CPU-only configure."
    }
}
if ($NoCuda) {
    Write-Host "  [--] CUDA disabled by -NoCuda flag (RETDEC_ENABLE_CUDA_ACCEL=OFF)" -ForegroundColor Gray
}

# ── Step 6: Resolve Qt6 ───────────────────────────────────────────────────
Write-Host ""
Write-Host "--- Qt6 ---" -ForegroundColor White
if ($QtDir -eq "" -and $env:Qt6_DIR) { $QtDir = $env:Qt6_DIR }
if ($QtDir -eq "") {
    $qtRoots = @("C:\Qt", "$env:USERPROFILE\Qt", "${env:ProgramFiles}\Qt", "${env:ProgramFiles(x86)}\Qt")
    foreach ($r in $qtRoots) {
        if (-not (Test-Path $r)) { continue }
        # Search for any Qt6 cmake config under any version / any msvc kit
        $found = Get-ChildItem "$r\*\*\lib\cmake\Qt6" -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -match "msvc" } |
                 Sort-Object FullName -Descending | Select-Object -First 1
        if ($found) { $QtDir = $found.FullName; break }
    }
}
if ($QtDir -ne "" -and (Test-Path $QtDir)) {
    Write-Host "  [OK] Qt6: $QtDir" -ForegroundColor Green
    $env:Qt6_DIR = $QtDir
    # Kit root = grandparent of lib/cmake/Qt6
    $QtKitRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $QtDir))
    # Add Qt bin to PATH for windeployqt
    $env:PATH = "$QtKitRoot\bin;" + $env:PATH
} else {
    if ($Preset -match '^full-' -and -not $AllowOptionalQt) {
        Write-Error "Preset '$Preset' requires Qt6 for retdec-gui. Install Qt 6 (MSVC x64), set `$env:Qt6_DIR or -QtDir, or pass -AllowOptionalQt for CLI-only."
        exit 1
    }
    Write-Warning "Qt6 not found; preset may skip GUI unless you use -AllowOptionalQt with a non-full preset."
    $QtDir   = ""
    $QtKitRoot = ""
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
if ($QtDir -ne "") {
    $cmakeArgs += "-D", "Qt6_DIR=$QtDir"
    if ($QtKitRoot) { $cmakeArgs += "-D", "CMAKE_PREFIX_PATH=$QtKitRoot" }
}
if ($NoCuda) {
    $cmakeArgs += "-D", "RETDEC_ENABLE_CUDA_ACCEL=OFF"
}
if ($AllowOptionalQt) {
    $cmakeArgs += "-D", "RETDEC_REQUIRE_QT6=OFF"
}

Write-Host ""
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host "  Configuring RetDec (CMake preset)                   " -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host "  Preset    : $Preset" -ForegroundColor White
Write-Host "  Build dir : $BuildDirFull" -ForegroundColor White
$cudaDisplay = if ($enableCuda) { $CudaPath } else { "OFF" }
$qtDisplay   = if ($QtDir -ne "") { $QtDir } else { "auto / optional" }
Write-Host "  CUDA      : $cudaDisplay" -ForegroundColor White
Write-Host "  Qt6       : $qtDisplay" -ForegroundColor White
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
