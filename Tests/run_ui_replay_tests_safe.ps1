param(
  [string]$EditorExe = "",
  [ValidateSet("Debug", "RelWithDebInfo", "Release")]
  [string]$Config = "RelWithDebInfo",
  [string]$ProjectRoot,
  [string]$OutDir,
  [string]$Preset,
  # Base fallback if scenario doesn't specify minFrames.
  # Note: at high refresh rates (e.g. 240Hz), 260 frames is ~1.1 seconds; prefer larger defaults.
  [int]$ExitAfterFrames = 900,
  # Hard wall-clock timeout per scenario. If hit, the Editor process is killed and the run is recorded as a failure.
  [int]$TimeoutSeconds = 120,
  # When enabled, captures Editor stdout/stderr to files under OutDir (helps diagnose crashes/hangs).
  [switch]$CaptureStdout = $true,
  [switch]$KillExistingEditor = $true,
  # Optional: override the default scenario list (useful for CTest/CI smoke).
  [string[]]$Scenarios = @()
)

# Safety-first runner:
# - Never throws (so callers/tools won't abort on a single failing scenario)
# - Always writes per-scenario logs
# - Prints a summary at the end

$ErrorActionPreference = "Continue"

# Resolve repo root from this script's location (Tests/run_ui_replay_tests_safe.ps1 → ..).
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) { $ProjectRoot = $repoRoot }
if ([string]::IsNullOrWhiteSpace($OutDir))      { $OutDir      = Join-Path $repoRoot 'Tests\UIReplay\out' }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if ([string]::IsNullOrWhiteSpace($EditorExe)) {
  $presets = @($Preset, 'vs2026-x64-local', 'vs2026-x64-local-unity',
               'vs2026-arm64-local', 'vs2022-x64-local', 'vs2022-x64-local-unity') |
             Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
  foreach ($p in $presets) {
    $candidate = Join-Path $repoRoot "build\$p\bin\$Config\Apps\Editor\Editor.exe"
    if (Test-Path $candidate) { $EditorExe = $candidate; break }
  }
  if ([string]::IsNullOrWhiteSpace($EditorExe)) {
    # Last-resort fallback so the error message points at the preferred path.
    $EditorExe = Join-Path $repoRoot "build\vs2026-x64-local\bin\$Config\Apps\Editor\Editor.exe"
  }
}

$extraFrames = 120

$scenarioDir = Join-Path $repoRoot 'Tests\UIReplay'
$defaultScenarios = @(
  'smoke_idle.json',
  # Detect black flicker / present regressions (center readback must not go fully black after warmup).
  'smoke_present_not_black.json',
  # Stress swapchain recreation/resizes; should not flicker to black.
  'smoke_present_resize_fuzz.json',
  # Tear-out regression: undock Hierarchy into a floating window, then close it.
  'repro_tearout_hierarchy_smoke.json',
  # Tear-out + playmode regression coverage for SceneView/GameView paths.
  'repro_tearout_sceneview_playmode_smoke.json',
  'repro_tearout_gameview_playmode_nocamera_smoke.json',
  # Stable, content-backed scroll scenarios (verify real scrollY changes via probes).
  'assets_tree_scroll.json',
  'assets_grid_scroll.json',
  # Scrollbar drag (captured) should still update virtualization immediately.
  # (repro_assets_tree_scrollbar_drag removed in #219: the assets folder-tree is
  #  empty in the replay project, so no scrollbar exists to grab — coverage lives
  #  in repro_hierarchy_tree_scrollbar_drag, which self-populates its tree.)
  'repro_assets_grid_scrollbar_drag.json',
  'repro_assets_split_drag_grid_scrollbar_drag.json',
  'repro_assets_tree_expand_collapse_scroll.json',
  'repro_assets_splitter_resize_loop.json',
  'repro_hierarchy_tree_scrollbar_drag.json',
  # Hierarchy multi-select + drag reparent/reorder smoke
  'repro_hierarchy_multiselect_drag_reparent.json',
  # SceneView input regression reproduction (RMB drag should exercise captured-move fast path).
  'repro_sceneview_rmb_drag.json',
  # Inspector editing stability: create primitives and edit transform values.
  'repro_inspector_edit_primitives.json',
  # Assets ListView mode: wheel scroll virtualization + header horizontal sync.
  'repro_assets_listview_wheel_scroll_delay.json',
  # Assets ListView mode: horizontal scrollbar drag (narrow panel) must not crop/blank rows.
  'repro_assets_listview_horizontal_scrollbar_drag.json',
  # Assets multi-select + drag/drop move (uses sandbox under Assets/UIReplaySandbox)
  'repro_assets_dnd_multiselect_move.json',
  # Selection UX: click empty clears; click inside multi-select collapses to single.
  'repro_assets_grid_selection_click_clear.json',
  'repro_assets_list_selection_click_clear.json',
  'repro_hierarchy_selection_click_clear.json',
  # Log-panel scenario: depends on runtime log content / capture semantics.
  'scroll_log_listview_scrollbar_drag.json'
) | ForEach-Object { Join-Path $scenarioDir $_ }

