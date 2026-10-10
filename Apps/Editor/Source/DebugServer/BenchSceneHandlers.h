#pragma once

namespace GameEngine
{

class EditorDebugServer;

// Registers the renderer scale-bench IPC handlers (spawn_bench_scene).
void RegisterBenchSceneHandlers(EditorDebugServer& server);

} // namespace GameEngine
