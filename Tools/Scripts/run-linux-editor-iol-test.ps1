param(
    [string] $BuildDir = "build-linux-docker",
    [string] $Configuration = "Debug",
    [switch] $ForceRebuildImage
)

$ErrorActionPreference = "Stop"

# Resolve repository root: this script lives under Tools/Scripts, repo root is two levels up.
$repoRoot = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")

Write-Host "== GameEngine Linux Docker Editor IoL test ==" -ForegroundColor Cyan
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

# Prepare log file to capture Docker/build/Editor output
$logDir = Join-Path $repoRoot $BuildDir
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$logFile = Join-Path $logDir "linux_editor_iol.log"
"==== Linux Editor IoL test run $(Get-Date -Format o) ====\n" | Out-File -FilePath $logFile -Encoding UTF8

# Canonical Editor logfile + IoL trace paths on the host. Remove any stale
# files up front so each run validates fresh output from the container.
$editorLogHostPath = Join-Path $repoRoot "Logs/Editor-Linux-IoL.log"
$iolTraceHostPath = Join-Path $repoRoot "Logs/Editor-Linux-IoL-ioltrace.log"
if (Test-Path $editorLogHostPath) { Remove-Item $editorLogHostPath -Force -ErrorAction SilentlyContinue }
if (Test-Path $iolTraceHostPath) { Remove-Item $iolTraceHostPath -Force -ErrorAction SilentlyContinue }

# On Windows hosts, proactively stop any running CompileServerHost instances that
# might be holding GameEngine.CompileServerHost.dll open on the shared volume.
# Otherwise, the Linux container's dotnet build can fail with MSB3021 "Access to
# the path ... is denied" when copying to bin/Debug/net10.0/.
if ($env:OS -eq "Windows_NT") {
    Write-Host "Pre-flight: stopping Windows CompileServerHost processes before Docker build..." -ForegroundColor Yellow
    try {
        $procs = Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*GameEngine.CompileServerHost.dll*' }
        if ($procs) {
            foreach ($p in $procs) {
                try {
                    Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
                } catch {}
            }
            "Stopped CompileServerHost processes on Windows host before Linux IoL build." | Out-File -FilePath $logFile -Append -Encoding UTF8
        }
        else {
            "No CompileServerHost processes were running on Windows host before Linux IoL build." | Out-File -FilePath $logFile -Append -Encoding UTF8
        }
    }
    catch {
        Write-Warning "Failed to query or stop CompileServerHost processes on Windows host: $_"
        "Warning: failed to query/stop CompileServerHost processes on Windows host: $_" | Out-File -FilePath $logFile -Append -Encoding UTF8
    }
}

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

    # Docker CLI sometimes writes progress or warnings to stderr even on success.
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
    # First container run: configure + build Editor and stage TestInit.cs into the
    # Editor's Assets folder. We keep this separate from the actual Editor IoL
    # run so that we can more closely mirror the manually-verified Editor
    # invocation that we know produces the correct logs.
    Write-Host "[2/3] Configuring and building Editor target inside container..." -ForegroundColor Yellow

    # NOTE: Use a single-quoted here-string to avoid PowerShell variable
    # interpolation inside the bash script. We substitute BuildDir and
    # Configuration via simple string replacement so that bash sees any
    # shell variables intact.
    $buildCommandTemplate = @'
