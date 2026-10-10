# Rebuild and publish the prebuilt vcpkg binary cache that fetch-vcpkg-cache.ps1
# hands to a fresh clone.
#
# vcpkg restores an archive only when its ABI hash matches, and that hash covers
# the registry baseline, the pinned vcpkg checkout and tools, the triplet files,
# the overlay ports and the compiler toolset. Change any of those and the
# published asset stops restoring: the configure silently compiles every port
# from source instead. This script republishes the asset for the set the current
# tree actually resolved.
#
# The archive set comes from the install root a configure already produced --
# vcpkg_installed/vcpkg/status lists one Abi: line per installed package, and
# that is exactly what this baseline needs and nothing else. The per-user cache
# accumulates every baseline the machine has ever used, so it is read by hash,
# never swept.
#
# Requires GitHub CLI (gh) and authentication (private repo):
#   gh auth login
#
# Usage:
#   cmake --preset vs2026-x64-local            # produces the install root
#   .\Tools\Scripts\refresh-vcpkg-cache.ps1 -Publish

param(
    [string]$BuildDir,
    [string]$Triplet,
    [string]$CacheDir,
    [string]$OutFile,
    [string]$Tag = "vcpkg-cache",
    [string]$Repo,
    [switch]$Publish,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

if ($Help) {
    Write-Host "Rebuild (and optionally publish) the vcpkg binary cache release asset"
    Write-Host "Usage: .\Tools\Scripts\refresh-vcpkg-cache.ps1 [-BuildDir <dir>] [-Triplet <triplet>]"
    Write-Host "                                              [-CacheDir <dir>] [-OutFile <zip>]"
    Write-Host "                                              [-Tag <tag>] [-Repo <owner/repo>] [-Publish]"
    Write-Host ""
    Write-Host "  -BuildDir  Configured build directory holding vcpkg_installed."
    Write-Host "             Default: build/vs2026-x64-local under the repo root."
    Write-Host "  -CacheDir  vcpkg binary cache to collect archives from."
    Write-Host "             Default: VCPKG_DEFAULT_BINARY_CACHE, else the per-user cache."
    Write-Host "  -Publish   Upload the zip to the release and rewrite the release notes."
    Write-Host "             Without it the zip is written and described, and nothing is published."
    exit 0
}

. (Join-Path $PSScriptRoot "vcpkg-cache-common.ps1")

$repoRoot = (git -C $PSScriptRoot rev-parse --show-toplevel 2>$null)
if (-not $repoRoot) {
    throw "Not inside a git checkout: $PSScriptRoot"
}

if (-not $Triplet) { $Triplet = Get-VcpkgHostTriplet }

if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot "build/vs2026-x64-local" }

if (-not $CacheDir) {
    $CacheDir = if ($env:VCPKG_DEFAULT_BINARY_CACHE) {
        $env:VCPKG_DEFAULT_BINARY_CACHE
    } else {
        Join-Path $env:LOCALAPPDATA "vcpkg\archives"
    }
}

$zipName = "vcpkg-archives-$Triplet.zip"
if (-not $OutFile) { $OutFile = Join-Path (Get-Location) $zipName }

if ([System.IO.Path]::GetFileName($OutFile) -ne $zipName) {
    throw "fetch-vcpkg-cache.ps1 downloads the asset by name, so the file must be called '$zipName' (got '$OutFile')."
}

# --- The install root the archive set is read out of -------------------------

$installedDir = Join-Path $BuildDir "vcpkg_installed"
$statusFile = Join-Path $installedDir "vcpkg/status"
if (-not (Test-Path $statusFile)) {
    throw @"
No vcpkg install root at $installedDir.
Configure the tree first, so vcpkg resolves the manifest at the current baseline:
    cmake --preset vs2026-x64-local
"@
}

# vcpkg records which manifest it installed from. A build directory belonging to
# another checkout resolves a different tree's manifest, overlay ports and pin,
# and publishing its archive set would ship a cache no clone of this tree can use.
$infoFile = Join-Path $installedDir "vcpkg/manifest-info.json"
if (-not (Test-Path $infoFile)) {
    throw "$installedDir records no manifest-info.json, so the tree that produced it is unknown. Reconfigure."
}
$installedManifest = (Get-Content $infoFile -Raw | ConvertFrom-Json).'manifest-path'
$expectedManifest = Join-Path $repoRoot "vcpkg.json"
if ([System.IO.Path]::GetFullPath($installedManifest) -ne [System.IO.Path]::GetFullPath($expectedManifest)) {
    throw @"
$installedDir was installed from $installedManifest,
not this tree's $expectedManifest. Point -BuildDir at a build directory of this checkout.
"@
}

