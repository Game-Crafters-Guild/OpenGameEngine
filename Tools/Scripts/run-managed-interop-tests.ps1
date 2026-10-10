param(
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

# Resolve repo root from this script's location (Tools/Scripts)
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$repoRoot = Resolve-Path (Join-Path $scriptDir "..\..")
$buildDir = Join-Path $repoRoot "build"
$binDir = Join-Path $buildDir "bin\$Configuration"

Write-Host "Using repo root: $repoRoot" -ForegroundColor Cyan
Write-Host "Using build bin directory: $binDir" -ForegroundColor Cyan

if (-not (Test-Path $binDir)) {
    throw "Build bin directory '$binDir' does not exist. Run CMake configure/build first."
}

$interopTestsCsproj = Join-Path $repoRoot "Managed\InteropTests\InteropTests.csproj"
if (-not (Test-Path $interopTestsCsproj)) {
    throw "InteropTests project not found at '$interopTestsCsproj'."
}

$prevGeNativeDir = [Environment]::GetEnvironmentVariable("GE_NATIVE_DIR", "Process")
try {
    [Environment]::SetEnvironmentVariable("GE_NATIVE_DIR", $binDir, "Process")
    Write-Host "GE_NATIVE_DIR set to '$binDir' for this test run." -ForegroundColor Yellow

    dotnet test $interopTestsCsproj -c $Configuration
}
finally {
    [Environment]::SetEnvironmentVariable("GE_NATIVE_DIR", $prevGeNativeDir, "Process")
    Write-Host "GE_NATIVE_DIR restored to previous value." -ForegroundColor Yellow
}