cmake -S . -B __BUILDDIR__ -G Ninja -DCMAKE_BUILD_TYPE=__CONFIG__ -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=ON -DENABLE_SCRIPTING=ON -DGE_SKIP_VCPKG_VERIFY=ON &&
cmake --build __BUILDDIR__ --target Editor -- -j4 &&
mkdir -p __BUILDDIR__/bin/__CONFIG__/Assets &&
cp /src/Assets/TestInit.cs __BUILDDIR__/bin/__CONFIG__/Assets/TestInit.cs
'@

    $buildCommand = $buildCommandTemplate.Replace("__BUILDDIR__", $BuildDir).Replace("__CONFIG__", $Configuration)

    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        docker run --rm `
            -v "${repoRoot}:/src" `
            -w "/src" `
            $imageTag `
            bash -lc "$buildCommand" 2>&1 |
            Tee-Object -FilePath $logFile -Append

        $dockerExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    if ($dockerExitCode -ne 0) {
        Write-Error "Linux Editor IoL test: Docker build/container step failed with exit code $dockerExitCode. See $logFile for details."
    } else {
        # Second container run: mirror the manually-verified Editor invocation as
        # closely as possible. We rely on the Editor's own GLFW failure to exit
        # the process; we do not background/kill the Editor here, which keeps the
        # control flow simple and makes logfile creation easier to reason about.
        Write-Host "[3/3] Running Linux Editor IoL test inside container..." -ForegroundColor Yellow

        $editorCommand = @'
mkdir -p /src/Logs &&
cd __BUILDDIR__/bin/__CONFIG__ &&
./Editor -logfile /src/Logs/Editor-Linux-IoL.log || echo "EDITOR_EXIT:$?"
'@

        $editorCommand = $editorCommand.Replace("__BUILDDIR__", $BuildDir).Replace("__CONFIG__", $Configuration)

        $previousErrorActionPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = "Continue"
            docker run --rm `
                -v "${repoRoot}:/src" `
                -w "/src" `
                -e "GE_HOTRELOAD_IOL_TRACE_FILE=/src/Logs/Editor-Linux-IoL-ioltrace.log" `
                $imageTag `
                bash -lc "$editorCommand" 2>&1 |
                Tee-Object -FilePath $logFile -Append

            $dockerExitCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
    }
}

    # Persist exit code for CI/editor tooling.
    $resultFile = Join-Path $repoRoot "build-linux-docker/last_linux_editor_iol_exitcode.txt"
    New-Item -ItemType Directory -Path (Split-Path $resultFile) -Force | Out-Null
    Set-Content -Path $resultFile -Value $dockerExitCode

    # If the Editor created its own logfile inside the container (mapped to /src/Logs),
    # splice a copy of it into the main IoL run log and validate that scripts actually
    # loaded and InitializeOnLoad methods ran. If scripts cannot be loaded or IoL does
    # not fire, this test should fail.
    if (Test-Path $editorLogHostPath) {
        "`n==== Begin Editor-Linux-IoL.log (copied from Logs/Editor-Linux-IoL.log) ==== `n" |
            Out-File -FilePath $logFile -Append -Encoding UTF8
        $editorLogLines = Get-Content $editorLogHostPath
        $editorLogLines |
            Out-File -FilePath $logFile -Append -Encoding UTF8

        # Treat any script assembly load failures as a hard failure for this IoL test.
        $scriptLoadErrors = $editorLogLines | Where-Object {
            $_ -like "*Failed to preload/swap script assembly via CoreBridge*" -or
            $_ -like "*Failed to load Scripts assembly after initial build*" -or
            $_ -like "*PreloadAndSwapFromPath not implemented for this platform*"
        }

        # Require the InitializeOnLoad Boot() marker from Assets/TestInit.cs to appear
        # in the Editor log, otherwise IoL is considered broken on this platform.
        # NOTE: Use a simple substring match without wildcard character-classes so
        # that literal "[Test]" in the log message does not interfere with matching.
        $initializeOnLoadMarkers = $editorLogLines | Where-Object {
            $_ -like "*InitializeOnLoad Boot() called*"
        }

        if ($scriptLoadErrors.Count -gt 0) {
            Write-Error "Linux Editor IoL test: script assembly failed to load. See Logs/Editor-Linux-IoL.log."
            $dockerExitCode = 1
        }
        elseif ($initializeOnLoadMarkers.Count -eq 0) {
            Write-Error "Linux Editor IoL test: InitializeOnLoad Boot() marker '[Test] InitializeOnLoad Boot() called' not found in Editor log."
            $dockerExitCode = 1
        }

        # Additionally, require IoL trace data when GE_HOTRELOAD_IOL_TRACE_FILE is set. This
        # verifies that HotReloadManager actually scheduled and executed InitializeOnLoad
        # methods even if Console redirection were misconfigured.
        if (Test-Path $iolTraceHostPath) {
            "`n==== Begin Editor-Linux-IoL-ioltrace.log (IoL trace) ==== `n" |
                Out-File -FilePath $logFile -Append -Encoding UTF8
            $iolTraceLines = Get-Content $iolTraceHostPath
            $iolTraceLines |
                Out-File -FilePath $logFile -Append -Encoding UTF8

            $iolInvokeLines = $iolTraceLines | Where-Object {
                $_ -like "*[IoL]*Invoke*" -or $_ -like "*[IoL] Methods:*"
            }

            if ($iolInvokeLines.Count -eq 0) {
                Write-Error "Linux Editor IoL test: IoL trace file did not record any InitializeOnLoad activity; IoL likely did not run."
                $dockerExitCode = 1
            }
        }
        else {
            # When GE_HOTRELOAD_IOL_TRACE_FILE is set, the HotReloadManager should always
            # emit IoL trace data during InitializeOnLoad discovery/invocation. If the
            # trace file is missing entirely, treat this as a hard failure rather than
            # a best-effort warning.
            Write-Error "Linux Editor IoL test: IoL trace file Logs/Editor-Linux-IoL-ioltrace.log was not created; HotReload IoL trace did not run."
            $dockerExitCode = 1
        }
    }
    else {
        Write-Error "Linux Editor IoL test: Editor logfile Logs/Editor-Linux-IoL.log was not created."
        $dockerExitCode = 1
    }

    if ($dockerExitCode -ne 0) {
        Write-Error "Linux Editor IoL test failed with exit code $dockerExitCode. See $logFile and Logs/Editor-Linux-IoL.log for details."
        exit $dockerExitCode
    }

    Write-Host "Linux Editor IoL test completed successfully." -ForegroundColor Green
    Write-Host "Editor logfile: Logs/Editor-Linux-IoL.log" -ForegroundColor Green

