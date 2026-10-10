#pragma once

#include "Components/Rendering/PostProcessEffects/ShadowSettingsEffect.h"
#include "Types/Types.h"

namespace GameEngine {
namespace Engine {
namespace Renderer {

// Per-world directional shadow overrides resolved once per frame by
// RenderExtractionSystem from the dominant PostProcessVolume that carries an
// enabled ShadowSettingsEffect. Consumed by ShadowMapNode::DeclareForView, which
// pushes the four fields into the ShadowMapRenderFeature's config before the
// cascade fit.
//
// HasOverride == false means "no volume in this world overrides shadows" — the
// node falls back to its blueprint (.rendergraph) values, so absent volumes cost
// nothing. The float defaults mirror the shipped ForwardPlus blueprint values
// (and ShadowSettingsEffect's defaults) purely so a value read before extraction
// runs is harmless.
struct ResolvedShadowSettings {
    bool    HasOverride       = false;
    // Opaque directional sampling source. RayTraced gates ScheduleRTShadowMask
    // (AS build + mask pass) and the per-view RTShadowMask keyword swap; the
    // default keeps every world on cascades, so absent volumes cost nothing.
    Components::DirectionalShadowMode Mode = Components::DirectionalShadowMode::Cascades;
    Components::RayTracedShadowQuality RayTracedQuality =
        Components::RayTracedShadowQuality::Performance;
    Components::DirectionalShadowFilter Filter = Components::DirectionalShadowFilter::PCSS;
    float32 MaxShadowDistance = 200.0f;
    float32 DistanceFadeFraction = Components::ShadowSettingsEffect{}.DistanceFadeFraction;
    float32 SplitLambda       = 0.75f;
    float32 DepthBias         = 0.0001f;
    float32 NormalBias        = 0.5f;
    bool ScreenSpaceShadows = false;
    float32 ScreenSpaceShadowThickness = 0.005f;
};

} // namespace Renderer
} // namespace Engine
} // namespace GameEngine
