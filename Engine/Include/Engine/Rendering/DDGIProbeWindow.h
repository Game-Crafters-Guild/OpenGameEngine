#pragma once

#include <algorithm>
#include <cstdint>

namespace GameEngine::Engine::Renderer
{
/// One tick's slice of the round-robin probe budget: the probes
/// [Base, Base + Count) are classified, traced and blended together this tick.
struct DDGIProbeWindow
{
    uint32_t Base = 0;
    uint32_t Count = 0;
    /// Where the next tick's window starts.
    uint32_t NextCursor = 0;
};

/// Advances the round-robin cursor by up to `probesPerTick` probes. The window
/// never straddles the end of the grid: every DDGI kernel drops a probe index
/// past the total, so a wrapped window would lose its tail while the cursor
/// moved past it, and with a budget that does not divide the total the probes
/// just after index 0 would then go unvisited for whole cycles. The window is
/// clamped to the end instead and the next tick starts at 0.
inline DDGIProbeWindow ComputeDDGIProbeWindow(uint32_t cursor, uint32_t probesPerTick, uint32_t probeTotal)
{
    DDGIProbeWindow window;
    if (probeTotal == 0)
        return window;
    window.Base = cursor % probeTotal;
    window.Count = std::min(std::max(probesPerTick, 1u), probeTotal - window.Base);
    window.NextCursor = (window.Base + window.Count) % probeTotal;
    return window;
}
}  // namespace GameEngine::Engine::Renderer
