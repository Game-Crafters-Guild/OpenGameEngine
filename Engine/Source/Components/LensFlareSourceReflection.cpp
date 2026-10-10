// Manual reflection + cross-DLL template instantiation for LensFlareSource.
//
// The build-time ComponentScanner skips this struct: its AssetRef<> field is a
// templated type the scanner's lightweight C++ parser can't resolve, so it omits
// the struct rather than emit an incomplete field list. Components with asset
// references are therefore registered by hand here (same approach as the terrain
// modifiers and EZTree component).

#include "Components/ComponentRegistration.h" // GE_REGISTER_COMPONENT
#include "Components/Rendering/LensFlareSource.h"

#include "ECS/ECSTemplates.h" // GE_INSTANTIATE_ENGINE_COMPONENT

// Field table + factory registration (Add Component menu + scene serialization).
GE_REGISTER_COMPONENT(GameEngine::Components::LensFlareSource, Flare, SunMode, Intensity, Scale, Tint, Occlude,
                      MaxDistanceOverride);

// Inspector polish: slider bounds + tooltips (must follow registration).
namespace
{
using GameEngine::Components::LensFlareSource;
using GameEngine::Components::SetReflectedFieldRange;
using GameEngine::Components::SetReflectedFieldTooltip;

const bool kLensFlareRanges =
    SetReflectedFieldRange<LensFlareSource>("Intensity", 0.0f, 10.0f) &&
    SetReflectedFieldRange<LensFlareSource>("Scale", 0.0f, 10.0f) &&
    SetReflectedFieldRange<LensFlareSource>("MaxDistanceOverride", 0.0f, 1000.0f) &&
    SetReflectedFieldTooltip<LensFlareSource>(
        "SunMode",
        "For a directional light, project the flare from the light direction as an "
        "infinite sun source. Positional distance and source-angle falloff are ignored.") &&
    SetReflectedFieldTooltip<LensFlareSource>(
        "Intensity", "Brightness multiplier on top of the flare definition's global brightness.") &&
    SetReflectedFieldTooltip<LensFlareSource>(
        "Scale", "Size multiplier on top of the flare definition's global scale.") &&
    SetReflectedFieldTooltip<LensFlareSource>(
        "Tint", "Per-source tint multiplied into every flare element's color.") &&
    SetReflectedFieldTooltip<LensFlareSource>(
        "Occlude", "Fade the flare when scene geometry covers the source (depth probe).") &&
    SetReflectedFieldTooltip<LensFlareSource>(
        "MaxDistanceOverride",
        "When above zero, replaces the definition's max distance for this source "
        "(distance fade, size falloff, and cull range).");
} // namespace

// Explicit instantiation of the type-erased handler + World accessors so the
// component works across the Engine DLL boundary.
namespace GameEngine::ECS
{
GE_INSTANTIATE_ENGINE_COMPONENT(Components::LensFlareSource);
} // namespace GameEngine::ECS
