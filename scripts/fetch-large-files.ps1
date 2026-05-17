# scripts/fetch-large-files.ps1
# ---------------------------------------------------------------------------
# Download the large source data files that are deliberately not committed
# to keep this repo small (see .gitignore). Required before the first build.
#
# Run from the repo root:
#   .\scripts\fetch-large-files.ps1                  # default, only fetch missing
#   .\scripts\fetch-large-files.ps1 -Force           # overwrite even if present
#   .\scripts\fetch-large-files.ps1 -BaseUrl <url>   # alternate mirror
#
# Files fetched (all <= 16 MiB each, ~60 MiB total):
#   support/types/{windows,windrivers,linux}.json
#   support/yara_patterns/signsrch/signsrch.yara
#   support/yara_patterns/tools/pe/x86/packers.yara
#   support/ordinals/x86/mfc*.ord
#   src/llvmir2hll/var_name_gen/var_name_gens/word_var_name_gen.cpp
# ---------------------------------------------------------------------------

[CmdletBinding()]
param(
    [string]$BaseUrl = "https://raw.githubusercontent.com/avast/retdec/master",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..")

# Per-file list. Keep in sync with .gitignore.
$files = @(
    "support/types/windows.json",
    "support/types/windrivers.json",
    "support/types/linux.json",
    "support/yara_patterns/signsrch/signsrch.yara",
    "support/yara_patterns/tools/pe/x86/packers.yara",
    "src/llvmir2hll/var_name_gen/var_name_gens/word_var_name_gen.cpp",
    # MFC ordinals (Windows binary symbol resolution).
    "support/ordinals/x86/mfc100.ord", "support/ordinals/x86/mfc100d.ord",
    "support/ordinals/x86/mfc100u.ord", "support/ordinals/x86/mfc100ud.ord",
    "support/ordinals/x86/mfc110.ord", "support/ordinals/x86/mfc110d.ord",
    "support/ordinals/x86/mfc110u.ord", "support/ordinals/x86/mfc110ud.ord"
)

$skipped = 0
$downloaded = 0
$failed = 0

foreach ($rel in $files) {
    $dst = Join-Path $repo $rel
    $dir = Split-Path -Parent $dst
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    if ((Test-Path $dst) -and -not $Force) {
        Write-Host "skip   $rel" -ForegroundColor DarkGray
        $skipped++
        continue
    }
    $url = "$BaseUrl/$rel"
    Write-Host "fetch  $rel " -NoNewline
    try {
        Invoke-WebRequest -Uri $url -OutFile $dst -UseBasicParsing
        $size = (Get-Item $dst).Length
        Write-Host ("({0} KiB)" -f [math]::Round($size/1KB, 1)) -ForegroundColor Green
        $downloaded++
    } catch {
        Write-Host "FAILED: $_" -ForegroundColor Red
        $failed++
    }
}

Write-Host ""
Write-Host ("Done: downloaded {0}, skipped {1}, failed {2}." -f $downloaded, $skipped, $failed)
if ($failed -gt 0) { exit 1 }
