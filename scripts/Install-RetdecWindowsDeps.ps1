#Requires -Version 5.1
<#
.SYNOPSIS
    Install prerequisites for a native Windows RetDec build (CUDA + Qt6 GUI + MSVC).

.DESCRIPTION
    Checks for and installs (via winget where possible):
      - Visual Studio Build Tools 2022 (MSVC v143, C++, Windows SDK)
      - CUDA Toolkit (NVIDIA — requires an NVIDIA GPU; skipped gracefully if absent)
      - Qt 6 for Windows (MSVC 2019/2022 x64 build)
      - CMake 3.28+
      - Ninja build system
      - Perl (Strawberry Perl — required for bundled OpenSSL configure)
      - Git

    After running this script, open a "Developer PowerShell for VS 2022"
    (or x64 Native Tools Command Prompt) and run:
        .\scripts\windows_native_configure.ps1
        .\scripts\windows_native_build.ps1

.NOTES
    - Run as Administrator.
    - winget must be available (ships with Windows 10 21H2+ / Windows 11).
    - Qt6 installation via winget installs the Qt Maintenance Tool; the
      script also provides a direct offline-installer download link.
    - CUDA Toolkit must match your driver version. Check
      https://developer.nvidia.com/cuda-downloads for the latest installer.
#>

$ErrorActionPreference = "Continue"
$script:AnyMissing = $false

function Test-Command($name) {
    return [bool](Get-Command $name -ErrorAction SilentlyContinue)
}

function Write-Check($label, $ok, $hint = "") {
    if ($ok) {
        Write-Host "  [OK]  $label" -ForegroundColor Green
    } else {
        Write-Host "  [!!]  $label  -- MISSING" -ForegroundColor Yellow
        if ($hint) { Write-Host "        $hint" -ForegroundColor Cyan }
        $script:AnyMissing = $true
    }
}

Write-Host ""
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host "  RetDec Windows Native Build — Dependency Installer  " -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host ""

# ── winget ────────────────────────────────────────────────────────────────
$hasWinget = Test-Command "winget"
if (-not $hasWinget) {
    Write-Host "[!] winget is not available. Install it from the Microsoft Store (App Installer)." -ForegroundColor Red
    Write-Host "    Alternatively install each dependency manually using the links below." -ForegroundColor Red
    Write-Host ""
}

# ── Visual Studio Build Tools ─────────────────────────────────────────────
Write-Host "--- Visual Studio / MSVC ---" -ForegroundColor White
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$hasMSVC = $false
if (Test-Path $vsWhere) {
    $vsPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    $hasMSVC = ($vsPath -ne $null -and $vsPath -ne "")
}
Write-Check "MSVC (Visual Studio Build Tools 2022)" $hasMSVC `
    "winget install --id Microsoft.VisualStudio.2022.BuildTools --override `"--add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --includeRecommended --quiet`""

if (-not $hasMSVC -and $hasWinget) {
    Write-Host "  Installing Visual Studio Build Tools 2022 ..." -ForegroundColor Cyan
    winget install --id Microsoft.VisualStudio.2022.BuildTools `
        --override "--add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --includeRecommended --quiet --wait" `
        --accept-package-agreements --accept-source-agreements
}

Write-Host ""

# ── CMake ─────────────────────────────────────────────────────────────────
Write-Host "--- CMake ---" -ForegroundColor White
$cmakeVer = $null
if (Test-Command "cmake") {
    $cmakeVer = (cmake --version 2>$null | Select-Object -First 1) -replace "cmake version ",""
}
$hasCmake = ($cmakeVer -ne $null) -and ([version]$cmakeVer -ge [version]"3.18")
Write-Check "CMake 3.18+" $hasCmake "winget install --id Kitware.CMake"
if (-not $hasCmake -and $hasWinget) {
    Write-Host "  Installing CMake ..." -ForegroundColor Cyan
    winget install --id Kitware.CMake --accept-package-agreements --accept-source-agreements
}

Write-Host ""

# ── Ninja ─────────────────────────────────────────────────────────────────
Write-Host "--- Ninja ---" -ForegroundColor White
$hasNinja = Test-Command "ninja"
Write-Check "Ninja" $hasNinja "winget install --id Ninja-build.Ninja"
if (-not $hasNinja -and $hasWinget) {
    Write-Host "  Installing Ninja ..." -ForegroundColor Cyan
    winget install --id Ninja-build.Ninja --accept-package-agreements --accept-source-agreements
}

Write-Host ""

# ── Git ────────────────────────────────────────────────────────────────────
Write-Host "--- Git ---" -ForegroundColor White
$hasGit = Test-Command "git"
Write-Check "Git" $hasGit "winget install --id Git.Git"
if (-not $hasGit -and $hasWinget) {
    Write-Host "  Installing Git ..." -ForegroundColor Cyan
    winget install --id Git.Git --accept-package-agreements --accept-source-agreements
}

Write-Host ""

