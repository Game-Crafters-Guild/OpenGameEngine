#include "Display/HdrOutputMatch.h"

namespace GameEngine
{
namespace Editor
{

HdrOutputRequestResolution ResolveHdrOutputRequest(bool projectEnabled,
                                                   Rendering::HdrOutputMode projectMode,
                                                   std::optional<Rendering::HdrOutputMode> forcedMode)
{
    HdrOutputRequestResolution resolved{projectEnabled, projectMode};
    if (forcedMode)
    {
        resolved.enabled = *forcedMode != Rendering::HdrOutputMode::Off;
        resolved.mode = *forcedMode;
    }
    if (resolved.enabled && resolved.mode == Rendering::HdrOutputMode::Off)
        resolved.mode = Rendering::HdrOutputMode::Auto;
    return resolved;
}

bool HdrOutputStateSatisfiesRequest(const Rendering::HdrOutputState& live,
                                    Rendering::HdrOutputMode mode,
                                    Rendering::HdrSwapchainBitDepth bitDepth,
                                    const Rendering::HdrStaticMetadata& metadata)
{
    // An Off request ignores bit depth and metadata: neither reaches the output
    // chain while HDR is off, so requiring them to match would re-apply over
    // values nothing reads.
    if (mode == Rendering::HdrOutputMode::Off)
        return !live.enabled && live.activeMode == Rendering::HdrOutputMode::Off;

    return live.enabled &&
           live.requestedMode == mode &&
           live.swapchainBitDepth == bitDepth &&
           live.staticMetadata == metadata;
}

} // namespace Editor
} // namespace GameEngine