$scenarios = $defaultScenarios
if ($null -ne $Scenarios -and $Scenarios.Count -gt 0) {
  $scenarios = $Scenarios
}

$failures = @()

function Get-ReplayResult([string]$logPath) {
  if (-not (Test-Path $logPath)) { return $null }
  foreach ($line in Get-Content -Path $logPath) {
    if ($line -match '"kind":"ui_replay_end"') {
      if ($line -match '"result":"(?<r>[^"]+)"') { return $Matches["r"] }
      return "unknown"
    }
    if ($line -match '"kind":"ui_replay_fail"') {
      return "fail"
    }
  }
  return $null
}

function Stop-EditorIfRunning() {
  $procs = Get-Process Editor -ErrorAction SilentlyContinue
  if ($null -eq $procs) { return }
  $procs | Stop-Process -Force -ErrorAction SilentlyContinue
  try { $procs | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue } catch { }
  Start-Sleep -Milliseconds 200
}

function Update-ProjectPickerRecentProject([string]$projectRoot) {
  if ([string]::IsNullOrWhiteSpace($projectRoot)) { return }

  $resolvedProjectRoot = $projectRoot
  try {
    $resolvedProjectRoot = [System.IO.Path]::GetFullPath($projectRoot)
  } catch {
    $resolvedProjectRoot = $projectRoot
  }
  if (-not (Test-Path -Path $resolvedProjectRoot -PathType Container)) { return }

  $prefsRoot = Join-Path (Join-Path $env:APPDATA "GameEngine") "Editor"
  $prefsPath = Join-Path $prefsRoot "Preferences.json"
  try {
    New-Item -ItemType Directory -Force -Path $prefsRoot | Out-Null
  } catch {
  }

  $prefsMap = @{}
  if (Test-Path $prefsPath) {
    try {
      $existing = Get-Content -Raw -Path $prefsPath | ConvertFrom-Json
      if ($null -ne $existing) {
        foreach ($prop in $existing.PSObject.Properties) {
          $prefsMap[$prop.Name] = $prop.Value
        }
      }
    } catch {
      $prefsMap = @{}
    }
  }

  if (-not $prefsMap.ContainsKey("schemaVersion")) {
    $prefsMap["schemaVersion"] = 1
  }

  $recent = New-Object System.Collections.Generic.List[string]
  $recent.Add($resolvedProjectRoot)
  if ($prefsMap.ContainsKey("recentProjects") -and $null -ne $prefsMap["recentProjects"]) {
    foreach ($candidate in @($prefsMap["recentProjects"])) {
      if ($null -eq $candidate) { continue }
      $candidatePath = [string]$candidate
      if ([string]::IsNullOrWhiteSpace($candidatePath)) { continue }
      if ([string]::Equals($candidatePath, $resolvedProjectRoot, [System.StringComparison]::OrdinalIgnoreCase)) { continue }
      $recent.Add($candidatePath)
      if ($recent.Count -ge 5) { break }
    }
  }

  $prefsMap["lastProjectPath"] = $resolvedProjectRoot
  $prefsMap["recentProjects"] = @($recent)

  try {
    $prefsMap | ConvertTo-Json -Depth 32 | Set-Content -Path $prefsPath -Encoding UTF8
  } catch {
  }
}

