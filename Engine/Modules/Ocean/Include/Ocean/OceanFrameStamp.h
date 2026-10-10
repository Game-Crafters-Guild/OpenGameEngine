#pragma once

#include "Types/Types.h"

#include <chrono>

namespace GameEngine::Ocean
{
// The frame a per-view temporal ocean pass last declared in. Frames are the
// render graph's monotonic RGFrame::FrameIndex(); the device frame slot
// (IDevice::GetFrameIndex) wraps every frames-in-flight and is no identity.
struct OceanFrameStamp
{
    static constexpr uint64 kNever = ~0ull;
    uint64 Frame = kNever;

    bool IsFrame(uint64 frame) const
    {
        return Frame == frame;
    }
    // True when the stamp is the frame immediately before `frame`: the stored
    // history is exactly one frame old and may be accumulated into.
    bool PrecedesFrame(uint64 frame) const
    {
        return Frame != kNever && Frame + 1u == frame;
    }
};

// A view that has not declared for this long no longer exists or no longer
// shows the ocean; its per-view history is released. Wall-clock, because views
// in different windows advance separate render-graph frame counters.
inline constexpr std::chrono::seconds kOceanViewHistoryIdleTimeout{5};
} // namespace GameEngine::Ocean
