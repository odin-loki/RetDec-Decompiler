#Requires -Version 5.1
<#
.SYNOPSIS
    Analyze a retdec-gui crash .dmp (!analyze -v) and save a text report.

.DESCRIPTION
    Tries, in order:
      1) cdb.exe (Windows SDK / Debugging Tools) — output captured in the console.
      2) WinDbgX.exe (Microsoft Store "WinDbg") — uses -logo to write the report file.
    Symbol path defaults to dist\windows\debuggable (retdec-gui.pdb) plus Microsoft's
    public symbol server.

    Store WinDbg (WinDbgX) is a packaged GUI app and may show Microsoft account / sign-in
    UI before running commands; for unattended use, install cdb.exe (Debugging Tools) and
    use -CdbOnly, or ensure cdb is on PATH so this script picks it first.

.PARAMETER CdbOnly
    Do not fall back to WinDbgX. Fails if cdb.exe is not installed (best for automation).

.PARAMETER DumpPath
    Full path to a .dmp file. If omitted, uses the newest *.dmp under _crash_dumps.

.PARAMETER SymbolDir
    Folder containing retdec-gui.pdb (default: dist\windows\debuggable in repo root).

.PARAMETER OutFile
    Text report path. Default: same base name as the dump with _analysis.txt suffix.

.PARAMETER ExtraCommands
    Optional extra cdb commands before quit, e.g. "lm m retdec*"
#>
param(
    [switch]$CdbOnly,
    [string]$DumpPath   = "",
    [string]$SymbolDir  = "",
    [string]$OutFile    = "",
    [string]$ExtraCommands = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

if ($SymbolDir -eq "") {
    $SymbolDir = Join-Path $RepoRoot "dist\windows\debuggable"
}
if (-not (Test-Path $SymbolDir)) {
    Write-Warning "Symbol folder not found: $SymbolDir`nRun scripts\windows_prepare_debuggable_gui.ps1 for a matching PDB."
}

if ($DumpPath -eq "") {
    $dumpsDir = Join-Path $RepoRoot "_crash_dumps"
    if (-not (Test-Path $dumpsDir)) {
        Write-Error "No _crash_dumps folder. Run scripts\run_gui_with_procdump.ps1 and reproduce the crash first."
        exit 1
    }
    $latest = Get-ChildItem -LiteralPath $dumpsDir -Filter "*.dmp" -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $latest) {
        Write-Error "No .dmp files in $dumpsDir"
        exit 1
    }
    $DumpPath = $latest.FullName
    Write-Host "Using newest dump: $DumpPath" -ForegroundColor Cyan
}

if (-not (Test-Path -LiteralPath $DumpPath)) {
    Write-Error "Dump not found: $DumpPath"
    exit 1
}

function Find-CdbExe {
    $cmd = Get-Command "cdb.exe" -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }

    # Typical layout: ...\Windows Kits\10\Debuggers\x64\cdb.exe (any kit version folder)
    $kitParents = @(
        "${env:ProgramFiles(x86)}\Windows Kits",
        "${env:ProgramFiles}\Windows Kits"
    )
    foreach ($parent in $kitParents) {
        if (-not (Test-Path -LiteralPath $parent)) { continue }
        foreach ($verDir in (Get-ChildItem -LiteralPath $parent -Directory -ErrorAction SilentlyContinue)) {
            $dbg = Join-Path $verDir.FullName "Debuggers\x64\cdb.exe"
            if (Test-Path -LiteralPath $dbg) { return $dbg }
        }
    }

    # Fallback: any cdb.exe under ...\Windows Kits\*\Debuggers (prefer x64)
    $all = [System.Collections.Generic.List[string]]::new()
    foreach ($parent in $kitParents) {
        if (-not (Test-Path -LiteralPath $parent)) { continue }
        foreach ($verDir in (Get-ChildItem -LiteralPath $parent -Directory -ErrorAction SilentlyContinue)) {
            $root = Join-Path $verDir.FullName "Debuggers"
            if (-not (Test-Path -LiteralPath $root)) { continue }
            foreach ($f in (Get-ChildItem -LiteralPath $root -Recurse -Filter "cdb.exe" -File -ErrorAction SilentlyContinue)) {
                $all.Add($f.FullName)
            }
        }
    }
    if ($all.Count -eq 0) { return $null }
    $pick = $all | Where-Object { $_ -match '\\x64\\' } | Sort-Object -Descending | Select-Object -First 1
    if ($pick) { return $pick }
    return ($all | Sort-Object -Descending | Select-Object -First 1)
}

function Find-WinDbgXExe {
    $cmd = Get-Command "WinDbgX.exe" -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }

    $stub = Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\WinDbgX.exe"
    if (Test-Path -LiteralPath $stub) { return $stub }

    $appsRoot = Join-Path $env:ProgramFiles "WindowsApps"
    if (Test-Path -LiteralPath $appsRoot) {
        foreach ($dir in (Get-ChildItem -LiteralPath $appsRoot -Directory -Filter "Microsoft.WinDbg*" -ErrorAction SilentlyContinue)) {
            $exe = Join-Path $dir.FullName "WinDbgX.exe"
            if (Test-Path -LiteralPath $exe) { return $exe }
        }
    }
    return $null
}

if ($OutFile -eq "") {
    $dir = Split-Path -Parent $DumpPath
    $base = [System.IO.Path]::GetFileNameWithoutExtension($DumpPath)
    $OutFile = Join-Path $dir "${base}_analysis.txt"
}

