$errs = $null
$null = [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'windows_native_configure.ps1'),
    [ref]$null,
    [ref]$errs
)
$errs | ForEach-Object { $_.ToString() }
