#pragma once

#include "Types/Types.h"

namespace GameEngine::Components
{

// Stochastic ray density for screen-space reflections: Low quarters the rough
// lobes' trace rate, High traces near-full-rate — less noise before the
// denoiser at more rays per frame. Values are the classify shader's quality
// index (serialized as the integer).
enum class SssrSampleQuality : int32
{
    Low = 0,
    Medium = 1,
    High = 2,
};

// FidelityFX SSSR-inspired stochastic screen-space reflections. Attach to the
// same entity as a PostProcessVolume. The renderer uses the scene HZB for
// hierarchical traversal and the following TAA stage as the temporal denoiser.
struct ScreenSpaceReflectionsEffect
{
    // Highest quality this build understands: scene load clamps a higher
    // on-disk value down to it rather than rejecting the scene.
    static constexpr int32 kSampleQualityLast = static_cast<int32>(SssrSampleQuality::High);
    // The composite clamps this to [0,1] before scaling the reflection, so it is
    // the fraction of the probe lobe screen-space radiance substitutes for: 1
    // replaces the whole lobe and no larger value can do more.
    static constexpr float32 kIntensityMax = 1.0f;
    static constexpr float32 kMinThickness = 0.001f;
    static constexpr float32 kMinEdgeFade = 0.001f;
    static constexpr float32 kMaxEdgeFade = 0.5f;
    static constexpr int32 kMinSteps = 8;
    static constexpr int32 kMaxSteps = 256;

    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    float32 Intensity{0.75f};
    float32 MaxDistance{100.0f};
    float32 Thickness{0.2f};
    float32 EdgeFade{0.08f};
    int32 MaxSteps{48};
    SssrSampleQuality SampleQuality{SssrSampleQuality::Medium};
    // Reflections-in-reflections from the previous frame's composited color.
    // Off by default: on curved objects the screen-space bounce fetches the
    // camera-facing shading (bright, reprojection-softened) where the direct
    // sample reads as a crisp mirror image — enable for flat mirror setups.
    bool MultiBounce{false};
};

} // namespace GameEngine::Components
