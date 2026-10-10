param(
  [string]$EditorExe = "",
  [ValidateSet("Debug", "RelWithDebInfo", "Release")]
  [string]$Config = "RelWithDebInfo",
  [string]$ProjectRoot,
  [string]$OutDir,
  [string]$Preset,
  [int]$ExitAfterFrames = 900,
  [switch]$KillExistingEditor = $true,
  # When enabled, writes the Editor process stdout/stderr for each run
  # next to the JSONL log. This helps diagnose random crashes or black frames.
  [switch]$CaptureStdout = $true
)

# Compare runner:
# - Runs selected scenarios under GE_UI_CORRECTNESS_MODE=0 and =1
# - Never throws (keeps automation sessions alive)
# - Writes separate logs per mode
# - Prints a compact summary (failures, fast-path hints, scroll ranges)

$ErrorActionPreference = "Continue"

# Resolve repo root from this script's location (Tests/run_ui_replay_compare_modes_safe.ps1 → ..).
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
    $EditorExe = Join-Path $repoRoot "build\vs2026-x64-local\bin\$Config\Apps\Editor\Editor.exe"
  }
}

$extraFrames = 180

$scenarioDir = Join-Path $repoRoot 'Tests\UIReplay'
$scenarios = @(
  'smoke_present_not_black.json',
  'smoke_present_resize_fuzz.json',
  'repro_hierarchy_multi_create.json',
  'repro_hierarchy_multi_create_cubes.json',
  'repro_inspector_scroll_after_create_cube.json',
  # Assets ListView mode: wheel scroll + horizontal scrollbar drag should pass in both modes.
  'repro_assets_listview_wheel_scroll_delay.json',
  'repro_assets_listview_horizontal_scrollbar_drag.json'
) | ForEach-Object { Join-Path $scenarioDir $_ }

function Get-ScenarioMinFrames([string]$path) {
  try {
    $j = Get-Content -Raw -Path $path | ConvertFrom-Json
    if ($null -ne $j -and $null -ne $j.minFrames) { return [int]$j.minFrames }
  } catch { }
  return 0
}

function Parse-FailMessage([string]$logPath) {
  if (-not (Test-Path $logPath)) { return $null }
  foreach ($line in Get-Content -Path $logPath) {
    if ($line -match '"kind":"ui_replay_fail"') {
      # crude JSON parse (message is safe)
      if ($line -match '"message":"(?<m>[^"]+)"') { return $Matches["m"] }
      return "ui_replay_fail"
    }
    if ($line -match '"kind":"ui_replay_end"') {
      return $null
    }
  }
  return $null
}

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

function Parse-ScrollYRange([string]$logPath) {
  if (-not (Test-Path $logPath)) { return $null }
  $minY = $null
  $maxY = $null
  $reScrollY = [regex]('"probe":\{.*?"scrollView":\{[^}]*"scrollY":(?<y>-?[0-9]+(\.[0-9]+)?)')
  foreach ($line in Get-Content -Path $logPath) {
    $m = $reScrollY.Match($line)
    if (-not $m.Success) { continue }
    $y = [double]$m.Groups["y"].Value
    if ($null -eq $minY -or $y -lt $minY) { $minY = $y }
    if ($null -eq $maxY -or $y -gt $maxY) { $maxY = $y }
  }
  if ($null -eq $minY -or $null -eq $maxY) { return $null }
  return @{ min = $minY; max = $maxY }
}

function Parse-FastPathCounts([string]$logPath) {
  if (-not (Test-Path $logPath)) { return @{} }
  $counts = @{
    tookVisualPatchOnly = 0
    tookNoOpFastPath = 0
    tookCapturedMoveFastPath = 0
    tookPassiveMoveFastPath = 0
    ranHeavyPass = 0
  }
  foreach ($line in Get-Content -Path $logPath) {
    if ($line -match '"tookVisualPatchOnly":true') { $counts.tookVisualPatchOnly++ }
    if ($line -match '"tookNoOpFastPath":true') { $counts.tookNoOpFastPath++ }
    if ($line -match '"tookCapturedMoveFastPath":true') { $counts.tookCapturedMoveFastPath++ }
    if ($line -match '"tookPassiveMoveFastPath":true') { $counts.tookPassiveMoveFastPath++ }
    if ($line -match '"ranHeavyPass":true') { $counts.ranHeavyPass++ }
  }
  return $counts
}

