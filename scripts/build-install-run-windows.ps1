#requires -Version 5.1
<#
.SYNOPSIS
  Configure (CMake preset), build, install, and run RetDec on Windows.

.DESCRIPTION
  Loads Visual Studio Dev Shell when available (so MSVC + Windows SDK includes work),
  then runs cmake --preset, cmake --build, cmake --install, and optionally runs:
    - Decompiler:  install\...\bin\retdec-decompiler.exe (default)
    - Gui:         install\...\bin\retdec-gui.exe (visible window)
    - GuiHeadless: same binary with --headless and optional --headless-exit-ms (Qt offscreen)
    - GuiTests:    build\...\tests\gui\retdec-gui-tests.exe (QTest + gtest, headless env)
    - Runner/Tests: leftover Qwen3 paths; they do not ship (DOC-06). Neural path is RETDEC_NEURAL_REFINE.

.EXAMPLE
  .\scripts\build-install-run-windows.ps1

.EXAMPLE
  .\scripts\build-install-run-windows.ps1 -Run Gui

.EXAMPLE
  .\scripts\build-install-run-windows.ps1 -Run GuiTests -SkipInstall

.EXAMPLE
  .\scripts\build-install-run-windows.ps1 -Run GuiHeadless -HeadlessExitMs 15000
#>

[CmdletBinding()]
param(
    [string] $SourceDir = "",
    [string] $Preset = "full-windows-release",
    [ValidateSet("Decompiler", "Runner", "Gui", "GuiHeadless", "GuiTests", "Tests")]
    [string] $Run = "Decompiler",
    [int] $HeadlessExitMs = 12000,
    [string] $Model = "",
    [string[]] $RunnerArgs = @(),
    [switch] $Trace,
    [switch] $TraceVerbose,
    [switch] $SkipConfigure,
    [switch] $SkipBuild,
    [switch] $SkipInstall,
    [switch] $SkipRun,
    [switch] $PackageInstallers,
    [int] $Parallel = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "retdec-paths.ps1")

if (-not $SourceDir) {
    $SourceDir = Get-RetDecRepoRoot
}
$SourceDir = (Resolve-Path -LiteralPath $SourceDir).Path

$buildDir   = Join-Path $SourceDir "build\windows"
$installDir = Join-Path $SourceDir "install\windows"
$installBin = Join-Path $installDir "bin"

function Invoke-CMakeBuild {
    $argList = @("--build", $buildDir)
    if ($Parallel -gt 0) {
        $argList += @("--parallel", "$Parallel")
    } else {
        $argList += "--parallel"
    }
    & cmake @argList
    if ($LASTEXITCODE -ne 0) { throw "cmake --build failed with exit code $LASTEXITCODE" }
}

if (-not (Enter-RetDecVsDevShell)) {
    Write-Warning "VS Dev Shell not loaded; if the build fails missing MSVC headers, use Developer PowerShell for VS."
}

Push-Location $SourceDir
try {
    if (-not $SkipConfigure) {
        Write-Host "==> cmake --preset $Preset"
        & cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw "cmake --preset failed with exit code $LASTEXITCODE" }
    } else {
        if (-not (Test-Path -LiteralPath (Join-Path $buildDir "CMakeCache.txt"))) {
            throw "No CMake cache at $buildDir ; run without -SkipConfigure first."
        }
    }

    if (-not $SkipBuild) {
        Write-Host "==> cmake --build (preset build dir)"
        Invoke-CMakeBuild
    }

    if (-not $SkipInstall) {
        Write-Host "==> cmake --install $buildDir"
        & cmake --install $buildDir
        if ($LASTEXITCODE -ne 0) { throw "cmake --install failed with exit code $LASTEXITCODE" }
    }

    if ($PackageInstallers) {
        Write-Host "==> Packaging Windows installers"
        & (Join-Path $PSScriptRoot "build-windows-installer.ps1") -SkipBuild
        if ($LASTEXITCODE -ne 0) { throw "build-windows-installer.ps1 failed with exit code $LASTEXITCODE" }
    }

    if ($SkipRun) {
        Write-Host "Done (skipped run). Install bin: $installBin"
        return
    }

    if ($Trace) { $env:RETDEC_QWEN3_TRACE = "1" } else { Remove-Item env:RETDEC_QWEN3_TRACE -ErrorAction SilentlyContinue }
    if ($TraceVerbose) { $env:RETDEC_QWEN3_TRACE_VERBOSE = "1" } else { Remove-Item env:RETDEC_QWEN3_TRACE_VERBOSE -ErrorAction SilentlyContinue }

    if ($Run -eq "GuiHeadless" -or $Run -eq "GuiTests") {
        if (-not $env:RETDEC_GUI_HEADLESS) { $env:RETDEC_GUI_HEADLESS = "1" }
    }

    switch ($Run) {
        "Decompiler" {
            $exe = Join-Path $installBin "retdec-decompiler.exe"
            if (-not (Test-Path -LiteralPath $exe)) {
                throw "retdec-decompiler not found: $exe (build/install may have failed or preset differs)."
            }
            if ($RunnerArgs.Count -eq 0) {
                $RunnerArgs = @("--help")
            }
            Write-Host "==> & `"$exe`" $($RunnerArgs -join ' ')"
            & $exe @RunnerArgs
            if ($LASTEXITCODE -ne 0) { throw "retdec-decompiler exited with code $LASTEXITCODE" }
        }
        "Runner" {
            throw "retdec-qwen3-runner does not ship (DOC-06). Use -Run Decompiler, or set RETDEC_NEURAL_REFINE + RETDEC_NEURAL_MODEL."
        }
        "Gui" {
            $exe = Join-Path $installBin "retdec-gui.exe"
            if (-not (Test-Path -LiteralPath $exe)) {
                throw "retdec-gui not found: $exe (Qt6 / GUI not built?)."
            }
            Write-Host "==> & `"$exe`""
            & $exe
            if ($LASTEXITCODE -ne 0) { throw "retdec-gui exited with code $LASTEXITCODE" }
        }
        "GuiHeadless" {
            $exe = Join-Path $installBin "retdec-gui.exe"
            if (-not (Test-Path -LiteralPath $exe)) {
                throw "retdec-gui not found: $exe (Qt6 / GUI not built?)."
            }
            $hArgs = @("--headless")
            if ($HeadlessExitMs -gt 0) {
                $hArgs += @("--headless-exit-ms", "$HeadlessExitMs")
            }
            Write-Host "==> & `"$exe`" $($hArgs -join ' ')"
            & $exe @hArgs
            if ($LASTEXITCODE -ne 0) { throw "retdec-gui (headless) exited with code $LASTEXITCODE" }
        }
        "GuiTests" {
            $exe = Join-Path $buildDir "tests\gui\retdec-gui-tests.exe"
            if (-not (Test-Path -LiteralPath $exe)) {
                throw "retdec-gui-tests not found: $exe (build target retdec-gui-tests)."
            }
            Write-Host "==> & `"$exe`" (RETDEC_GUI_HEADLESS=1)"
            & $exe
            if ($LASTEXITCODE -ne 0) { throw "retdec-gui-tests failed with exit code $LASTEXITCODE" }
        }
        "Tests" {
            throw "retdec-qwen3-tests does not exist. Use -Run GuiTests, or ctest in the build tree."
        }
    }
}
finally {
    Pop-Location
}

Write-Host "OK: build/install/run finished."
