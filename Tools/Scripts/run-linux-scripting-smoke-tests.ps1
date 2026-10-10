param(
	[string] $BuildDir = "build-linux-docker",
	[string] $Configuration = "Debug",
	[switch] $ForceRebuildImage
)

$ErrorActionPreference = "Stop"

# Resolve repository root: this script lives under Tools/Scripts, repo root is two levels up.
$repoRoot = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..");

Write-Host "== GameEngine Linux Docker scripting/compile-server smoke tests ==" -ForegroundColor Cyan
Write-Host "Repo root: $repoRoot" -ForegroundColor DarkCyan
Write-Host "Build dir (inside container): $BuildDir" -ForegroundColor DarkCyan
Write-Host "Configuration: $Configuration" -ForegroundColor DarkCyan

# Basic docker availability check
try {
    docker --version | Out-Null
} catch {
    Write-Error "Docker does not appear to be installed or on PATH. Please install Docker Desktop and try again."
    exit 1
}

	$imageTag = "gameengine-linux-dev"
	
	# Prepare log file for this run so we can inspect Docker/build/ctest output even
	# when running under restricted terminals (CI, editor integrations, etc.).
	$logDir = Join-Path $repoRoot $BuildDir
	New-Item -ItemType Directory -Path $logDir -Force | Out-Null
	$logFile = Join-Path $logDir "linux_scripting_smoke.log"
	"==== Linux scripting/compile-server smoke test run $(Get-Date -Format o) ====\n" | Out-File -FilePath $logFile -Encoding UTF8

	$buildImageExit = 0
	$dockerExitCode = 0

	# Decide whether we need to (re)build the Docker image, or can reuse an existing one.
	$needBuild = $ForceRebuildImage.IsPresent

	if (-not $needBuild) {
	    Write-Host "[1/3] Checking for existing Docker image '$imageTag'..." -ForegroundColor Yellow
	    $previousErrorActionPreference = $ErrorActionPreference
	    try {
	        $ErrorActionPreference = "SilentlyContinue"
	        docker image inspect $imageTag | Out-Null
	        if ($LASTEXITCODE -ne 0) {
	            $needBuild = $true
	            Write-Host "Docker image '$imageTag' not found. Building..." -ForegroundColor Yellow
	        } else {
	            Write-Host "Reusing existing Docker image '$imageTag'. Use -ForceRebuildImage to rebuild." -ForegroundColor Green
	            "Reusing existing Docker image '$imageTag' (no rebuild)." | Out-File -FilePath $logFile -Append
	        }
	    }
	    finally {
	        $ErrorActionPreference = $previousErrorActionPreference
	    }
	}

	if ($needBuild) {
	    Write-Host "[1/3] Building Docker image '$imageTag'..." -ForegroundColor Yellow

	    # Docker CLI sometimes writes progress or warnings to stderr even on success, which
	    # PowerShell treats as a NativeCommandError when $ErrorActionPreference = Stop.
	    # Temporarily relax error handling for the native docker invocation and rely on
	    # its actual exit code instead.
	    $previousErrorActionPreference = $ErrorActionPreference
	    try {
	        $ErrorActionPreference = "Continue"
	        docker build -f (Join-Path $repoRoot "Tools/Docker/Dockerfile.linux-dev") -t $imageTag $repoRoot 2>&1 |
	            Tee-Object -FilePath $logFile -Append
	        $buildImageExit = $LASTEXITCODE
	    }
	    finally {
	        $ErrorActionPreference = $previousErrorActionPreference
	    }
	}

	if ($buildImageExit -ne 0) {
	    Write-Error "Docker image build failed with exit code $buildImageExit. See $logFile for details."
	    $dockerExitCode = $buildImageExit
	} else {

	Write-Host "[2/3] Configuring and building smoke test targets inside container..." -ForegroundColor Yellow

		$bashCommand = @"
cmake -S . -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=$Configuration -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=ON -DENABLE_SCRIPTING=ON &&
cmake --build $BuildDir --target EngineCoreClrSmokeTests EngineCompileServerSmokeTests -- -j4 &&
cd $BuildDir &&
ctest -R EngineCoreClrSmokeTests --output-on-failure &&
ctest -R EngineCompileServerSmokeTests --output-on-failure &&
cd /src &&
dotnet test Managed/InteropTests.IoL/InteropTests.IoL.csproj -c $Configuration
"@
    
    Write-Host "[3/3] Running Linux scripting/compile-server smoke tests inside container..." -ForegroundColor Yellow
    
    # Bind-mount the repo at /src and run the bash command via bash -lc so PATH and env are initialized.
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        docker run --rm `
            -v "${repoRoot}:/src" `
            -w "/src" `
            $imageTag `
            bash -lc "$bashCommand" 2>&1 |
            Tee-Object -FilePath $logFile -Append
        
        $dockerExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
}

# Persist the last docker-run (or image-build) exit code so automated tooling
# (including CI and editor integrations) can inspect the result without having
# to parse PowerShell or Docker output.
$resultFile = Join-Path $repoRoot "build-linux-docker/last_linux_scripting_smoke_exitcode.txt"
New-Item -ItemType Directory -Path (Split-Path $resultFile) -Force | Out-Null
Set-Content -Path $resultFile -Value $dockerExitCode

if ($dockerExitCode -ne 0) {
    Write-Error "Linux scripting/compile-server smoke tests failed with exit code $dockerExitCode. See $logFile for details."
    exit $dockerExitCode
}

Write-Host "Linux scripting/compile-server smoke tests completed successfully." -ForegroundColor Green

