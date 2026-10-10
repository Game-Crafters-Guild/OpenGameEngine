# Editor Developer Guidelines (Undo/Redo and Live Updates)

This document describes **how editor code should mutate ECS data** and **how to keep the editor UI/tools in sync** without introducing latency, stale inspectors, or undo/redo regressions.

## Core rule: one edit path

- **All editor-driven data changes must go through the editor edit APIs** (Undo/Redo + notifications), not ad-hoc `e.Set(...)` calls scattered across UI/tools.
- **Gizmos and inspectors must behave identically**: preview live, commit once, undo/redo restores exact state.

## Undo/Redo rules

- **Interactive edits (drag/scrub)**
  - Start on gesture begin (mouse down / begin edit / `OnValueChanging` first call).
  - Apply changes as **Preview** while the gesture is active.
  - **Commit once** on gesture end (mouse up / commit / `OnValueChanged`).
  - Result: **one undo step** per drag/scrub gesture.

- **One-shot edits (buttons/toggles/commands)**
  - Use a single command execute/commit path (no preview).

- **Never push bytes in tools**
  - Tools/inspectors should never manually manage “before/after bytes”.
  - Use `UndoRedoService::BeginInteractiveEdit(...)` with a snapshot target so capture/apply is centralized.

## Preview vs Commit semantics

- **Preview** is for “in-progress” updates that should be visible immediately (scene rendering + inspector fields).
- **Commit** is for the “final” update that becomes one undo entry.
- Do not spam undo stack on every mouse move; always coalesce interactive gestures.

## Notifications (live editor sync)

- Any editor mutation path must emit `EditorChangeNotifications::ComponentChangedEvent`.
  - `kind=Preview` for live dragging/scrubbing.
  - `kind=Commit` for commit/undo/redo.

- Subsystems should subscribe and refresh derived state:
  - Inspectors: refresh displayed fields when not actively editing them.
  - Scene tools: refresh gizmo pivot/selection visuals after commits and undo/redo.

## Inspector rules

- Inspectors must be **bi-directional**:
  - UI → ECS: via preview/commit edit sessions.
  - ECS → UI: via notifications (and/or polling) to refresh fields.

- **Do not stomp active edits**
  - When refreshing fields, do not overwrite the currently-focused field subtree. Update other fields freely.

- **Color fields use color controls**
  - New component inspectors must expose RGB-like properties as a color swatch plus color picker, following `LightInspector` and `ColorFilterEffectInspector`.
  - Do not present separate `R`, `G`, and `B` numeric rows unless the channels are intentionally independent data rather than a color.

## ECS / snapshot rules

- Undo/redo commands must not rely on stable component pointers.
  - Components can move due to archetype transitions.
  - Use `World::CaptureComponentBytes(...)` / `World::ApplyComponentBytesImmediate(...)`.

- If a component is not safely serializable as a POD blob, it must provide a proper handler-based serialize/apply path (via the ECS component registry).
