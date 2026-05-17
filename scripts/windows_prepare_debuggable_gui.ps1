#Requires -Version 5.1
<#
.SYNOPSIS
    Turn the existing MSVC Release build into a debuggable GUI: /Zi PDBs, rebuild
    RetDec targets (not LLVM/OpenSSL externals), stage dist\windows\debuggable.

.DESCRIPTION
    You do not need the Visual Studio IDE — only Build Tools + CMake + Ninja (same
    as a normal build).  This script:
      1. Loads vcvars64 (same helper as windows_native_build.ps1).
      2. Reconfigures CMAKE_CXX_FLAGS_RELEASE with /Zi and linker /DEBUG:FULL.
      3. Rebuilds retdec-gui and its dependencies (qwen3, panels, etc.).
      4. Mirrors dist\windows → dist\windows\debuggable and overwrites
         retdec-gui.exe + retdec-gui.pdb from the build tree.

.PARAMETER BuildDir
    CMake build directory (default: build\windows).

.PARAMETER DistSource
    Reference staging folder to copy DLL/Qt layout from (default: dist\windows).

.PARAMETER DistOut
    Output folder for the debuggable bundle (default: dist\windows\debuggable).

.PARAMETER Jobs
    Parallel compile jobs (default: 24).
#>
param(
    [string]$BuildDir    = "build\windows",
    [string]$DistSource  = "dist\windows",
    [string]$DistOut     = "dist\windows\debuggable",
    [int]   $Jobs       = 24
)

$ErrorActionPreference = "Stop"
$RepoRoot     = Split-Path -Parent $PSScriptRoot
$BuildDirFull = Join-Path $RepoRoot $BuildDir
$DistSrcFull  = Join-Path $RepoRoot $DistSource
$DistOutFull  = Join-Path $RepoRoot $DistOut

