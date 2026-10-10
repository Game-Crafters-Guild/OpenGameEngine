# Fetch prebuilt vcpkg binary cache from the repo's GitHub Release into the
# per-user cache the presets consult first (they set VCPKG_BINARY_SOURCES to
# clear;default,readwrite plus any sources the machine environment adds):
# $env:VCPKG_DEFAULT_BINARY_CACHE if set, else %LOCALAPPDATA%\vcpkg\archives.
# Shared by every clone and worktree on the machine.
#
# Requires GitHub CLI (gh) and authentication (private repo):
#   gh auth login

param(
    [string]$Triplet,
    [string]$Tag = "vcpkg-cache",
    [string]$Repo,
    [switch]$Force,
    [switch]$Help
)

if ($Help) {
    Write-Host "Fetch vcpkg binary cache from GitHub Releases" 
    Write-Host "Usage: .\Tools\Scripts\fetch-vcpkg-cache.ps1 [-Triplet <triplet>] [-Tag <tag>] [-Repo <owner/repo>] [-Force]"
    Write-Host "" 
    Write-Host "Examples:"
    Write-Host "  .\Tools\Scripts\fetch-vcpkg-cache.ps1                      # auto-detect triplet"
    Write-Host "  .\Tools\Scripts\fetch-vcpkg-cache.ps1 -Triplet x64-windows"
    exit 0
}

. (Join-Path $PSScriptRoot "vcpkg-cache-common.ps1")

if (-not $Triplet) {
    $Triplet = Get-VcpkgHostTriplet
}

if (-not $Repo) {
    $Repo = Get-VcpkgCacheRepo
}

if (-not $Repo) {
    throw "Unable to determine GitHub repo. Pass -Repo owner/repo or ensure git origin remote points to GitHub."
}

Assert-GitHubCli

$zipName = "vcpkg-archives-$Triplet.zip"
$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("ge-vcpkg-cache-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null

try {
    & gh release download $Tag --repo $Repo --pattern $zipName --dir $tempDir
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to download '$zipName' from release tag '$Tag' in '$Repo'."
    }

    $zipPath = Join-Path $tempDir $zipName
    if (-not (Test-Path $zipPath)) {
        throw "Downloaded file not found: $zipPath"
    }

    $destDir = if ($env:VCPKG_DEFAULT_BINARY_CACHE) {
        $env:VCPKG_DEFAULT_BINARY_CACHE
    } else {
        Join-Path $env:LOCALAPPDATA "vcpkg\archives"
    }
    New-Item -ItemType Directory -Force -Path $destDir | Out-Null

    if ($Force) {
        Expand-Archive -Path $zipPath -DestinationPath $destDir -Force
    } else {
        Expand-Archive -Path $zipPath -DestinationPath $destDir
    }

    Write-Host "Installed vcpkg cache for '$Triplet' into $destDir"
}
finally {
    Remove-Item -Recurse -Force -Path $tempDir -ErrorAction SilentlyContinue
}
