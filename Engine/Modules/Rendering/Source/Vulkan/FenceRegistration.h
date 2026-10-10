#pragma once
#include <vector>
#include <cstdint>

namespace GameEngine {
namespace Rendering {

// A tiny helper to track whether a per-frame fence was actually submitted
class FenceRegistration {
public:
    void Initialize(size_t frames) { inUse.assign(frames, false); }
    void MarkSignaled(uint32_t frame) { if (frame < inUse.size()) inUse[frame] = true; }
    bool IsArmed(uint32_t frame) const { return frame < inUse.size() && inUse[frame]; }
    void Disarm(uint32_t frame) { if (frame < inUse.size()) inUse[frame] = false; }
    void Clear() { inUse.clear(); }

private:
    std::vector<bool> inUse;
};

} // namespace Rendering
} // namespace GameEngine

