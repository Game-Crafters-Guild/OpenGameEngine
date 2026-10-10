#pragma once

#include "Rendering/Core/Device.h"

#include <optional>

namespace GameEngine
{
namespace Rendering
{

/// Extended-dynamic-range headroom of the screen a window sits on, as macOS
/// reports it: the largest framebuffer value the screen shows above SDR white
/// (1.0 means no headroom).
struct MetalEdrHeadroom
{
    /// Headroom available right now (shrinks with ambient light and brightness).
    float current = 1.0f;
    /// Headroom the screen can reach; above 1.0 means the screen supports EDR.
    float potential = 1.0f;
    /// GE_METAL_FORCE_EDR: run the EDR path on an SDR screen (output clips at 1.0).
    bool forced = false;
};

/// One HDR output request in the form MetalDevice::SetHdrOutputMode takes it.
struct MetalHdrOutputRequest
{
    HdrOutputMode mode = HdrOutputMode::Off;
    HdrStaticMetadata metadata{};
    HdrSwapchainBitDepth bitDepth = HdrSwapchainBitDepth::Bit10;
};

/// The HDR request a Metal device applies when its first window target becomes
/// active, from the device creation parameters. Empty for an SDR launch, which
/// keeps the device's default HDR state.
std::optional<MetalHdrOutputRequest> MetalStartupHdrRequest(const DeviceDesc& desc);

/// The HDR state a Metal device holds after an HDR output request, given the
/// EDR headroom of the active window's screen. A null metadata keeps the
/// metadata of the previous state. The swapchain format fields of the display
/// are left for the device to fill, since they depend on a live swapchain.
HdrOutputState ResolveMetalHdrOutputState(HdrOutputState state, HdrOutputMode mode,
                                          const HdrStaticMetadata* metadata, HdrSwapchainBitDepth bitDepth,
                                          const MetalEdrHeadroom& headroom);

} // namespace Rendering
} // namespace GameEngine
