# Animation Window

The Animation window is a dockable editor panel that provides timeline and animation-editing views.

**Full API reference (HTML):** [Timeline-API-Manual.html](Timeline-API-Manual.html) — engine `EvaluateTimeline`, `.timeline` JSON, `AnimationWindowPanel`, and timeline bar widgets.

## Views

- **Timeline bar** – Horizontal strip with time ruler, playhead, and Play/Stop (and Pause). Shared by all views. Timeline state (current time, range, FPS, play/loop) is owned by the panel and advanced each frame when playing.
- **Dope Sheet** – Rows per channel; keyframes shown as blocks on a time grid. Driven by a single AnimationClip (e.g. current selection). Uses engine `AnimationClip` and `AnimChannel` data.
- **Curves (graph)** – Time vs value for selected channels; step/linear segments and keyframe points. Same clip source as Dope Sheet.
- **Composite** – Non-linear composite of clips on tracks; tracks as rows, clips as blocks. Editor-only model (`TimeCompositeModel`, `CompositeTrack`, `CompositeClip`).
- **Lane-based clip editor** – Lanes as rows, clip instances as horizontal blocks. Editor-only model (`LaneClipModel`, `LaneClipLane`, `LaneClipInstance`).

## Data

- **Current clip** – Set via `AnimationWindowPanel::SetCurrentClip(AnimationClip*)` (e.g. from asset selection). Feeds Dope Sheet and Curves views.
- **Timeline state** – `TimelineState` (current time, range start/end, FPS, playing, loop). Panel advances time in `TickTimeline(deltaTime)` when playing; the editor calls this from `EditorApplication::Update`.

## Layout

The panel is registered as `AnimationWindowPanel` and appears in the dock as the "Animation" tab (e.g. with Assets and Log in the default layout). Open it via the dock tab bar or layout presets.
