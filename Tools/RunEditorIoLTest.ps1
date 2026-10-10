param(
    [string]$Preset = "vs2022-x64-local",
    [string]$Configuration = "Debug",
    [string]$Suffix = "v8"
)

$logDir = "Logs"

if (-not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir | Out-Null
}

$env:GE_HOTRELOAD_IOL_TRACE_FILE = "$logDir/IoLTrace_FileSink_$Suffix.txt"
$env:GE_HOTRELOAD_VERBOSE = "1"
$env:GE_COREBRIDGE_LOGWRITER_TRACE_FILE = "$logDir/CoreBridgeLogWriterTrace_$Suffix.txt"

$repoRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $repoRoot
$editorDir = Join-Path $repoRoot "build\$Preset\bin\$Configuration\Apps\Editor"

if (-not (Test-Path $editorDir)) {
    throw "Editor output folder not found: $editorDir. Build first (e.g. .\Tools\Scripts\build.ps1 -Preset $Preset -Configuration $Configuration)."
}

Set-Location $editorDir

./Editor.exe -logfile "$logDir/IoLDebug_FileSink_$Suffix.txt"