function Import-VsEnv([string]$vcvarsPath) {
    $cmdExe  = Join-Path $env:SystemRoot "System32\cmd.exe"
    $cmdLine = "`"$vcvarsPath`" && set"
    $output  = & $cmdExe /c $cmdLine 2>&1
    foreach ($line in $output) {
        if ($line -match "^([^=]+)=(.*)$") {
            $name  = $Matches[1].Trim()
            $value = $Matches[2].Trim()
            Set-Item -Path "env:$name" -Value $value -ErrorAction SilentlyContinue
        }
    }
}

$env:PATH = "$env:SystemRoot\System32;$env:PATH"

if (-not (Test-Path (Join-Path $BuildDirFull "CMakeCache.txt"))) {
    Write-Error "No CMakeCache.txt in '$BuildDirFull'. Run windows_native_configure.ps1 first."
    exit 1
}

$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Get-Command "cl.exe" -ErrorAction SilentlyContinue)) {
    if (-not (Test-Path $vsWhere)) { Write-Error "vswhere.exe not found."; exit 1 }
    $vsPath = & $vsWhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    Import-VsEnv $vcvars
}

if (-not (Get-Command "ninja" -ErrorAction SilentlyContinue)) {
    $vsPath2 = & $vsWhere -latest -products * -property installationPath 2>$null
    $n = Join-Path $vsPath2 "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    if (Test-Path $n) { $env:PATH = (Split-Path $n) + ";" + $env:PATH }
}

# Windows Kits rc.exe on PATH for any triggered external steps
$kitBin = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\rc.exe" -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
if ($kitBin) { $env:PATH = "$(Split-Path $kitBin.FullName);$env:PATH" }

# Qt windeployqt (discover like windows_native_build.ps1)
$qt6Dir = $env:Qt6_DIR
if (-not $qt6Dir) {
    $qtRoots = @("C:\Qt", "$env:USERPROFILE\Qt", "${env:ProgramFiles}\Qt", "${env:ProgramFiles(x86)}\Qt")
    foreach ($r in $qtRoots) {
        if (-not (Test-Path $r)) { continue }
        $found = Get-ChildItem "$r\*\*\lib\cmake\Qt6" -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "msvc" } |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($found) { $qt6Dir = $found.FullName; break }
    }
}
if ($qt6Dir -and (Test-Path $qt6Dir)) {
    $QtKitRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $qt6Dir))
    $env:PATH = "$QtKitRoot\bin;$env:PATH"
}

Write-Host ""
Write-Host "=== Reconfigure: Release + PDB (/Zi, /DEBUG:FULL) ===" -ForegroundColor Cyan
cmake `
    -S $RepoRoot `
    -B $BuildDirFull `
    -DCMAKE_CXX_FLAGS_RELEASE="/MD /O2 /Ob2 /DNDEBUG /Zi" `
    -DCMAKE_EXE_LINKER_FLAGS_RELEASE="/INCREMENTAL:NO /DEBUG:FULL" `
    -DCMAKE_SHARED_LINKER_FLAGS_RELEASE="/INCREMENTAL:NO /DEBUG:FULL"
if ($LASTEXITCODE -ne 0) { Write-Error "CMake reconfigure failed: $LASTEXITCODE"; exit $LASTEXITCODE }

Write-Host ""
Write-Host "=== Rebuild retdec-gui (pulls qwen3, panels, …) ===" -ForegroundColor Cyan
cmake --build $BuildDirFull --config Release --parallel $Jobs --target retdec-gui
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed: $LASTEXITCODE"; exit $LASTEXITCODE }

$builtExe = Join-Path $BuildDirFull "src\gui\retdec-gui.exe"
$builtPdb = Join-Path $BuildDirFull "src\gui\retdec-gui.pdb"
if (-not (Test-Path $builtExe)) { Write-Error "Built exe missing: $builtExe"; exit 1 }

Write-Host ""
Write-Host "=== Stage $DistOut (mirror + overwrite exe/pdb) ===" -ForegroundColor Cyan
if (Test-Path $DistSrcFull) {
    New-Item -ItemType Directory -Force -Path $DistOutFull | Out-Null
    robocopy $DistSrcFull $DistOutFull /E /XO /NFL /NDL /NJH /NJS /nc /ns /np | Out-Null
    if ($LASTEXITCODE -gt 7) { Write-Error "robocopy failed with exit $LASTEXITCODE"; exit $LASTEXITCODE }
} else {
    Write-Warning "Source dist '$DistSrcFull' missing — creating output dir only."
    New-Item -ItemType Directory -Force -Path $DistOutFull | Out-Null
}

Copy-Item $builtExe $DistOutFull -Force

$winDeploy = Get-Command "windeployqt6.exe" -ErrorAction SilentlyContinue
if (-not $winDeploy) { $winDeploy = Get-Command "windeployqt.exe" -ErrorAction SilentlyContinue }
if ($winDeploy) {
    $guiOut = Join-Path $DistOutFull "retdec-gui.exe"
    & $winDeploy.Source --release --no-translations --no-system-d3d-compiler $guiOut
}

# Copy PDB and exe again after windeployqt (ensures we never ship a stale exe from robocopy).
Copy-Item $builtExe $DistOutFull -Force
if (Test-Path $builtPdb) {
    Copy-Item $builtPdb $DistOutFull -Force
    Write-Host "  [OK] retdec-gui + retdec-gui.pdb staged" -ForegroundColor Green
} else {
    Write-Warning "retdec-gui.pdb not found at $builtPdb — check linker /DEBUG:FULL."
}

$readme = Join-Path $DistOutFull "README_DEBUG.txt"
@"
RetDec GUI — debuggable build (PDB next to exe)

Run from this folder so DLLs load (same as dist\windows).

Capture a crash dump without Visual Studio:
  ..\scripts\run_gui_with_procdump.ps1

Turn a .dmp into a text report (!analyze -v); uses cdb (SDK) or WinDbgX (Store):
  ..\scripts\windows_analyze_crash_dump.ps1

Or open the dump manually in WinDbg from the Microsoft Store:
  File → Open executable → retdec-gui.exe
  (Point symbol path at this folder so retdec-gui.pdb is found.)

"@ | Set-Content $readme -Encoding UTF8

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host "  Folder: $DistOutFull" -ForegroundColor White
Write-Host "  Next:   .\scripts\run_gui_with_procdump.ps1" -ForegroundColor White

# robocopy uses exit codes 0-7 for success with different meanings; do not leak 1-3 as script failure.
exit 0