# --- The archive set ---------------------------------------------------------

# Deb822 stanzas separated by blank lines. Feature stanzas carry no Abi: line;
# one package stanza per installed package does.
$packages = @()
$stanza = @{}
foreach ($line in ((Get-Content $statusFile) + "")) {
    if ($line -match '^(?<key>[A-Za-z-]+):\s*(?<value>.*)$') {
        $stanza[$Matches.key] = $Matches.value
        continue
    }
    if ($line.Trim() -eq "") {
        if ($stanza.ContainsKey("Abi") -and $stanza["Architecture"] -eq $Triplet) {
            $packages += [pscustomobject]@{
                Name = $stanza["Package"]
                Version = $stanza["Version"]
                Abi = $stanza["Abi"]
            }
        }
        $stanza = @{}
    }
}

if ($packages.Count -eq 0) {
    throw "$statusFile lists no installed $Triplet package with an ABI hash. Wrong -Triplet, or the install did not complete."
}

$missing = @()
foreach ($package in $packages) {
    $archive = Join-Path $CacheDir (Join-Path $package.Abi.Substring(0, 2) "$($package.Abi).zip")
    if (-not (Test-Path $archive)) { $missing += "$($package.Name):$Triplet -> $archive" }
    $package | Add-Member -NotePropertyName Archive -NotePropertyValue $archive
}

if ($missing.Count -gt 0) {
    throw @"
$($missing.Count) of $($packages.Count) packages have no archive in $CacheDir :
$($missing -join "`n")
The cache holds an archive only for a package vcpkg built or restored with writes enabled.
Reconfigure with the default binary source (VCPKG_BINARY_SOURCES unset, or 'clear;default,readwrite')
so every port lands in the cache, then run this again.
"@
}

# --- Stage and zip -----------------------------------------------------------

$stagingDir = Join-Path ([System.IO.Path]::GetTempPath()) ("ge-vcpkg-refresh-" + [System.Guid]::NewGuid().ToString("N"))

try {
    New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null

    # The layout the fetch script expands into the cache root: the two-hex-digit
    # shard directories sit at the zip root, not under a wrapper directory.
    foreach ($package in $packages) {
        $shard = Join-Path $stagingDir $package.Abi.Substring(0, 2)
        New-Item -ItemType Directory -Force -Path $shard | Out-Null
        Copy-Item -Path $package.Archive -Destination $shard
    }

    $rawBytes = (Get-ChildItem $stagingDir -Recurse -File | Measure-Object -Property Length -Sum).Sum

    if (Test-Path $OutFile) { Remove-Item $OutFile -Force }
    Write-Host "Compressing $($packages.Count) archives ($([math]::Round($rawBytes / 1MB)) MB) into $OutFile ..."
    Compress-Archive -Path (Join-Path $stagingDir "*") -DestinationPath $OutFile
}
finally {
    Remove-Item -Recurse -Force -Path $stagingDir -ErrorAction SilentlyContinue
}

$zipBytes = (Get-Item $OutFile).Length
$sha256 = (Get-FileHash -Path $OutFile -Algorithm SHA256).Hash.ToLowerInvariant()

# --- What the asset was built from -------------------------------------------

$pinFile = Join-Path $repoRoot "cmake/VcpkgPin.cmake"
$pinMatch = Select-String -Path $pinFile -Pattern 'set\(GE_VCPKG_COMMIT "([0-9a-f]{40})"\)'
if (-not $pinMatch) { throw "No GE_VCPKG_COMMIT in $pinFile." }
$vcpkgCommit = $pinMatch.Matches[0].Groups[1].Value

$configuration = Get-Content (Join-Path $repoRoot "vcpkg-configuration.json") -Raw | ConvertFrom-Json
$baseline = $configuration.'default-registry'.baseline

$date = (Get-Date).ToString("yyyy-MM-dd")

