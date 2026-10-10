#pragma once

namespace GameEngine
{
struct EditorContext;
} // namespace GameEngine

namespace GameEngine::Editor
{

/// Registers the editor's own view overlays with ViewOverlayHost::Get(). Called once at startup,
/// after `context` carries what the overlays read; `context` outlives the overlays (the editor
/// resets the host before tearing the context down).
void RegisterBuiltInViewOverlays(const EditorContext& context);

} // namespace GameEngine::Editor
