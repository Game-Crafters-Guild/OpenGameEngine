# UIReplay

UIReplay is a deterministic automation harness that runs the Editor, injects scripted UI input events, and checks invariants/assertions (and optionally pixel hashes) to catch UI correctness regressions—especially around retained caching + virtualization.

## Run a single scenario (direct)

Build the Editor, then run (from the repo root):

```powershell
$repo   = (Resolve-Path .).Path
$editor = Join-Path $repo "build\vs2026-x64-local\bin\RelWithDebInfo\Apps\Editor\Editor.exe"
$s      = Join-Path $repo "Tests\UIReplay\repro_assets_listview_horizontal_scrollbar_drag.json"
$out    = Join-Path $repo "Tests\UIReplay\out\repro_assets_listview_horizontal_scrollbar_drag.RelWithDebInfo.jsonl"

& $editor --project $repo --ui-replay $s --ui-replay-log $out --exit-after-frames 600
```

Outputs:
- `Tests/UIReplay/out/*.jsonl`: per-frame telemetry + probes + assertion failures (if any)
- Depending on the runner, `*.stdout.txt` / `*.stderr.txt` may also be captured.

## Run the safe suite (recommended)

This is the “won’t hang your console” runner (per-scenario timeout, catches failures, always exits success):

```powershell
powershell -ExecutionPolicy Bypass -File ".\Tests\run_ui_replay_tests_safe.ps1" -Config RelWithDebInfo
```

To run only a subset (useful for quick local checks / CI smoke):

```powershell
$repo = (Resolve-Path .).Path
powershell -ExecutionPolicy Bypass -File ".\Tests\run_ui_replay_tests_safe.ps1" -Config RelWithDebInfo -Scenarios @(
  (Join-Path $repo "Tests\UIReplay\smoke_idle.json"),
  (Join-Path $repo "Tests\UIReplay\repro_assets_listview_horizontal_scrollbar_drag.json")
)
```

## Compare cached-mode vs correctness-mode

This runner executes selected scenarios twice:
- `GE_UI_CORRECTNESS_MODE=0` (retained fast paths enabled)
- `GE_UI_CORRECTNESS_MODE=1` (force heavy pass + disable fast paths)

```powershell
powershell -ExecutionPolicy Bypass -File ".\Tests\run_ui_replay_compare_modes_safe.ps1" -Config RelWithDebInfo
```

If scenarios include `pixelHash`, this script will also compare mode0 vs mode1 hashes per-frame.

## Useful env vars for debugging

- **Correctness mode**
  - `GE_UI_CORRECTNESS_MODE=1`: force heavy pass every frame (debug invalidation)
  - `GE_UI_CORRECTNESS_ASSERTS=1`: logs if `LayoutDirty/ChildrenDirty` remain after Update

- **Bisect retained fast paths (without full correctness mode)**
  - `GE_UI_DISABLE_NOOP_FAST_PATH=1`
  - `GE_UI_DISABLE_VISUAL_PATCH_ONLY=1`
  - `GE_UI_DISABLE_SCROLL_ONLY_PATH=1`

- **Text debug dump (automation-friendly)**
  - `GE_UI_TEXT_DEBUG_DUMP=1`: dumps `UI_TextDebug.txt` once when scroll activity occurs

## Adding a new scenario

- Create a JSON file under `Tests/UIReplay/`.
- Prefer asserting invariants (e.g. `paintOffsetsMatch`, `visibleTextNotFullyClipped`, `layoutConverged`) over pixel hashes when possible—hashes are more environment-sensitive.
- Add it to:
  - `Tests/run_ui_replay_tests_safe.ps1` for the default suite, and/or
  - `Tests/run_ui_replay_compare_modes_safe.ps1` if it should pass in both cached + correctness modes.

## CTest integration

If configured, CTest exposes a small smoke target (Windows):

```powershell
ctest -R UIReplay -C RelWithDebInfo
```