# Local PDBs first, then cached copies of Microsoft symbols
$cache = Join-Path $env:LOCALAPPDATA "Symbols\msdl"
$symPath = "$SymbolDir;srv*$cache*https://msdl.microsoft.com/download/symbols"

# Command list: cdb uses -cf script file; WinDbgX uses repeated -c (one command per -c).
# WinDbgX native argv parsing treats ".reload /f /i" as multiple tokens; standalone "/f" becomes
# "Unrecognised command line option F". Use plain ".reload" for WinDbgX only; keep /f /i for cdb.
$cmdFile = $null
$cmdLinesCdb = [System.Collections.Generic.List[string]]::new()
$cmdLinesCdb.Add(".symopt+0x10")
$cmdLinesCdb.Add(".reload /f /i")
if ($ExtraCommands.Trim().Length -gt 0) {
    foreach ($part in ($ExtraCommands -split ";")) {
        $t = $part.Trim()
        if ($t.Length -gt 0) { $cmdLinesCdb.Add($t) }
    }
}
$cmdLinesCdb.Add("!analyze -v")
$cmdLinesCdb.Add("kv")
$cmdLinesCdb.Add("q")

$cmdLinesWinDbg = [System.Collections.Generic.List[string]]::new()
foreach ($line in $cmdLinesCdb) {
    if ($line -ceq ".reload /f /i") {
        $cmdLinesWinDbg.Add(".reload")
    } else {
        $cmdLinesWinDbg.Add($line)
    }
}

$cdb     = Find-CdbExe
$windbgx = Find-WinDbgXExe

if ($CdbOnly -and -not $cdb) {
    Write-Error @'
CdbOnly was set but cdb.exe was not found.

Install "Windows Debugging Tools" (Visual Studio Installer -> Individual components),
then add to PATH for this session, for example:
  $env:PATH = "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64;$env:PATH"
'@
    exit 1
}

if (-not $cdb -and -not $windbgx) {
    Write-Error @'
No debugger found (cdb.exe or WinDbgX.exe).

Recommended for scripts — Windows SDK (console only, no Store sign-in):
  Visual Studio Installer -> Individual components -> "Windows Debugging Tools"
  Typical path: C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe

Optional — Microsoft Store "WinDbg" (WinDbgX):
  May prompt for Microsoft account / first-run UI; can block automation until dismissed.
'@
    exit 1
}

Write-Host ""
Write-Host "dump:     $DumpPath" -ForegroundColor Gray
Write-Host "symbols:  $symPath" -ForegroundColor Gray
Write-Host "report:   $OutFile" -ForegroundColor Gray
Write-Host ""

try {
    if ($cdb) {
        $cmdFile = Join-Path $env:TEMP ("retdec_analyze_dump_{0}.txt" -f [Guid]::NewGuid().ToString("n"))
        Set-Content -LiteralPath $cmdFile -Value $cmdLinesCdb -Encoding ascii
        Write-Host "Engine:   cdb (SDK)" -ForegroundColor Gray
        Write-Host "cmd file: $cmdFile" -ForegroundColor DarkGray
        Write-Host "Running analysis (may take a minute on large dumps)..." -ForegroundColor Yellow
        $output = & $cdb "-z" $DumpPath "-y" $symPath "-cf" $cmdFile "-lines" 2>&1
        $output | Out-File -LiteralPath $OutFile -Encoding utf8
    } else {
        Write-Host "Engine:   WinDbgX (Microsoft Store)" -ForegroundColor Gray
        Write-Warning @'
Store WinDbg can show a Microsoft sign-in or first-run window (packaged app + account UX).
It is not required for dump analysis - close it, choose Skip, or sign in once; then the script should continue.
For fully unattended runs, install cdb.exe (Debugging Tools) and re-run; cdb is used automatically when found.
'@
        Write-Host "Running analysis (may take a minute on large dumps)..." -ForegroundColor Yellow
        if (Test-Path -LiteralPath $OutFile) { Remove-Item -LiteralPath $OutFile -Force }
        $windbgArgs = [System.Collections.Generic.List[string]]::new()
        foreach ($a in @("-Q", "-z", $DumpPath, "-y", $symPath, "-logo", $OutFile)) {
            $windbgArgs.Add([string]$a)
        }
        foreach ($line in $cmdLinesWinDbg) {
            $windbgArgs.Add("-c")
            $windbgArgs.Add($line)
        }
        $p = Start-Process -FilePath $windbgx -ArgumentList $windbgArgs.ToArray() -PassThru -Wait
        if ($p.ExitCode -ne 0) {
            Write-Warning "WinDbgX exited with code $($p.ExitCode); check $OutFile anyway."
        }
    }
} finally {
    if ($cmdFile -and (Test-Path -LiteralPath $cmdFile)) {
        Remove-Item -LiteralPath $cmdFile -Force -ErrorAction SilentlyContinue
    }
}

if (-not (Test-Path -LiteralPath $OutFile) -or ((Get-Item -LiteralPath $OutFile).Length -lt 80)) {
    Write-Error "Analysis output missing or empty. Install cdb (Debugging Tools) for reliable automation, or dismiss WinDbg sign-in and retry."
    exit 1
}

Write-Host ""
Write-Host "Done. Open the report:" -ForegroundColor Green
Write-Host "  $OutFile" -ForegroundColor White
