param(
	[switch] $DryRun,
	[string] $ClangFormatPath = "clang-format",
	[switch] $SkipCMake,
	[switch] $SkipDotNet
)

$ErrorActionPreference = "Stop"

# Resolve repository root: this script lives under Tools/Scripts, repo root is two levels up.
$repoRootInfo = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")
$repoRoot = $repoRootInfo.Path

Write-Host "== GameEngine code formatter ==" -ForegroundColor Cyan
Write-Host "Repo root: $repoRoot" -ForegroundColor DarkCyan

# Locate clang-format
if (Test-Path $ClangFormatPath) {
    $clangFormatExe = (Resolve-Path $ClangFormatPath).Path
} else {
    $clangCmd = Get-Command $ClangFormatPath -ErrorAction SilentlyContinue
    if (-not $clangCmd) {
        throw "clang-format not found. Install it and ensure it is on PATH, or pass -ClangFormatPath."
    }
    $clangFormatExe = $clangCmd.Path
}

Write-Host "Using clang-format: $clangFormatExe" -ForegroundColor DarkCyan

# Directories we treat as dependencies / generated and do NOT want to touch
# Mirrors .clang-tidy's HeaderFilterRegex and extends it slightly.
$skipPathPattern = '(?i)(\\third_party\\|\\external\\|\\extern\\|\\generated\\|\\build\\|\\bin\\|\\dependencies\\|\\vcpkg_installed\\|\\vcpkg\\|\\node_modules\\|\\dist\\)'

function Should-SkipPath {
    param(
        [string] $RelativePath
    )

    return $RelativePath -match $skipPathPattern
}

# File extensions to format with clang-format (C/C++/headers only)
$clangExtensions = @(
	".h", ".hh", ".hpp", ".hxx", ".inl",
	".c", ".cc", ".cpp", ".cxx"
)

$clangTargets = New-Object System.Collections.Generic.List[System.IO.FileInfo]

Write-Host "Scanning for C/C++/header files to format with clang-format..." -ForegroundColor Yellow

Get-ChildItem -Path $repoRoot -Recurse -File | ForEach-Object {
    $ext = $_.Extension.ToLowerInvariant()
    if ($clangExtensions -contains $ext) {
        $relative = $_.FullName.Substring($repoRoot.Length + 1)
        if (-not (Should-SkipPath -RelativePath $relative)) {
            $clangTargets.Add($_)
        }
    }
}

Write-Host ("Found {0} files for clang-format." -f $clangTargets.Count) -ForegroundColor Yellow

if ($DryRun) {
    Write-Host "Dry run mode: no files will be modified." -ForegroundColor Yellow
    foreach ($f in $clangTargets) {
        Write-Host ("[DRY RUN] clang-format would process: {0}" -f $f.FullName) -ForegroundColor DarkGray
    }
} else {
    foreach ($f in $clangTargets) {
        & $clangFormatExe -i -- "$($f.FullName)"
        if ($LASTEXITCODE -ne 0) {
            throw "clang-format failed on '$($f.FullName)' with exit code $LASTEXITCODE."
        }
    }
}


# Optional C# formatting with dotnet format (Managed projects)
if (-not $SkipDotNet) {
	$dotnetCmd = Get-Command "dotnet" -ErrorAction SilentlyContinue
	if (-not $dotnetCmd) {
		Write-Host "dotnet CLI not found on PATH; skipping C# dotnet-format step." -ForegroundColor Yellow
	} else {
		$managedDir = Join-Path $repoRoot "Managed"
		if (Test-Path $managedDir) {
			$csprojFiles = Get-ChildItem -Path $managedDir -Recurse -Filter *.csproj -File
			Write-Host ("Found {0} C# projects for dotnet format." -f $csprojFiles.Count) -ForegroundColor Yellow

			foreach ($proj in $csprojFiles) {
				if ($DryRun) {
						Write-Host ("[DRY RUN] dotnet format '{0}'" -f $proj.FullName) -ForegroundColor DarkGray
				} else {
						Write-Host ("Running: dotnet format '{0}'" -f $proj.FullName) -ForegroundColor DarkCyan
					& $dotnetCmd.Path "format" $proj.FullName
					if ($LASTEXITCODE -ne 0) {
						throw "dotnet format failed for '$($proj.FullName)' with exit code $LASTEXITCODE."
					}
				}
			}
		} else {
			Write-Host "Managed directory not found; skipping C# dotnet-format step." -ForegroundColor Yellow
		}
	}
}


# Optional CMake formatting (if cmake-format is available)
if (-not $SkipCMake) {
    $cmakeCmd = Get-Command "cmake-format" -ErrorAction SilentlyContinue
    if (-not $cmakeCmd) {
        Write-Host "cmake-format not found on PATH; skipping CMake files." -ForegroundColor Yellow
    } else {
        $cmakeExe = $cmakeCmd.Path
        Write-Host "Using cmake-format: $cmakeExe" -ForegroundColor DarkCyan

        $cmakeFiles = Get-ChildItem -Path $repoRoot -Recurse -File | Where-Object {
            $_.Name -eq "CMakeLists.txt" -or $_.Extension.ToLowerInvariant() -eq ".cmake"
        } | ForEach-Object {
            $relative = $_.FullName.Substring($repoRoot.Length + 1)
            if (-not (Should-SkipPath -RelativePath $relative)) { $_ }
        }

        Write-Host ("Found {0} CMake files for cmake-format." -f $cmakeFiles.Count) -ForegroundColor Yellow

        if ($DryRun) {
            foreach ($f in $cmakeFiles) {
                Write-Host ("[DRY RUN] cmake-format would process: {0}" -f $f.FullName) -ForegroundColor DarkGray
            }
        } else {
            foreach ($f in $cmakeFiles) {
                & $cmakeExe -i -- "$($f.FullName)"
                if ($LASTEXITCODE -ne 0) {
                    throw "cmake-format failed on '$($f.FullName)' with exit code $LASTEXITCODE."
                }
            }
        }
    }
}

Write-Host "Code formatting completed." -ForegroundColor Green

