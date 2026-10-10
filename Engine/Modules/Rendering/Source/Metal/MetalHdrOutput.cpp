#include "MetalHdrOutput.h"

#include <algorithm>

namespace GameEngine
{
namespace Rendering
{

std::optional<MetalHdrOutputRequest> MetalStartupHdrRequest(const DeviceDesc& desc)
{
    if (!desc.hdrEnabled || desc.hdrMode == HdrOutputMode::Off)
    {
        return std::nullopt;
    }
    return MetalHdrOutputRequest{desc.hdrMode, desc.hdrStaticMetadata, desc.hdrSwapchainBitDepth};
}

HdrOutputState ResolveMetalHdrOutputState(HdrOutputState state, HdrOutputMode mode,
                                          const HdrStaticMetadata* metadata, HdrSwapchainBitDepth bitDepth,
                                          const MetalEdrHeadroom& headroom)
{
    state.enabled = mode != HdrOutputMode::Off;
    state.requestedMode = mode;
    if (metadata != nullptr)
    {
        state.staticMetadata = *metadata;
    }

    // A screen with headroom above 1.0 shows scRGB values past SDR white.
    const bool edrAvailable = headroom.potential > 1.0f || headroom.forced;

    HdrDisplayInfo& display = state.display;
    display = {};
    display.name = "EDR";
    display.activeMonitor = true;
    display.hdrAvailable = edrAvailable;
    display.supportsScRGB = edrAvailable;
    display.maxLuminance = headroom.potential * state.staticMetadata.paperWhiteNits;
    display.maxFullFrameLuminance = display.maxLuminance;
    display.paperWhiteNits = state.staticMetadata.paperWhiteNits;
    display.outputMaxLinearValue = std::max(headroom.current, 1.0f);
    display.requestedMode = mode;
    if (headroom.forced)
    {
        display.diagnosticHints.push_back("GE_METAL_FORCE_EDR active: EDR forced on an SDR display");
    }

    const HdrOutputMode resolved = ResolveHdrOutputMode(mode, display);
    display.resolvedMode = resolved;
    state.activeMode = resolved;
    // Metal EDR drawables are fp16 regardless of the requested bit depth.
    state.swapchainBitDepth = resolved == HdrOutputMode::ScRGB ? HdrSwapchainBitDepth::Float16 : bitDepth;
    return state;
}

} // namespace Rendering
} // namespace GameEngine
