# run_gui_tests_under_debugger.ps1
# ---------------------------------------------------------------------------
# Run retdec-gui-tests under cdb.exe so any crash gets a stack trace and a
# minidump. Suitable for both interactive use and CI (returns non-zero on
# crash or timeout).
#
# Usage:
#   .\scripts\run_gui_tests_under_debugger.ps1                 # auto-detect build
#   .\scripts\run_gui_tests_under_debugger.ps1 -BuildDir build\windows
#   .\scripts\run_gui_tests_under_debugger.ps1 -GtestFilter "ProgressPanelTest.*"
# ---------------------------------------------------------------------------

[CmdletBinding()]
param(
    [string]$BuildDir,
    [string]$GtestFilter = "",
    [int]$TimeoutSec = 600,
    [switch]$NoDebugger,
    [string]$OutDir = "gui-tests-debug-artifacts"
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) { Write-Host "[gui-test-dbg] $msg" -ForegroundColor Cyan }

if (-not $BuildDir) {
    foreach ($cand in @("build\windows", "build\Release", "build\Debug", "build")) {
        if (Test-Path (Join-Path $cand "tests\gui\retdec-gui-tests.exe")) {
            $BuildDir = $cand
            break
        }
    }
}

if (-not $BuildDir) {
    Write-Error "Could not auto-detect build dir; pass -BuildDir."
    exit 2
}

$TestExe = Resolve-Path (Join-Path $BuildDir "tests\gui\retdec-gui-tests.exe")
Write-Step "Tests: $TestExe"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$LogFile = Join-Path $OutDir "tests.log"
$DumpFile = Join-Path $OutDir "tests.dmp"
if (Test-Path $LogFile) { Remove-Item $LogFile }
if (Test-Path $DumpFile) { Remove-Item $DumpFile }

$env:RETDEC_GUI_HEADLESS = "1"
$env:QT_QPA_PLATFORM = "offscreen"

$gtestArgs = @()
if ($GtestFilter) { $gtestArgs += "--gtest_filter=$GtestFilter" }
$gtestArgs += "--gtest_color=no"

if ($NoDebugger) {
    Write-Step "Running without debugger."
    $proc = Start-Process -FilePath $TestExe -ArgumentList $gtestArgs `
        -NoNewWindow -PassThru -RedirectStandardOutput $LogFile `
        -RedirectStandardError (Join-Path $OutDir "tests.err.log")
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        Write-Warning "Timeout after $TimeoutSec s; killing."
        try { $proc.Kill() } catch {}
        exit 124
    }
    exit $proc.ExitCode
}

$cdb = (Get-Command cdb.exe -ErrorAction SilentlyContinue).Source
if (-not $cdb) {
    foreach ($g in @(
        "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe",
        "C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe"
    )) { if (Test-Path $g) { $cdb = $g; break } }
}

if (-not $cdb) {
    Write-Warning "cdb.exe not found; falling back to no-debugger mode."
    & $PSCommandPath -BuildDir $BuildDir -GtestFilter $GtestFilter `
        -TimeoutSec $TimeoutSec -NoDebugger -OutDir $OutDir
    exit $LASTEXITCODE
}

Write-Step "cdb: $cdb"

$cdbScript = Join-Path $OutDir "cdb.script"
@"
sxe -c "" gp
sxe -c ".dump /ma $DumpFile;qd" av
sxe -c ".dump /ma $DumpFile;qd" eh
sxe -c ".dump /ma $DumpFile;qd" sov
.lines -e
g
qd
"@ | Set-Content -Path $cdbScript -Encoding ASCII

$cdbArgs = @(
    "-g", "-G",
    "-cf", $cdbScript,
    "-logo", $LogFile,
    "-c", "g",
    $TestExe
) + $gtestArgs

Write-Step "Launching: $cdb $($cdbArgs -join ' ')"
$proc = Start-Process -FilePath $cdb -ArgumentList $cdbArgs `
    -NoNewWindow -PassThru
if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
    Write-Warning "Timeout after $TimeoutSec s; killing cdb (and child tests)."
    try { $proc.Kill() } catch {}
    exit 124
}
Write-Step "cdb exited with $($proc.ExitCode). Log: $LogFile"
if (Test-Path $DumpFile) { Write-Step "Dump: $DumpFile" }
exit $proc.ExitCode
