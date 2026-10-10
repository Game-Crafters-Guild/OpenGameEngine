#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Rendering
{

struct RenderingDeviceCapabilities;

// Centralized feature-policy derived once from device capabilities at
// renderer init. Feature code gates on THIS, never on raw caps scattered
// through call sites, so the policy is loggable and testable in one place.
//
// Full is today's desktop path. Compatibility is the WebGPU-class profile:
// classic bind groups instead of bindless, CPU-built draw streams instead of
// GPU-driven scatter, and the advanced-feature passes off.
struct RendererProfile
{
    enum class Tier : uint8_t
    {
        Full,
        Compatibility,
    };

    Tier ActiveTier = Tier::Full;

    bool UseBindlessMaterials = true;
    bool UseGpuDrivenDraws = true;   // GPU scatter + drawIndirectCount consumption
    bool UseGpuOcclusionCulling = true;
    bool UseMeshShaders = true;
    bool UseRayTracing = true;
    bool UseInt64Compute = true;     // CBT terrain and friends
    // interpolateAtOffset in shaders (the parallax footprint at the pixel centre); off where the
    // device has no sampleRateShading, and the footprint then takes the centroid inputs' derivatives
    bool UseInterpolationFunctions = true;

    // Anisotropy ratio of the material trilinear sampler
    // (SamplerPreset::LinearRepeat): the engine's request, the same on every
    // tier, clamped to the device's maxSamplerAnisotropy. 1 = isotropic filtering.
    float MaterialSamplerAnisotropy = 1.0f;

    bool IsCompat() const { return ActiveTier == Tier::Compatibility; }

    // Derive the profile from what the device reports. Bindless is the
    // load-bearing discriminator: a device without it cannot run the full
    // path at all, so it selects Compatibility outright; the remaining flags
    // then AND in their own capability bits.
    static RendererProfile FromCapabilities(const RenderingDeviceCapabilities& caps);

    // Log the resolved profile with per-flag reasons (called once at init).
    void LogResolved() const;
};

// GE_FORCE_COMPAT=1 clamps a device's reported capabilities to the WebGPU-class
// subset (bindless/BDA/indirect-count/int64/mesh/RT/sample-rate shading off) so the
// whole compatibility renderer is testable on desktop Vulkan with native
// tooling. Applied by backends at the end of capability population; returns
// true when the override was active (callers may want to log it).
bool ApplyForceCompatOverride(RenderingDeviceCapabilities& caps);

} // namespace Rendering
} // namespace GameEngine
