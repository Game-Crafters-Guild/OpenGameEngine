# Stops the CompileServerHost that runs from the given assembly path, and no other process.
#
# Run by the host's build on Windows before it copies a fresh build into its output, after
# the build has asked that host to exit (--shutdown-running-from): this is the last resort
# for a host that did not answer. A running host holds the assembly it loaded open, so the
# copy fails while a host runs from the same file. Every host writes <Temp>/GE_CompileServer/<pipe>.pid (line 1 the
# process id, line 2 the full path of the assembly it runs from); a host whose file names
# HostPath is the only one stopped. Hosts running from another tree or from an editor's
# staged copy, and processes whose command line merely contains the path (shells, builds),
# are never touched.
param(
    [Parameter(Mandatory = $true)][string]$HostPath
)

$pidDir = Join-Path ([System.IO.Path]::GetTempPath()) 'GE_CompileServer'
if (-not (Test-Path -LiteralPath $pidDir)) { exit 0 }
$target = [System.IO.Path]::GetFullPath($HostPath)

foreach ($pidFile in Get-ChildItem -LiteralPath $pidDir -Filter '*.pid' -File) {
    $lines = @(Get-Content -LiteralPath $pidFile.FullName -ErrorAction SilentlyContinue)
    if ($lines.Count -lt 2) { continue }
    $hostId = 0
    if (-not [int]::TryParse($lines[0], [ref]$hostId)) { continue }
    try { $recordedPath = [System.IO.Path]::GetFullPath($lines[1]) } catch { continue }
    if (-not [string]::Equals($recordedPath, $target, [System.StringComparison]::OrdinalIgnoreCase)) { continue }

    $process = Get-CimInstance Win32_Process -Filter "ProcessId = $hostId" -ErrorAction SilentlyContinue
    if ($null -eq $process) { continue }
    # Hosts run under dotnet.exe; a process started after its pid file was written reuses a
    # dead host's id.
    if ($process.Name -ne 'dotnet.exe') { continue }
    if ($process.CreationDate -gt $pidFile.LastWriteTime) { continue }

    Write-Host "Stopping CompileServerHost pid $hostId running from $target"
    Stop-Process -Id $hostId -Force -ErrorAction SilentlyContinue
    Wait-Process -Id $hostId -Timeout 10 -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $pidFile.FullName -ErrorAction SilentlyContinue
}
exit 0