function Parse-PixelHashMap([string]$logPath) {
  $map = @{}
  if (-not (Test-Path $logPath)) { return $map }
  foreach ($line in Get-Content -Path $logPath) {
    if ($line -notmatch '"kind":"ui_replay_pixel_hash"') { continue }
    $f = $null
    $h = $null
    if ($line -match '"frame":(?<f>[0-9]+)') { $f = [int]$Matches["f"] }
    if ($line -match '"hash64":"(?<h>[0-9a-fA-F]+)"') { $h = $Matches["h"] }
    # 8e-7: hashes carry the producing arm/source. Frames hashed on different
    # arms or sources (old backbuffer bytes vs RG2 FinalLinear) are not
    # comparable — key them so the diff treats cross-arm pairs as SKIPPED.
    $arm = "old"
    $src = "backbuffer"
    if ($line -match '"arm":"(?<a>[a-z0-9_]+)"') { $arm = $Matches["a"] }
    if ($line -match '"src":"(?<s>[a-z0-9_]+)"') { $src = $Matches["s"] }
    if ($null -ne $f -and $null -ne $h) { $map[$f] = @{ hash = $h; arm = $arm; src = $src } }
  }
  return $map
}

function Stop-EditorIfRunning() {
  $procs = Get-Process Editor -ErrorAction SilentlyContinue
  if ($null -eq $procs) { return }
  # Try to stop and wait for exit to avoid rapid-restart hazards (Vulkan/WSI).
  $procs | Stop-Process -Force -ErrorAction SilentlyContinue
  try { $procs | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue } catch { }
  Start-Sleep -Milliseconds 200
}

function Clean-DiffArtifacts([string]$scenarioName) {
  # Diff images are generated out-of-band (python script) and can become stale.
  # Remove previous diff PNGs for this scenario so manual inspection can’t be misled.
  $pat = ("diff_{0}.frame*.png" -f $scenarioName)
  Get-ChildItem -Path $OutDir -Filter $pat -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
}

$results = @()

foreach ($s in $scenarios) {
  $name = [IO.Path]::GetFileNameWithoutExtension($s)
  $minFrames = Get-ScenarioMinFrames $s
  $scenarioExit = [Math]::Max($ExitAfterFrames, ($minFrames + $extraFrames))

  Clean-DiffArtifacts $name

  foreach ($mode in @("0","1")) {
    if ($KillExistingEditor) {
      Stop-EditorIfRunning
    }

    $env:GE_UI_CORRECTNESS_MODE = $mode
    $out = Join-Path $OutDir ("{0}.{1}.mode{2}.jsonl" -f $name, $Config, $mode)
    $stdout = Join-Path $OutDir ("{0}.{1}.mode{2}.stdout.txt" -f $name, $Config, $mode)
    Write-Host ("[UIReplayCompare] Running {0} mode={1} (config={2})" -f $name, $mode, $Config)

    if ($CaptureStdout) {
      & $EditorExe --project $ProjectRoot --ui-replay $s --ui-replay-log $out --exit-after-frames $scenarioExit *> $stdout
    } else {
      & $EditorExe --project $ProjectRoot --ui-replay $s --ui-replay-log $out --exit-after-frames $scenarioExit
    }
    $code = $LASTEXITCODE
    $result = Get-ReplayResult $out

    $failMsg = Parse-FailMessage $out
    $scrollRange = Parse-ScrollYRange $out
    $fastCounts = Parse-FastPathCounts $out

    $results += [PSCustomObject]@{
      scenario = $name
      mode = $mode
      exitCode = $code
      result = $result
      log = $out
      stdout = $stdout
      fail = $failMsg
      scrollMin = if ($null -ne $scrollRange) { $scrollRange.min } else { $null }
      scrollMax = if ($null -ne $scrollRange) { $scrollRange.max } else { $null }
      ranHeavyPass = $fastCounts.ranHeavyPass
      tookVisualPatchOnly = $fastCounts.tookVisualPatchOnly
      tookNoOpFastPath = $fastCounts.tookNoOpFastPath
      tookCapturedMoveFastPath = $fastCounts.tookCapturedMoveFastPath
      tookPassiveMoveFastPath = $fastCounts.tookPassiveMoveFastPath
    }
  }
}

