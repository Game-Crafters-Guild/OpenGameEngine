# Editor harness

Live-editor end-to-end tests. They drive a running editor over its debug-server
IPC socket, edit assets on disk to trigger hot-reload, capture screenshots before
and after, and assert the visual delta with a pure-JS PNG decoder and
per-channel diff. Every assertion is effect-side: a handler answering `ok: true`
is never treated as evidence.

These are the only automated tests that exercise the editor as a running
application. `CLAUDE.md` calls runtime smoke mandatory for rendering, UI, GPU and
text changes; this harness is what automates it.

## Why these are not CTest targets

`Tests/Mcp/` drives the CLI against faked editors and needs no binary, so it
registers as ctest. Every script here needs a built editor, a window manager and
a real Vulkan device, so none of them is registered and none should be until
there is a machine that can host them. They are run by hand against an editor
you launched.

## When to run

Whenever you touch:

- Hot-reload paths (`RenderServices` invalidators, `UIManager` texture and font
  handlers, asset registries).
- The render-graph compile/apply pipeline.
- The `take_screenshot`, `set_scroll` or `set_gizmos_visibility` IPC handlers.
- Virtualization (`ListView`/`TreeView`/`GridView`).
- Inspector or Hierarchy panel rebuild paths.
- Terrain sculpting, or the play-mode review overlay (the standalone scripts).

## Run

Launch an editor first. Give it a port of its own — never 9999, which is the
port a person's editor uses:

```bash
# PowerShell, from the repo root:
#   Start-Process -FilePath ".\build\vs2026-x64-local\bin\DebugFast\Apps\Editor\Editor.exe" `
#       -ArgumentList '--debug-port','10077' `
#       -RedirectStandardOutput "$env:TEMP\editor.log" -RedirectStandardError "$env:TEMP\editor.err"

# Then, once the IPC port answers:
GE_EDITOR_DEBUG_PORT=10077 node Tests/EditorHarness/harness.mjs

# Or filter to specific scenarios by tag:
GE_EDITOR_DEBUG_PORT=10077 node Tests/EditorHarness/harness.mjs --only=rg-toggle,texture
```

Exit code 0 means every targeted scenario passed (a skip is allowed), 1 means at
least one failed, 2 means the harness could not reach an editor at all.

Output, all under `results/` and all gitignored:

- `report.md` — human-readable summary.
- `report.json` — machine-readable.
- `<scenario>_<label>.png` — the screenshot evidence each assertion read.

## Scenarios (`harness.mjs`)

`--only=` takes the tag; the report names the scenario.

| Tag | Scenario in the report | What a failure means |
|---|---|---|
| `screenshot-modes` | `screenshot-modes` | `take_screenshot` lost a window, viewport, panel, element or rect mode |
| `rapid-select` | `rapid-select-coalesce` | Concurrent `select_entity` calls no longer coalesce to the last one |
| `gizmo-visibility` | `gizmo-visibility` | `set_gizmos_visibility` stopped changing the viewport, stopped restoring it, or stopped naming which group a bad argument belonged to |
| `rg-toggle` | `rg-toggle` | A render-pipeline asset edited on disk no longer hot-reloads |
| `texture` | `texture-edit-ui-icon` | A UI background texture edited on disk no longer hot-reloads |
| `texture-scene` | `texture-scene-material` | The material-texture path — assign, swap, restore, switch — stopped reaching the renderer |
| `search-dialog-scroll` | `search-dialog-scroll` | Virtualized list rebuild after `set_scroll` stopped producing new content |

There is no model hot-reload scenario. The one that existed looked for `Fox.glb`
and `Duck.glb` at the root of the editor asset mount, where neither has ever been
staged, so it reported a skip on every run since it was written. Restoring that
coverage needs a model staged into the editor mount, which is a content decision,
not a harness one.

## Standalone scripts

Each takes the same `GE_EDITOR_DEBUG_PORT` and runs on its own.

| Script | What it proves |
|---|---|
| `play-review-dismissal.mjs` | Entities created and deleted through the real Play snapshot and Undo paths; the review overlay renders, and pointer input afterwards lands. Asserts the rendered overlay, because `get_editor_state.modal` does not report it. |
| `inspector-new-stack.mjs` | New Stack on an emitter that runs the default stack: the stack asset is written and assigned, the inspector rebuilds from a section's refresh callback once the asset loads, and the editor keeps answering. Red before the inspector deferred that rebuild (signal 11 in `InspectorPanel::TickSimulationRefresh`). Creates and deletes its own entity and undoes the created asset. |
| `terrain-sculpt-roundtrip.mjs` | A brush edit on a live terrain reaches the heightfield, survives save and reopen, and comes back rendering — in both domains (planar `.tzone` sidecar, planet `.tsculpt` sidecar). Takes `--domain=planar\|spherical\|both` and `--keep`. |

## Fixture projects

The directories next to the scripts are editor projects opened by hand
(`Editor.exe --project Tests/EditorHarness/<name>`) when a change needs a scene
that isolates one feature. No script opens them automatically.

| Project | Isolates |
|---|---|
| `aniso-rotation` | Anisotropic rotation across two materials |
| `beer-lambert` | Thin and thick glass absorption |
| `game-ui-hud` | A game-UI HUD driven from native systems |
| `game-ui-hud-csharp` | The same HUD driven from C# systems. The pair stays as two projects on purpose: both scripting hosts discover every script under a project, and both drivers target the same element ids, so one merged project would run both handlers on every click and could not attribute the HUD to either host. |
| `mat-selfheal` | Material recovery after an asset-identity change |
| `rg2-gate1`, `rg2-gate1-content` | Render-graph smoke, empty and with licensed glTF content |

## Files

- `harness.mjs` — the scenario runner.
- `lib/ipc.mjs` — a minimal TCP IPC client, deliberately separate from the
  `mcp/ge.mjs` CLI: the harness needs a client it can close deterministically
  between scenarios. It carries no command list, so it cannot drift.
- `lib/pngdiff.mjs` — pure-JS PNG decoder and per-channel diff.
- `lib/pngwrite.mjs` — solid-colour PNG encoder for generated fixtures.
- `lib/helpers.mjs` — repo-root and editor-asset-root resolution, backup and
  restore, screenshot capture and diffing.

## Restore guarantee

The harness backs up every asset it edits and restores the bytes on exit,
including on Ctrl+C. `backups/` is empty after a clean run. If a scenario crashes
the editor mid-edit, restore by hand from `backups/<sha>_<filename>`.
