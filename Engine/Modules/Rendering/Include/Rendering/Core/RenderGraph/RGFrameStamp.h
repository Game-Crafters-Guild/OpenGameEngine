#pragma once

#include <cstdint>

namespace GameEngine::Rendering::RenderGraph
{
class RGFrame;

// Frame-identity stamp shared by every cache of RGFrame-local ids. RGFrame
// pointers are reused across frames (a stable per-window RGFrame is re-begun
// every app frame, and mid-app-frame by passive RenderSingle paths), so a
// recycled address must never validate a stale incarnation: validity is always
// the (pointer, FrameIndex) PAIR. Callers that only need to reap a dying frame
// at a soon-to-be-recycled address compare Frame alone on purpose — see the
// pointer-only reap sites — and must NOT route through IsFor.
//
// Lives in its own header, with the two accessors out of line, so that holding
// a stamp costs a forward declaration rather than the whole RGFrame graph.
// Editor headers cache frame-local ids without otherwise touching RGFrame, and
// before this split they paid for the type safety in compile time and so went
// without it, hand-rolling the pair as `const void*` + `uint64_t`.
// The pointer stays NON-const deliberately. It reads as identity here, but at
// least one consumer (RenderServicesDrawStreams' emit context) takes it back out
// as the frame producers sub-allocate their per-frame binding data from, so the
// stamp doubles as a frame handle there. Narrowing it to `const RGFrame*` would
// break that site, and papering over it with a const_cast would hide a real
// question about where a mutable frame should come from. The consequence is that
// STAMPING REQUIRES A MUTABLE FRAME — callers that retain a stamp take their
// frame by non-const reference, which is also the more honest signature for a
// function that keeps a pointer to it.
struct RGFrameStamp
{
    RGFrame* Frame = nullptr;
    uint64_t FrameIndex = 0;

    bool IsFor(const RGFrame& f) const;
    void Stamp(RGFrame& f);
};

} // namespace GameEngine::Rendering::RenderGraph