Write-Host ""
Write-Host "[UIReplayCompare] Summary:"
foreach ($r in $results) {
  $scroll = if ($null -ne $r.scrollMin -and $null -ne $r.scrollMax) { ("scrollY=[{0},{1}]" -f $r.scrollMin, $r.scrollMax) } else { "scrollY=(n/a)" }
  $fp = ("heavy={0} vp={1} noop={2} capMove={3} passMove={4}" -f $r.ranHeavyPass, $r.tookVisualPatchOnly, $r.tookNoOpFastPath, $r.tookCapturedMoveFastPath, $r.tookPassiveMoveFastPath)
  $ok = ($r.result -eq "pass")
  $status = if ($ok) { "OK" } else { ("FAIL result={0} exit={1} msg={2}" -f $r.result, $r.exitCode, $r.fail) }
  if ($CaptureStdout) {
    Write-Host ("  - {0} mode={1}: {2} {3} {4} stdout={5}" -f $r.scenario, $r.mode, $status, $scroll, $fp, $r.stdout)
  } else {
    Write-Host ("  - {0} mode={1}: {2} {3} {4}" -f $r.scenario, $r.mode, $status, $scroll, $fp)
  }
}

# Pixel-hash comparison (mode0 vs mode1), per scenario.
Write-Host ""
Write-Host "[UIReplayCompare] Pixel hash (mode0 vs mode1):"
foreach ($s in ($results | Select-Object -ExpandProperty scenario -Unique)) {
  $r0 = $results | Where-Object { $_.scenario -eq $s -and $_.mode -eq "0" } | Select-Object -First 1
  $r1 = $results | Where-Object { $_.scenario -eq $s -and $_.mode -eq "1" } | Select-Object -First 1
  if ($null -eq $r0 -or $null -eq $r1) { continue }
  if ($r0.result -ne "pass" -or $r1.result -ne "pass") {
    Write-Host ("  - {0}: (skipped; non-pass result)" -f $s)
    continue
  }

  $m0 = Parse-PixelHashMap $r0.log
  $m1 = Parse-PixelHashMap $r1.log
  $common = @()
  foreach ($k in $m0.Keys) { if ($m1.ContainsKey($k)) { $common += $k } }
  $common = $common | Sort-Object
  if ($common.Count -eq 0) {
    Write-Host ("  - {0}: (no pixel hashes logged)" -f $s)
    continue
  }

  $mismatches = @()
  $skippedCrossArm = 0
  foreach ($f in $common) {
    # Different arm/source = not comparable (different bytes by design) — skip.
    if ($m0[$f].arm -ne $m1[$f].arm -or $m0[$f].src -ne $m1[$f].src) {
      $skippedCrossArm++
      continue
    }
    if ($m0[$f].hash -ne $m1[$f].hash) { $mismatches += $f }
  }
  Write-Host ("  - {0}: mismatches={1}/{2} (skipped cross-arm: {3})" -f $s, $mismatches.Count, $common.Count, $skippedCrossArm)
  if ($mismatches.Count -gt 0) {
    $show = $mismatches | Select-Object -First 5
    foreach ($f in $show) {
      Write-Host ("      frame {0}: mode0={1} mode1={2}" -f $f, $m0[$f].hash, $m1[$f].hash)
    }
  }
}

# Always exit success to keep automation sessions alive.
exit 0

