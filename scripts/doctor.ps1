#Requires -Version 5.1
<#
.SYNOPSIS
  Check common RetDec build prerequisites (read-only).

.DESCRIPTION
  Verifies CMake >= 3.26, a C++23 compiler (MSVC 2022 17.6+ / cl 19.36+),
  fetch-large-files marker, NSIS/makensis, EnVar NSIS plugin, git-lfs, and
  python3. Prints a pass/fail summary. There is no GUI toolkit to check for:
  retdec-gui is built from FuseUI and Forge, vendored in third_party/.

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

# ── C++23 compiler (MSVC) ─────────────────────────────────────────────────────
# The tree is C++23; retdec-gui's model layer uses std::expected and
# std::format. cl 19.36 (Visual Studio 2022 17.6) is the oldest that builds it.
$clMinMajor = 19
$clMinMinor = 36
$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($cl) {
    $banner = (& $cl.Source 2>&1 | Select-Object -First 1) -join ""
    if ($banner -match 'Version (\d+)\.(\d+)') {
        $clMaj = [int]$Matches[1]; $clMin = [int]$Matches[2]
        if ($clMaj -gt $clMinMajor -or ($clMaj -eq $clMinMajor -and $clMin -ge $clMinMinor)) {
            Write-Pass "C++23 compiler: cl $clMaj.$clMin"
        } else {
            Write-Fail "cl $clMaj.$clMin is too old for C++23 - need $clMinMajor.$clMinMinor+ (Visual Studio 2022 17.6+)"
        }
    } else {
        Write-Warn "cl.exe on PATH but its version could not be read: $banner"
    }
} else {
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $vsVer = $null
    if (Test-Path -LiteralPath $vsWhere) {
        $vsVer = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property catalog_productDisplayVersion 2>$null
    }
    if ($vsVer -and $vsVer -match '^(\d+)\.(\d+)') {
        $vsMaj = [int]$Matches[1]; $vsMin = [int]$Matches[2]
        if ($vsMaj -gt 17 -or ($vsMaj -eq 17 -and $vsMin -ge 6)) {
            Write-Pass "C++23 compiler: Visual Studio $vsVer with MSVC (run from a Developer shell, or the configure script finds vcvars64.bat)"
        } else {
            Write-Fail "Visual Studio $vsVer is too old for C++23 - need 2022 17.6+"
        }
    } else {
        Write-Fail "no MSVC found - install Visual Studio 2022 17.6+ with 'Desktop development with C++'"
    }
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

# ── licence files ─────────────────────────────────────────────────────────────
foreach ($f in @("LICENSE", "LICENSE-AGPL", "LICENSE-COMMERCIAL", "NOTICE")) {
    $p = Join-Path $RepoRoot $f
    if (Test-Path -LiteralPath $p) {
        Write-Pass "$f present"
    } else {
        Write-Fail "missing $f — run scripts/install-licence-files.sh"
    }
}

# ── dependency release candidates ─────────────────────────────────────────────
$deps = Join-Path $RepoRoot "cmake\deps.cmake"
if (Test-Path -LiteralPath $deps) {
    $depsText = Get-Content -LiteralPath $deps -Raw
    if ($depsText -match '5\.0-rc|v4\.2\.0-rc') {
        Write-Fail "cmake/deps.cmake still pins a release-candidate dependency"
    } else {
        Write-Pass "no RC dependency pins in cmake/deps.cmake"
    }
}

# ── CI workflows enabled ──────────────────────────────────────────────────────
foreach ($wf in @("ci-smoke.yml", "ctest-linux.yml")) {
    $wfPath = Join-Path $RepoRoot ".github\workflows\$wf"
    if ((Test-Path -LiteralPath $wfPath) -and (Select-String -Path $wfPath -Pattern '^\s*push:' -Quiet)) {
        Write-Pass "workflow $wf triggers on push"
    } else {
        Write-Fail "workflow $wf missing push trigger"
    }
}

# ── commercial GPL exclusion ──────────────────────────────────────────────────
$packaging = Join-Path $RepoRoot "packaging"
if (Test-Path -LiteralPath $packaging) {
    $hits = Get-ChildItem -Path $packaging -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { Select-String -Path $_.FullName -Pattern 'capstone2llvmirtool' -Quiet }
    if ($hits) {
        Write-Fail "capstone2llvmirtool found in packaging/ (commercial GPL exclusion)"
    } else {
        Write-Pass "no capstone2llvmirtool in packaging manifests"
    }
}

Write-Host ""
Write-Host "Summary: $($script:Pass) passed, $($script:Fail) failed, $($script:Warn) warnings"
if ($script:Fail -gt 0) {
    exit 1
}
exit 0
