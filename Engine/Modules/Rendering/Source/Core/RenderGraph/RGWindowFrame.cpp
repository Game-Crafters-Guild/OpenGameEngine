#include "Rendering/Core/RenderGraph/RGWindowFrame.h"

#include "Rendering/Core/Device.h"

#include "Platform/Environment.h"

#include <algorithm>

namespace GameEngine::Rendering::RenderGraph
{
namespace
{

constexpr uint32_t kMinUploadRingSlots = 2;

// Async compute is on unless GE_ASYNC_COMPUTE is set to an off value. Read once
// per process: the queue map of a live frame stream never changes.
bool AsyncComputeAllowed()
{
    static const bool kAllowed = Platform::EnvironmentSwitchEnabled("GE_ASYNC_COMPUTE", true);
    return kAllowed;
}

} // namespace

RGWindowFrame MakeWindowRGFrame(IDevice& device)
{
    RGWindowFrame out;
    out.PersistentPool = std::make_unique<RGResourcePool>(&device);
    out.TransientPool = std::make_unique<RGTransientPool>(&device);
    out.UploadRing = std::make_unique<RGUploadRing>(
        &device, std::max(kMinUploadRingSlots, device.GetFramesInFlight()),
        kWindowUploadRingInitialSlotBytes);
    out.Frame = std::make_unique<RGFrame>(&device, out.PersistentPool.get(),
                                          out.TransientPool.get(), out.UploadRing.get());
    // Timeline semaphores order the cross-queue edges and resources use CONCURRENT
    // sharing across families, so the identity map needs no ownership transfers.
    if (AsyncComputeAllowed()
        && device.GetComputeQueueFamilyIndex() != device.GetGraphicsQueueFamilyIndex())
        out.Frame->SetPhysicalQueueMap(RGFrame::kIdentityQueueMap);
    return out;
}

} // namespace GameEngine::Rendering::RenderGraph