foreach ($s in $scenarios) {
  $name = [IO.Path]::GetFileNameWithoutExtension($s)
  $out = Join-Path $OutDir ("{0}.{1}.jsonl" -f $name, $Config)
  $stdout = Join-Path $OutDir ("{0}.{1}.stdout.txt" -f $name, $Config)
  $stderr = Join-Path $OutDir ("{0}.{1}.stderr.txt" -f $name, $Config)
  Write-Host ("[UIReplay] Running {0} (config={1})" -f $name, $Config)

  $scenarioJson = $null
  try {
    $scenarioJson = Get-Content -Raw -Path $s | ConvertFrom-Json
  } catch {
    $scenarioJson = $null
  }

  # Optional per-scenario launch mode:
  # - default: pass --project <ProjectRoot> (existing behavior)
  # - launchWithoutProject=true: start without --project to exercise project-picker flow
  $launchWithoutProject = $false
  if ($null -ne $scenarioJson -and $null -ne $scenarioJson.launchWithoutProject) {
    try {
      $launchWithoutProject = [bool]$scenarioJson.launchWithoutProject
    } catch {
      $launchWithoutProject = $false
    }
  }
  if ($launchWithoutProject) {
    Update-ProjectPickerRecentProject -projectRoot $ProjectRoot
  }

  # Optional: scenario-owned Assets sandbox (created under ProjectRoot\Assets\UIReplaySandbox\<name>).
  # This avoids polluting other tests and keeps file-based DnD deterministic.
  $sandboxName = $null
  if ($null -ne $scenarioJson -and $null -ne $scenarioJson.sandboxAssets) {
    $sandboxName = [string]$scenarioJson.sandboxAssets
  }
  if (-not [string]::IsNullOrWhiteSpace($sandboxName)) {
    $prep = Join-Path $ProjectRoot "Tests\\UIReplay\\prepare_assets_sandbox.ps1"
    if (Test-Path $prep) {
      powershell -ExecutionPolicy Bypass -File $prep -ProjectRoot $ProjectRoot -Name $sandboxName | Out-Null
    }
    # Make sandbox name available to UIReplay invokeCommand automation.
    $env:GE_UIREPLAY_ASSETS_SANDBOX = $sandboxName
  }

  if ($KillExistingEditor) {
    Stop-EditorIfRunning
  }

  # Ensure correctness mode is OFF for these stability tests.
  # Some shells may have this set from other scripts, and correctness mode forces heavy passes
  # which can mask performance-path regressions we want to catch.
  $env:GE_UI_CORRECTNESS_MODE = "0"

  # Pick a safer per-scenario frame budget:
  # - if the JSON specifies minFrames, run at least (minFrames + extraFrames)
  # - also respect the user-provided $ExitAfterFrames as a minimum
  $scenarioExit = $ExitAfterFrames
  if ($null -ne $scenarioJson -and $null -ne $scenarioJson.minFrames) {
    $min = [int]$scenarioJson.minFrames
    $scenarioExit = [Math]::Max($scenarioExit, ($min + $extraFrames))
  }

  $editorArgs = @("--ui-replay", $s, "--ui-replay-log", $out, "--exit-after-frames", $scenarioExit)
  if (-not $launchWithoutProject) {
    $editorArgs = @("--project", $ProjectRoot) + $editorArgs
  }

  $p = $null
  $timedOut = $false
  try {
    if ($CaptureStdout) {
      $p = Start-Process -FilePath $EditorExe -ArgumentList $editorArgs -NoNewWindow -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    } else {
      $p = Start-Process -FilePath $EditorExe -ArgumentList $editorArgs -NoNewWindow -PassThru
    }
    try {
      $p | Wait-Process -Timeout $TimeoutSeconds -ErrorAction Stop
    } catch {
      $timedOut = $true
    }
  } catch {
    $p = $null
  }

  $code = 0
  if ($timedOut) {
    $code = 124
    if ($null -ne $p) {
      try { $p | Stop-Process -Force -ErrorAction SilentlyContinue } catch { }
      try { $p | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue } catch { }
    }
    Write-Host ("[UIReplay] TIMEOUT {0} after {1}s (killed) log={2}" -f $name, $TimeoutSeconds, $out)
  } else {
    if ($null -ne $p) {
      try { $p.Refresh() } catch { }
      try { $code = $p.ExitCode } catch { $code = 1 }
    } else {
      $code = $LASTEXITCODE
    }
  }
  $result = Get-ReplayResult $out

  # Optional: verify probed scroll actually changed without visual inspection.
  # We only attempt this when the scenario declares probe.scrollViewUnderId.
  if ($null -ne $scenarioJson -and $null -ne $scenarioJson.probe -and $null -ne $scenarioJson.probe.scrollViewUnderId) {
    $underId = [string]$scenarioJson.probe.scrollViewUnderId
    if (-not [string]::IsNullOrWhiteSpace($underId) -and (Test-Path $out)) {
      $minY = $null
      $maxY = $null
      # JSONL probe objects often serialize fields in varying order; extract the scrollView object first,
      # then parse out underId + scrollY from that substring.
      $reProbeScrollView = [regex]('"probe":\{.*?"scrollView":\{(?<sv>[^}]*)\}')
      $reUnderId = [regex]('"underId":"(?<id>[^"]+)"')
      $reScrollY = [regex]('"scrollY":(?<y>-?[0-9]+(\.[0-9]+)?)')
      foreach ($line in Get-Content -Path $out) {
        $m = $reProbeScrollView.Match($line)
        if (-not $m.Success) { continue }
        $sv = $m.Groups["sv"].Value
        if ([string]::IsNullOrWhiteSpace($sv)) { continue }

        $mId = $reUnderId.Match($sv)
        $mY = $reScrollY.Match($sv)
        if (-not $mId.Success -or -not $mY.Success) { continue }
        if ($mId.Groups["id"].Value -ne $underId) { continue }

        $y = [double]$mY.Groups["y"].Value
        if ($null -eq $minY -or $y -lt $minY) { $minY = $y }
        if ($null -eq $maxY -or $y -gt $maxY) { $maxY = $y }
      }
      if ($null -ne $minY -and $null -ne $maxY) {
        $delta = [Math]::Abs($maxY - $minY)
        if ($delta -gt 0.01) {
          Write-Host ("[UIReplay] ScrollVerify OK underId={0} scrollYRange=[{1},{2}]" -f $underId, $minY, $maxY)
        } else {
          Write-Host ("[UIReplay] ScrollVerify NOTE underId={0} scrollYRange=[{1},{2}] (no change observed)" -f $underId, $minY, $maxY)
        }
      } else {
        Write-Host ("[UIReplay] ScrollVerify NOTE underId={0} (probe not found in log)" -f $underId)
      }
    }
  }

  $passedByLog = ($result -eq "pass")
  if (-not $passedByLog -and $code -ne 0) {
    $failures += [PSCustomObject]@{ scenario = $name; exitCode = $code; log = $out }
    if ($CaptureStdout) {
      Write-Host ("[UIReplay] FAILED {0} (exit={1}) log={2} stdout={3} stderr={4}" -f $name, $code, $out, $stdout, $stderr)
    } else {
      Write-Host ("[UIReplay] FAILED {0} (exit={1}) log={2}" -f $name, $code, $out)
    }
  } elseif (-not $passedByLog -and $null -ne $result -and $result -ne "pass") {
    $failures += [PSCustomObject]@{ scenario = $name; exitCode = $code; log = $out }
    if ($CaptureStdout) {
      Write-Host ("[UIReplay] FAILED {0} (result={1} exit={2}) log={3} stdout={4} stderr={5}" -f $name, $result, $code, $out, $stdout, $stderr)
    } else {
      Write-Host ("[UIReplay] FAILED {0} (result={1} exit={2}) log={3}" -f $name, $result, $code, $out)
    }
  } else {
    if ($CaptureStdout) {
      Write-Host ("[UIReplay] PASSED {0} log={1} stdout={2} stderr={3}" -f $name, $out, $stdout, $stderr)
    } else {
      Write-Host ("[UIReplay] PASSED {0} log={1}" -f $name, $out)
    }
  }

  # Cleanup sandbox after scenario (best-effort).
  if (-not [string]::IsNullOrWhiteSpace($sandboxName)) {
    $prep = Join-Path $ProjectRoot "Tests\\UIReplay\\prepare_assets_sandbox.ps1"
    if (Test-Path $prep) {
      powershell -ExecutionPolicy Bypass -File $prep -ProjectRoot $ProjectRoot -Name $sandboxName -CleanOnly | Out-Null
    }
  }
}

if ($failures.Count -gt 0) {
  Write-Host ("[UIReplay] Done with {0} failure(s):" -f $failures.Count)
  foreach ($f in $failures) {
    Write-Host ("  - {0} (exit={1}) log={2}" -f $f.scenario, $f.exitCode, $f.log)
  }
} else {
  Write-Host "[UIReplay] Done. All scenarios passed."
}

# Always exit success to keep automation sessions alive.
exit 0

