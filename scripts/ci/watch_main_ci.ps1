# Poll GitHub Actions on main until key workflows finish; log failures.
$ErrorActionPreference = "Continue"
$RepoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location $RepoRoot

$watch = @(
    "ci-smoke", "ctest-linux", "ctest-macos", "ctest-windows",
    "standalone-check", "doc-integrity", "codeql", "fuzz-pr", "verify-esbmc"
)
$ref = "main"
$log = Join-Path $env:TEMP "retdec-ci-watch.log"
"=== watch started $(Get-Date -Format o) ref=$ref ===" | Out-File $log -Encoding utf8

function Get-Status {
    $runs = gh run list --branch $ref --limit 30 --json databaseId,name,status,conclusion,workflowName,createdAt |
        ConvertFrom-Json
    $by = @{}
    foreach ($r in $runs) {
        $w = $r.workflowName
        if ($watch -notcontains $w) { continue }
        if (-not $by.ContainsKey($w) -or $r.createdAt -gt $by[$w].createdAt) {
            $by[$w] = $r
        }
    }
    return $by
}

while ($true) {
    $by = Get-Status
    $line = "$(Get-Date -Format 'HH:mm:ss') "
    $pending = 0
    foreach ($w in $watch) {
        if (-not $by.ContainsKey($w)) {
            $line += "${w}=? "
            $pending++
            continue
        }
        $r = $by[$w]
        $line += "${w}=$($r.status)/$($r.conclusion) "
        if ($r.status -ne "completed") { $pending++ }
        if ($r.conclusion -eq "failure") {
            $msg = "FAIL $w run $($r.databaseId)"
            $msg | Tee-Object -FilePath $log -Append
            gh run view $r.databaseId --log-failed 2>&1 | Select-Object -Last 40 |
                Out-File $log -Append -Encoding utf8
        }
    }
    $line | Tee-Object -FilePath $log -Append
    if ($pending -eq 0) {
        "=== all watched workflows completed $(Get-Date -Format o) ===" | Out-File $log -Append
        break
    }
    Start-Sleep -Seconds 120
}

Write-Host "Log: $log"
