# Shared helpers for the vcpkg binary-cache release scripts:
# fetch-vcpkg-cache.ps1 (consumes the release) and refresh-vcpkg-cache.ps1
# (produces it). Dot-source it; it defines functions and runs nothing.

function Get-VcpkgHostTriplet {
    $arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()

    $archPart = switch ($arch) {
        "x64" { "x64" }
        "arm64" { "arm64" }
        default { throw "Unsupported architecture: $arch" }
    }

    # Test platforms inline: $isWindows/$isLinux/$isMacOS locals would collide
    # with pwsh's read-only automatic variables of the same names.
    $runtime = [System.Runtime.InteropServices.RuntimeInformation]
    if ($runtime::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)) { return "$archPart-windows" }
    if ($runtime::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Linux)) { return "$archPart-linux" }
    if ($runtime::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::OSX)) { return "$archPart-osx" }

    throw "Unsupported OS: $($runtime::OSDescription)"
}

function Get-VcpkgCacheRepo {
    $originUrl = (git remote get-url origin 2>$null)
    if (-not $originUrl) { return $null }

    # https://github.com/OWNER/REPO(.git)
    if ($originUrl -match '^https://github\.com/(?<owner>[^/]+)/(?<repo>[^/]+?)(\.git)?$') {
        return "$($Matches.owner)/$($Matches.repo)"
    }

    # git@github.com:OWNER/REPO(.git)
    if ($originUrl -match '^git@github\.com:(?<owner>[^/]+)/(?<repo>[^/]+?)(\.git)?$') {
        return "$($Matches.owner)/$($Matches.repo)"
    }

    return $null
}

# Throws unless the GitHub CLI is installed and holds a usable login. Both
# scripts talk to a release in a private repository, so neither can proceed
# without one.
function Assert-GitHubCli {
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        throw "GitHub CLI (gh) not found. Install it and run 'gh auth login'."
    }

    & gh auth status 1>$null 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "Not authenticated with GitHub CLI. Run: gh auth login"
    }
}
