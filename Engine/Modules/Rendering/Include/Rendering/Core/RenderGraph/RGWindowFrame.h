#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"

#include <cstdint>
#include <memory>

namespace GameEngine::Rendering::RenderGraph
{

// Everything one window's render-graph frame stream owns: its pools, its upload
// ring (never shared between frames, see RGFrame's constructor) and the frame
// that drives them. Members are declared so that the frame is destroyed before
// the pools and the ring it points into.
struct RGWindowFrame
{
    std::unique_ptr<RGResourcePool> PersistentPool;
    std::unique_ptr<RGTransientPool> TransientPool;
    std::unique_ptr<RGUploadRing> UploadRing;
    std::unique_ptr<RGFrame> Frame;
};

// Starting capacity of each upload ring slot; a slot grows on demand.
inline constexpr uint64_t kWindowUploadRingInitialSlotBytes = 1u << 20;

// Builds the frame stream an app window renders through. The upload ring holds
// one slot per frame in flight, and at least two.
// When the device has a distinct compute queue family, logical Compute passes
// run on it (RGFrame::kIdentityQueueMap); GE_ASYNC_COMPUTE set to an off value
// (Platform::EnvironmentSwitchEnabled) keeps every pass on the graphics queue.
RGWindowFrame MakeWindowRGFrame(IDevice& device);

} // namespace GameEngine::Rendering::RenderGraph
