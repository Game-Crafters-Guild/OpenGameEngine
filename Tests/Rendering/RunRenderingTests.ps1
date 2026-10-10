Param(
    [switch]$StopOnFail
)

$ErrorActionPreference = 'Stop'
$repoRoot = Get-Location
$binDir = Join-Path $repoRoot 'build\bin\Debug'
if (-not (Test-Path $binDir)) {
    Write-Error "Bin dir not found: $binDir"
    exit 1
}

$tests = Get-ChildItem $binDir -Filter '*Tests.exe' | Sort-Object Name
if ($tests.Count -eq 0) {
    Write-Warning "No *Tests.exe found in $binDir"
    exit 0
}

$hadFail = $false
foreach ($t in $tests) {
    Write-Host "=== Running $($t.Name) ==="
    & $t.FullName --gtest_color=yes
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED: $($t.Name)"
        $hadFail = $true
        if ($StopOnFail) { break }
    }
}

if ($hadFail) { exit 1 } else { exit 0 }