Write-Host ""
Write-Host "Asset:             $OutFile"
Write-Host "Triplet:           $Triplet"
Write-Host "Packages:          $($packages.Count)"
Write-Host "Uncompressed:      $rawBytes bytes"
Write-Host "Compressed:        $zipBytes bytes"
Write-Host "SHA-256:           $sha256"
Write-Host "vcpkg commit:      $vcpkgCommit"
Write-Host "Registry baseline: $baseline"
Write-Host ""

if (-not $Publish) {
    Write-Host "Not published (-Publish was not passed)."
    exit 0
}

# --- Publish -----------------------------------------------------------------

if (-not $Repo) { $Repo = Get-VcpkgCacheRepo }
if (-not $Repo) {
    throw "Unable to determine GitHub repo. Pass -Repo owner/repo or ensure git origin remote points to GitHub."
}

Assert-GitHubCli

& gh release upload $Tag $OutFile --repo $Repo --clobber
if ($LASTEXITCODE -ne 0) { throw "Failed to upload '$zipName' to release tag '$Tag' in '$Repo'." }

$notes = @"
Prebuilt vcpkg binary cache for this repository's dependency manifest.

## What this is

Each ``vcpkg-archives-<triplet>.zip`` contains vcpkg binary-cache archives (``<2-char-prefix>/<abi-hash>.zip`` layout) for every port the current ``vcpkg.json`` manifest installs for that triplet. With these in the per-user binary cache, a fresh configure restores all third-party dependencies in seconds instead of compiling them from source.

Currently published:

| | |
|---|---|
| Triplet | ``$Triplet`` |
| Packages | $($packages.Count) |
| Asset size | $zipBytes bytes |
| SHA-256 | ``$sha256`` |
| vcpkg commit (``cmake/VcpkgPin.cmake``) | ``$vcpkgCommit`` |
| Registry baseline (``vcpkg-configuration.json``) | ``$baseline`` |
| Refreshed | $date |

## How to use it

From a repo checkout, before the first configure:

- Windows: ``.\Tools\Scripts\fetch-vcpkg-cache.ps1``
- macOS/Linux: ``./Tools/Scripts/fetch-vcpkg-cache.sh``

The script downloads the asset matching the host triplet (override with ``-Triplet`` / ``--triplet``) and extracts it into the per-user cache that the CMake presets consult: ``%LOCALAPPDATA%\vcpkg\archives`` on Windows, ``~/.cache/vcpkg/archives`` elsewhere, ``VCPKG_DEFAULT_BINARY_CACHE`` if set. Requires an authenticated GitHub CLI (``gh auth login``).

A configure that finds a cold cache runs this for itself, so a fresh clone normally needs no separate step. Opt out with ``GE_NO_CACHE_FETCH=1``.

## ABI caveat

vcpkg restores an archive only when the ABI hash matches, and that hash covers the registry baseline, the pinned vcpkg checkout and its ABI-sensitive tools, the triplet files, the overlay ports under ``cmake/ports`` (and, for the macOS archives, under ``cmake/macos-ports``), and the compiler toolset version. On a mismatch the configure silently falls back to building the affected ports from source; nothing breaks, the cache just misses. MSVC toolset updates are the most common cause of misses on Windows.

## When to re-publish

Re-publish whenever a fresh configure stops restoring all ports from this asset -- typically after a change to ``vcpkg.json``, the ``vcpkg-configuration.json`` baseline, ``cmake/VcpkgPin.cmake``, an overlay port or a triplet file, or after a compiler toolset upgrade.

Configure a tree so vcpkg resolves the manifest at the current baseline, then:

``````
.\Tools\Scripts\refresh-vcpkg-cache.ps1 -Publish
``````

It reads the archive set out of that tree's ``vcpkg_installed/vcpkg/status``, collects those archives from the per-user cache, zips them in the layout the fetch script expands, uploads the asset and rewrites these notes.
"@

$notesFile = Join-Path ([System.IO.Path]::GetTempPath()) ("ge-vcpkg-notes-" + [System.Guid]::NewGuid().ToString("N") + ".md")
try {
    Set-Content -Path $notesFile -Value $notes -Encoding utf8NoBOM
    & gh release edit $Tag --repo $Repo --notes-file $notesFile
    if ($LASTEXITCODE -ne 0) { throw "Uploaded the asset but failed to update the notes of release '$Tag'." }
}
finally {
    Remove-Item -Force -Path $notesFile -ErrorAction SilentlyContinue
}

Write-Host "Published $zipName to release '$Tag' in $Repo."