# ── Perl (Strawberry) — required for bundled OpenSSL ─────────────────────
Write-Host "--- Perl (Strawberry) ---" -ForegroundColor White
$hasPerl = Test-Command "perl"
if ($hasPerl) {
    $perlVer = (perl --version 2>$null | Select-String "v\d+\.\d+\.\d+" | ForEach-Object { $_.Matches[0].Value })
    Write-Check "Perl ($perlVer)" $true
} else {
    Write-Check "Perl" $false "winget install --id StrawberryPerl.StrawberryPerl"
    if ($hasWinget) {
        Write-Host "  Installing Strawberry Perl ..." -ForegroundColor Cyan
        winget install --id StrawberryPerl.StrawberryPerl --accept-package-agreements --accept-source-agreements
    }
}

Write-Host ""

# ── CUDA Toolkit ──────────────────────────────────────────────────────────
Write-Host "--- CUDA Toolkit (NVIDIA GPU required) ---" -ForegroundColor White
$cudaDir = $env:CUDA_PATH
if (-not $cudaDir) {
    $cudaDir = Get-ChildItem "HKLM:\SOFTWARE\NVIDIA Corporation\GPU Computing Toolkit\CUDA" -ErrorAction SilentlyContinue |
        Sort-Object Name | Select-Object -Last 1 |
        ForEach-Object { $_.GetValue("InstallDir") }
}
$hasCuda = ($cudaDir -ne $null) -and (Test-Path "$cudaDir\bin\nvcc.exe")
$cudaHint = @"
Download from: https://developer.nvidia.com/cuda-downloads
Choose: Windows > x86_64 > your Windows version > exe (local)
After install, CUDA_PATH env var should point to the toolkit root.
"@
Write-Check "CUDA Toolkit (nvcc.exe)" $hasCuda $cudaHint

if ($hasCuda) {
    $nvccVer = (& "$cudaDir\bin\nvcc.exe" --version 2>$null | Select-String "release" | ForEach-Object { $_.Line }) -replace ".*release ",""
    Write-Host "        CUDA at: $cudaDir  (nvcc $nvccVer)" -ForegroundColor Gray
}

Write-Host ""

# ── Qt6 ───────────────────────────────────────────────────────────────────
Write-Host "--- Qt 6 (MSVC 2019/2022 x64) ---" -ForegroundColor White
# Check common Qt install locations
$qt6Candidates = @(
    "C:\Qt",
    "$env:USERPROFILE\Qt",
    "${env:ProgramFiles}\Qt",
    "${env:ProgramFiles(x86)}\Qt"
)
$qt6Root = $null
foreach ($c in $qt6Candidates) {
    if (Test-Path $c) {
        # Look for any Qt 6.x msvc directory
        $found = Get-ChildItem "$c\6.*\msvc*_64\lib\cmake\Qt6" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $qt6Root = $found.FullName; break }
    }
}
# Also check CMAKE_PREFIX_PATH env
if (-not $qt6Root -and $env:Qt6_DIR) { $qt6Root = $env:Qt6_DIR }

$hasQt6 = ($qt6Root -ne $null)
$qt6Hint = @"
Download the Qt Online Installer from: https://www.qt.io/download-qt-installer
During install, select: Qt 6.7.x (or latest) > MSVC 2019 64-bit (or MSVC 2022 64-bit)
After install, set Qt6_DIR to the cmake config dir, e.g.:
    `$env:Qt6_DIR = 'C:\Qt\6.7.3\msvc2019_64\lib\cmake\Qt6'
Or set CMAKE_PREFIX_PATH to the kit root:
    `$env:CMAKE_PREFIX_PATH = 'C:\Qt\6.7.3\msvc2019_64'
"@
Write-Check "Qt 6 (MSVC x64)" $hasQt6 $qt6Hint
if ($hasQt6) {
    Write-Host "        Qt6 found at: $qt6Root" -ForegroundColor Gray
}

Write-Host ""

# ── Summary ────────────────────────────────────────────────────────────────
Write-Host "======================================================" -ForegroundColor Cyan
if ($script:AnyMissing) {
    Write-Host "  Some dependencies are missing (see [!!] above)." -ForegroundColor Yellow
    Write-Host "  After installing them, RESTART your terminal so" -ForegroundColor Yellow
    Write-Host "  PATH changes take effect, then re-run this script" -ForegroundColor Yellow
    Write-Host "  to verify everything is in order." -ForegroundColor Yellow
} else {
    Write-Host "  All dependencies found — ready to build!" -ForegroundColor Green
}
Write-Host ""
Write-Host "  Next steps:" -ForegroundColor White
Write-Host "    1. Open 'x64 Native Tools Command Prompt for VS 2022'" -ForegroundColor White
Write-Host "       (or 'Developer PowerShell for VS 2022')" -ForegroundColor White
Write-Host "    2. Set Qt6_DIR if not auto-detected:" -ForegroundColor White
Write-Host "       `$env:Qt6_DIR = 'C:\Qt\6.x.x\msvc2019_64\lib\cmake\Qt6'" -ForegroundColor Cyan
Write-Host "    3. Run: .\scripts\windows_native_configure.ps1" -ForegroundColor White
Write-Host "    4. Run: .\scripts\windows_native_build.ps1" -ForegroundColor White
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host ""
