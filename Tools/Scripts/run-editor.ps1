param(
    [string]$Preset = "vs2026-x64-local",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Debug",
    [switch]$Configure,
    [switch]$Build,
    [switch]$NoRun,
    [string]$Project = "",
    [string]$LogFile = "",
    [Alias("UiReplay")]
    [string]$UiReplayScenario = "",
    [Alias("UiReplayLog")]
    [string]$UiReplayLogFile = "",
    [Alias("UiReplayFrames")]
    [UInt64]$ExitAfterFrames = 0,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$EditorArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    $here = $PSScriptRoot
    return (Resolve-Path (Join-Path $here "..\\..")).Path
}

$repoRoot = Get-RepoRoot
$buildDir = Join-Path $repoRoot ("build\\" + $Preset)
$exePath = Join-Path $buildDir ("bin\\" + $Config + "\\Apps\\Editor\\Editor.exe")
$exeDir = Split-Path -Parent $exePath
$defaultLog = Join-Path $exeDir "Logs\\Editor.log"

Write-Host ("RepoRoot : " + $repoRoot)
Write-Host ("Preset   : " + $Preset)
Write-Host ("Config   : " + $Config)
Write-Host ("Exe      : " + $exePath)
if ([string]::IsNullOrWhiteSpace($LogFile)) { $LogFile = $defaultLog }
Write-Host ("LogFile  : " + $LogFile)
if ([string]::IsNullOrWhiteSpace($Project)) { $Project = $repoRoot }
Write-Host ("Project  : " + $Project)

if ($Configure) {
    Write-Host "Configuring..."
    Push-Location $repoRoot
    try {
        cmake --preset $Preset
    }
    finally { Pop-Location }
}

if ($Build) {
    Write-Host "Building Editor..."
    Push-Location $repoRoot
    try {
        cmake --build --preset $Preset --config $Config --target Editor
    }
    finally { Pop-Location }
}

if (-not (Test-Path $exePath)) {
    throw ("Editor.exe not found at '" + $exePath + "'. Build it with: cmake --preset " + $Preset + "; cmake --build --preset " + $Preset + " --config " + $Config + " --target Editor")
}

if ($NoRun) {
    Write-Host "Build complete (NoRun requested)."
    exit 0
}

Write-Host ("Launching from: " + $exeDir)
Push-Location $exeDir
try {
    $logDir = Split-Path -Parent $LogFile
    if (-not [string]::IsNullOrWhiteSpace($logDir)) {
        New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    }

    # Convenience: allow passing UIReplay/exit flags as script params, but still
    # forward them to the Editor as command-line args.
    if (-not [string]::IsNullOrWhiteSpace($UiReplayScenario)) {
        $EditorArgs += @("--ui-replay", $UiReplayScenario)
        if (-not [string]::IsNullOrWhiteSpace($UiReplayLogFile)) {
            $EditorArgs += @("--ui-replay-log", $UiReplayLogFile)
        }
        if ($ExitAfterFrames -gt 0) {
            $EditorArgs += @("--exit-after-frames", $ExitAfterFrames.ToString())
        }
    } elseif ($ExitAfterFrames -gt 0) {
        # Also allow exit-after-frames for normal runs.
        $EditorArgs += @("--exit-after-frames", $ExitAfterFrames.ToString())
    }

    $hasProjectArg = $false
    foreach ($a in $EditorArgs) {
        if ($a -eq "--project" -or $a.StartsWith("--project=")) { $hasProjectArg = $true; break }
    }

    if ($hasProjectArg) {
        & $exePath "-logfile" $LogFile @EditorArgs
    } else {
        & $exePath "-logfile" $LogFile "--project" $Project @EditorArgs
    }
}
finally { Pop-Location }


