#pragma once

// Presentation for the periodic [CascadeCache] instrumentation lines: the
// per-window miss-cause histogram that both the Info stats line and the storm
// Warning carry. Composed in one place so the two lines cannot drift, and kept
// out of the cache (a pure state machine) and out of the pass file (pass
// declaration) so the text is unit-testable without a device.

#include "Engine/Rendering/CascadeShadowCache.h"

#include <cstdint>
#include <string>

namespace GameEngine
{
namespace Engine::Renderer
{

// "first 1 contrib 0 contribchg 0 underdraw 0 phys 0 casters 3@v41 camera 12
// fit 4 config 0 settle 2" — every miss cause for ONE window, in the priority
// order Evaluate resolves them.
//
// `casterContentVersion` is RenderServices::ShadowCasterContentVersion for the
// view's world, printed against the cause it explains: a caster-miss count
// only means something next to the version driving it. A high count with a
// fast-climbing version is content churn; a high count with a static version
// is the cache comparing against a record that never committed.
std::string FormatCascadeCacheCauseHistogram(const CascadeShadowCache::Stats& window,
                                             uint64_t casterContentVersion);

} // namespace Engine::Renderer
} // namespace GameEngine
