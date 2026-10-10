#pragma once

namespace GameEngine::Editor
{

// Registers the editor's own Scene View tools with SceneViewToolStripRegistry::Get(), in
// their strip order (the spline tool, then the terrain brush). Called once at startup,
// before any module registers a tool of its own.
void RegisterBuiltInSceneViewTools();

} // namespace GameEngine::Editor
