param(
    [string] $ImageTag = "gameengine-linux-dev",
    [switch] $ForceRebuildImage
)

$ErrorActionPreference = "Stop"

# Resolve repository root: this script lives under Tools/Scripts, repo root is two levels up.
$repoRoot = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..");

Write-Host "== GameEngine Linux Docker dev shell ==" -ForegroundColor Cyan
Write-Host "Repo root: $repoRoot" -ForegroundColor DarkCyan
Write-Host "Image tag: $ImageTag" -ForegroundColor DarkCyan

# Basic docker availability check
try {
    docker --version | Out-Null
} catch {
    Write-Error "Docker does not appear to be installed or on PATH. Please install Docker Desktop and try again."
    exit 1
}

$buildImageExit = 0
$needBuild = $ForceRebuildImage.IsPresent

if (-not $needBuild) {
    Write-Host "[1/2] Checking for existing Docker image '$ImageTag'..." -ForegroundColor Yellow
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "SilentlyContinue"
        docker image inspect $ImageTag | Out-Null
        if ($LASTEXITCODE -ne 0) {
            $needBuild = $true
            Write-Host "Docker image '$ImageTag' not found. Building..." -ForegroundColor Yellow
        } else {
            Write-Host "Reusing existing Docker image '$ImageTag'. Use -ForceRebuildImage to rebuild." -ForegroundColor Green
        }
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
}

if ($needBuild) {
    Write-Host "[1/2] Building Docker image '$ImageTag'..." -ForegroundColor Yellow

    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        docker build -f (Join-Path $repoRoot "Tools/Docker/Dockerfile.linux-dev") -t $ImageTag $repoRoot
        $buildImageExit = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    if ($buildImageExit -ne 0) {
        Write-Error "Docker image build failed with exit code $buildImageExit."
        exit $buildImageExit
    }
}

Write-Host "[2/2] Starting interactive Linux dev shell inside container..." -ForegroundColor Yellow
Write-Host "(Mounting '$repoRoot' at /src inside the container)" -ForegroundColor DarkYellow

# Run an interactive bash shell with the repo mounted at /src
$previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        docker run -it --rm `
            -v "${repoRoot}:/src" `
            -w "/src" `
            $ImageTag `
            bash
    }
finally {
    $ErrorActionPreference = $previousErrorActionPreference
}

exit $LASTEXITCODE

