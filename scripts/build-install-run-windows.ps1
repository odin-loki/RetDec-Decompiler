#requires -Version 5.1
<#
.SYNOPSIS
  Configure (CMake preset), build, install, and run RetDec Qwen3 tools on Windows.

.DESCRIPTION
  Loads Visual Studio Dev Shell when available (so MSVC + Windows SDK includes work),
  then runs cmake --preset, cmake --build, cmake --install, and optionally runs:
    - Runner: install\...\bin\retdec-qwen3-runner.exe (default)
    - Gui:         install\...\bin\retdec-gui.exe (visible window)
    - GuiHeadless: same binary with --headless and optional --headless-exit-ms (Qt offscreen)
    - GuiTests:    build\...\tests\gui\retdec-gui-tests.exe (QTest + gtest, headless env)
    - Tests:       build\...\tests\qwen3\retdec-qwen3-tests.exe (not installed by default)

.EXAMPLE
  .\scripts\build-install-run-windows.ps1

.EXAMPLE
  .\scripts\build-install-run-windows.ps1 -Model "C:\models\qwen3.gguf" -RunnerArgs @("-p","Hello","-n","16")

.EXAMPLE
  .\scripts\build-install-run-windows.ps1 -Trace -TraceVerbose -Model "C:\models\moe.gguf" -RunnerArgs @("-p","test","-n","4")

.EXAMPLE
  .\scripts\build-install-run-windows.ps1 -Run Gui

.EXAMPLE
  .\scripts\build-install-run-windows.ps1 -Run Tests -SkipInstall

.EXAMPLE
  .\scripts\build-install-run-windows.ps1 -Run GuiHeadless -HeadlessExitMs 15000 -Trace
#>

[CmdletBinding()]
param(
    [string] $SourceDir = "",
    [string] $Preset = "full-windows-release",
    [ValidateSet("Runner", "Gui", "GuiHeadless", "GuiTests", "Tests")]
    [string] $Run = "Runner",
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
        "Runner" {
            $exe = Join-Path $installBin "retdec-qwen3-runner.exe"
            if (-not (Test-Path -LiteralPath $exe)) {
                throw "Runner not found: $exe (build/install may have failed or preset differs)."
            }
            if (-not $Model -and $RunnerArgs.Count -eq 0) {
                $RunnerArgs = @("--version")
                Write-Host "No -Model or -RunnerArgs; running smoke: retdec-qwen3-runner --version"
            }
            Write-Host "==> & `"$exe`" ..."
            if ($Model) {
                & $exe $Model @RunnerArgs
            } else {
                & $exe @RunnerArgs
            }
            if ($LASTEXITCODE -ne 0) { throw "retdec-qwen3-runner exited with code $LASTEXITCODE" }
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
            $exe = Join-Path $buildDir "tests\qwen3\retdec-qwen3-tests.exe"
            if (-not (Test-Path -LiteralPath $exe)) {
                throw "Tests binary not found: $exe (configure with RETDEC_TESTS=ON and build retdec-qwen3-tests)."
            }
            Write-Host "==> & `"$exe`""
            & $exe
            if ($LASTEXITCODE -ne 0) { throw "retdec-qwen3-tests failed with exit code $LASTEXITCODE" }
        }
    }
}
finally {
    Pop-Location
}

Write-Host "OK: build/install/run finished."
