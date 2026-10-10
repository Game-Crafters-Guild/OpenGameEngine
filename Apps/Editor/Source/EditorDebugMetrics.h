#pragma once

namespace Rendering { class IDevice; namespace RenderGraph { class RGFrame; } }

namespace GameEngine
{

// Collects per-frame engine stats (VRAM, draw calls, render-graph counts,
// physics, audio, etc.) and pushes them into DebugMetrics. `frame` is the main
// window's live render graph (may be null) — source for the Render/* counters.
// Called from EditorApplication::Update. No-op when GE_ENABLE_METRICS is unset.
void CollectEditorDebugMetrics(Rendering::IDevice* device,
                               Rendering::RenderGraph::RGFrame* frame);

} // namespace GameEngine
