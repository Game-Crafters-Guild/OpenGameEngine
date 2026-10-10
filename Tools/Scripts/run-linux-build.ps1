param(
    [string] $BuildDir = "build-linux-docker",
    [string] $Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

# Resolve repository root: this script lives under Tools/Scripts, repo root is two levels up.
$repoRoot = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..");

Write-Host "== GameEngine Linux Docker build ==" -ForegroundColor Cyan
Write-Host "Repo root: $repoRoot" -ForegroundColor DarkCyan

# Basic docker availability check
try {
    docker --version | Out-Null
} catch {
    Write-Error "Docker does not appear to be installed or on PATH. Please install Docker Desktop and try again."
    exit 1
}

$imageTag = "gameengine-linux-dev"

Write-Host "[1/2] Building Docker image '$imageTag'..." -ForegroundColor Yellow
docker build -f (Join-Path $repoRoot "Tools/Docker/Dockerfile.linux-dev") -t $imageTag $repoRoot

Write-Host "[2/2] Running Linux build inside container..." -ForegroundColor Yellow

$bashCommand = @"
cmake -S . -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=$Configuration -DBUILD_EXAMPLES=ON -DBUILD_TESTING=ON -DENABLE_SCRIPTING=OFF &&
cmake --build $BuildDir --target Editor -- -j4
"@

docker run --rm `
    -v "${repoRoot}:/src" `
    -w "/src" `
    $imageTag `
    bash -lc "$bashCommand"

Write-Host "Linux Docker build completed successfully." -ForegroundColor Green
