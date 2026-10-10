#pragma once

#include "Rendering/Core/Device.h"

#include <optional>

namespace GameEngine
{
namespace Editor
{

struct HdrOutputRequestResolution
{
    bool enabled = false;
    Rendering::HdrOutputMode mode = Rendering::HdrOutputMode::Auto;
};

/// Resolves the effective HDR output request from the project setting and the
/// operator override (GE_FORCE_HDR_OUTPUT, already parsed).
///
/// An explicitly set override wins outright: it names the output mode, and Off
/// means disabled. That is what makes a launch recipe able to pin HDR without
/// editing project files — a project that pins `hdr.enabled` must not be able to
/// silently ignore the operator, because an override that reports the opposite
/// of the truth is worse than no override at all. With no override the project
/// setting governs.
///
/// An enabled request never carries mode Off; it normalizes to Auto, so callers
/// can read `mode` alone without re-checking `enabled`.
HdrOutputRequestResolution ResolveHdrOutputRequest(bool projectEnabled,
                                                   Rendering::HdrOutputMode projectMode,
                                                   std::optional<Rendering::HdrOutputMode> forcedMode);

/// True when the device's live output state already carries this request, so
/// applying it again would cost a device idle, a 1.5 s render-suppression window
/// with 30 skipped frames, and a world re-record — for no change. Guards the
/// settings-driven re-apply paths: settings page edits and project open.
///
/// Those costs are the editor's own and land on every backend. A swapchain
/// recreate is backend-dependent on top of them: VulkanDevice::SetHdrOutputMode
/// returns early when its own inputs are unchanged, while D3D12Device folds
/// "metadata was supplied" into its changed test and ApplyForMonitor always
/// supplies it.
///
/// An HDR request is matched on `enabled`/`requestedMode` — what the device
/// accepted — never on `activeMode`. A display exposing no HDR colorspace
/// resolves activeMode to Off while still holding the HDR request, so matching
/// activeMode would report "unsatisfied" forever and re-apply on every call. An
/// Off request is the one case activeMode is read: the output chain has to be in
/// SDR already, not merely hold an Off request.
bool HdrOutputStateSatisfiesRequest(const Rendering::HdrOutputState& live,
                                    Rendering::HdrOutputMode mode,
                                    Rendering::HdrSwapchainBitDepth bitDepth,
                                    const Rendering::HdrStaticMetadata& metadata);

} // namespace Editor
} // namespace GameEngine
