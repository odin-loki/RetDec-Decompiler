#Requires -Version 5.1
<#
.SYNOPSIS
  Check common RetDec build prerequisites (read-only).

.DESCRIPTION
  Verifies CMake >= 3.26, fetch-large-files marker, Qt6 hint, NSIS/makensis,
  EnVar NSIS plugin, git-lfs, python3, and perl. Prints a pass/fail summary.

.EXAMPLE
  .\scripts\doctor.ps1
#>

[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$CMakeMinMajor = 3
$CMakeMinMinor = 26
$LargeFileMarker = "src\llvmir2hll\var_name_gen\var_name_gens\word_var_name_gen.cpp"

$script:Pass = 0
$script:Fail = 0
$script:Warn = 0

function Write-Pass([string]$Message) {
    Write-Host "PASS  $Message" -ForegroundColor Green
    $script:Pass++
}

function Write-Fail([string]$Message) {
    Write-Host "FAIL  $Message" -ForegroundColor Red
    $script:Fail++
}

function Write-Warn([string]$Message) {
    Write-Host "WARN  $Message" -ForegroundColor Yellow
    $script:Warn++
}

Write-Host ""
Write-Host "RetDec doctor (Windows)"
Write-Host "repo: $RepoRoot"
Write-Host ""

# ── CMake ─────────────────────────────────────────────────────────────────────
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) {
    $verLine = (& cmake --version 2>$null | Select-Object -First 1)
    if ($verLine -match '(\d+)\.(\d+)') {
        $maj = [int]$Matches[1]
        $min = [int]$Matches[2]
        if ($maj -gt $CMakeMinMajor -or ($maj -eq $CMakeMinMajor -and $min -ge $CMakeMinMinor)) {
            Write-Pass "cmake $($Matches[0]) (>= $($CMakeMinMajor).$($CMakeMinMinor))"
        } else {
            Write-Fail "cmake $($Matches[0]) - need >= $($CMakeMinMajor).$($CMakeMinMinor)"
        }
    } else {
        Write-Pass "cmake on PATH ($verLine)"
    }
} else {
    Write-Fail "cmake not on PATH - install CMake $($CMakeMinMajor).$($CMakeMinMinor)+"
}

# ── Large support files ───────────────────────────────────────────────────────
$marker = Join-Path $RepoRoot $LargeFileMarker
if (Test-Path -LiteralPath $marker) {
    Write-Pass "fetch-large-files marker present ($LargeFileMarker)"
} else {
    Write-Fail "missing $LargeFileMarker - run: .\scripts\fetch-large-files.ps1"
}

# ── Qt6 hint ──────────────────────────────────────────────────────────────────
$qtOk = $false
if ($env:Qt6_DIR -and (Test-Path -LiteralPath $env:Qt6_DIR)) {
    Write-Pass "Qt6 hint: Qt6_DIR=$($env:Qt6_DIR)"
    $qtOk = $true
} else {
    foreach ($root in @("C:\Qt\6.11.0\msvc2022_64", "C:\Qt", "$env:USERPROFILE\Qt")) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        if (Test-Path (Join-Path $root "bin\Qt6Core.dll")) {
            Write-Pass "Qt6 hint: $root"
            $qtOk = $true
            break
        }
        $found = Get-ChildItem "$root\*\msvc*_64" -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName "bin\Qt6Core.dll") } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($found) {
            Write-Pass "Qt6 hint: $($found.FullName)"
            $qtOk = $true
            break
        }
    }
}
if (-not $qtOk) {
    Write-Warn "Qt6 not detected - GUI presets need Qt 6 MSVC x64 (see Install-RetdecWindowsDeps.ps1)"
}

# ── NSIS / makensis ───────────────────────────────────────────────────────────
$makensis = Get-Command makensis.exe -ErrorAction SilentlyContinue
if (-not $makensis) {
    $makensis = Get-Command makensis -ErrorAction SilentlyContinue
}
if (-not $makensis) {
    $defaultNsis = Join-Path ${env:ProgramFiles(x86)} "NSIS\makensis.exe"
    if (Test-Path -LiteralPath $defaultNsis) {
        $makensis = Get-Item -LiteralPath $defaultNsis
    }
}
if ($makensis) {
    $makensisPath = $makensis | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue
    if (-not $makensisPath) {
        $makensisPath = $makensis | Select-Object -ExpandProperty FullName -ErrorAction SilentlyContinue
    }
    if (-not $makensisPath) { $makensisPath = "$makensis" }
    Write-Pass "NSIS makensis ($makensisPath)"
} else {
    Write-Warn "makensis not found - optional for setup.exe (portable zip still works)"
}

# ── EnVar NSIS plugin ─────────────────────────────────────────────────────────
$nsisRoot = Join-Path ${env:ProgramFiles(x86)} "NSIS"
$envarDll = Join-Path $nsisRoot "Plugins\x86-unicode\EnVar.dll"
$envarNsh = Join-Path $nsisRoot "Include\EnVar.nsh"
if ((Test-Path -LiteralPath $envarDll) -and (Test-Path -LiteralPath $envarNsh)) {
    Write-Pass "EnVar NSIS plugin installed"
} elseif ($makensis) {
    Write-Fail "EnVar NSIS plugin missing - installer PATH updates will fail at compile time"
    Write-Host "       Install from https://github.com/GsNSIS/EnVar/releases" -ForegroundColor Cyan
    Write-Host "       Copy EnVar.dll -> $nsisRoot\Plugins\x86-unicode\" -ForegroundColor Cyan
    Write-Host "       Copy EnVar.nsh  -> $nsisRoot\Include\" -ForegroundColor Cyan
} else {
    Write-Warn "EnVar NSIS plugin not checked (makensis absent)"
}

# ── git-lfs ───────────────────────────────────────────────────────────────────
$git = Get-Command git -ErrorAction SilentlyContinue
if ($git) {
    $lfsOut = & git lfs version 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Pass "git-lfs $($lfsOut -join ' ')"
    } else {
        Write-Warn "git-lfs not available (optional unless you use legacy LFS objects)"
    }
} else {
    Write-Fail "git not on PATH"
}

# ── python3 ───────────────────────────────────────────────────────────────────
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) {
    $py = Get-Command python3 -ErrorAction SilentlyContinue
}
if ($py) {
    $pyVer = & $py.Source --version 2>&1
    Write-Pass "python $($pyVer -join ' ')"
} else {
    Write-Fail "python not on PATH - needed for validate_pipeline_json.py and ci-smoke tests"
}

# ── perl ──────────────────────────────────────────────────────────────────────
$perl = Get-Command perl -ErrorAction SilentlyContinue
if ($perl) {
    $perlVer = & perl -e 'print $^V' 2>$null
    if (-not $perlVer) { $perlVer = (& perl --version 2>&1 | Select-Object -Skip 1 -First 1) }
    Write-Pass "perl $perlVer"
} else {
    Write-Fail "perl not on PATH - required for bundled OpenSSL configure (Strawberry Perl)"
}

Write-Host ""
Write-Host "Summary: $($script:Pass) passed, $($script:Fail) failed, $($script:Warn) warnings"
if ($script:Fail -gt 0) {
    exit 1
}
exit 0
