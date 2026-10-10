#pragma once

// Threading switches for multi-threaded rendering.

#include <atomic>

namespace GameEngine { namespace Rendering {

// Global flag to force single-threaded rendering for debugging.
// When true, all multi-threaded rendering paths should fall back to
// sequential execution. Check this at the top of any parallel dispatch.
inline std::atomic<bool> g_ForceSingleThreadedRendering{false};

}} // namespace GameEngine::Rendering
