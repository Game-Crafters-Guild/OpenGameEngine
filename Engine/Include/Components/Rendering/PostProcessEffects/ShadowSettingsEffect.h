#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// How opaque receivers sample the primary directional light's shadow term.
// Cascades is the shipping default. RayTraced swaps opaque directional
// sampling to the per-pixel ray-query mask (hard edges, no peter-panning);
// cascades still render regardless — transmissive receivers (glass) always
// sample them, and RayTraced silently keeps Cascades on devices without ray
// query. Experimental coverage gaps: no skinned or terrain casters, and
// cutout foliage casts solid.
enum class DirectionalShadowMode : uint32 {
    Cascades = 0,
    RayTraced,
};

// Ray budget of the RayTraced mask. Performance traces one cone-jittered ray per
// pixel and resolves the penumbra with a wide spatial denoise (plus TAA where a
// view has it). Quality traces four rays per pixel on a per-pixel R2 sequence, so
// the pixel itself holds a fractional occlusion, and narrows the denoise to half
// the footprint: thin contact shadows and the physical 0.53 deg sun resolve in
// place instead of being averaged away.
enum class RayTracedShadowQuality : uint32 {
    Performance = 0,
    Quality,
};

// Cascade filter for the Cascades mode. Numeric values mirror
// Renderer::ShadowFilterQuality one-to-one (the node casts between them). A
// world runs one filter for every directional cascade, so it is a volume
// choice, not a per-light one. PCSS and MSM4 demote automatically on a device
// that lacks their prerequisites; the value stays the request.
enum class DirectionalShadowFilter : uint32 {
    Grid5x5 = 0,
    Grid3x3,
    PoissonPCF,
    PCSS,
    MSM4,
    DilatedPCF,
};

// Per-world directional shadow overrides. Attach to the same entity as a
// PostProcessVolume (like BloomEffect / ColorGradeEffect). When present and
// Enabled on the world's dominant volume, these override the ShadowMap render
// node's blueprint (.rendergraph) values for the directional cascade fit and the
// receiver anti-acne bias. Absent => the node's blueprint values apply unchanged,
// so a world with no ShadowSettingsEffect pays nothing.
//
// Cascade COUNT and shadow-map RESOLUTION deliberately stay node-side: they size
// GPU textures (the pooled depth array), so they are a pipeline-authoring decision
// rather than a per-frame volume tunable.
//
// Defaults mirror the shipped ForwardPlus blueprint values (200 / 0.75 /
// 0.0001 / 0.5) — the de-facto stock look — NOT the node's hardcoded members.
// A static default cannot match an arbitrary loaded blueprint, so an untouched
// component still overrides a pipeline whose blueprint diverges from these.
struct ShadowSettingsEffect {
    // Highest serialized DirectionalShadowMode value this build understands;
    // the scene schema maps anything above it (a future mode) to Cascades.
    static constexpr uint32 kDirectionalShadowModeLast =
        static_cast<uint32>(DirectionalShadowMode::RayTraced);
    static constexpr uint32 kRayTracedShadowQualityLast =
        static_cast<uint32>(RayTracedShadowQuality::Quality);
    static constexpr uint32 kDirectionalShadowFilterLast =
        static_cast<uint32>(DirectionalShadowFilter::DilatedPCF);
    // Upper bound of DistanceFadeFraction, shared by the descriptor clamp and the inspector.
    // A band wider than about half the distance weakens the nearest shadows, so it stops there.
    static constexpr float32 kDistanceFadeFractionMax = 0.5f;

    bool    Enabled           {true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    // Directional shadows are world-global: the dominant volume's Mode wins outright
    // (enums don't blend), so a local volume snaps the whole world's mode at its
    // blend edge. Prefer global volumes for Mode.
    DirectionalShadowMode Mode {DirectionalShadowMode::Cascades};
    // Ray budget for Mode == RayTraced; ignored under Cascades.
    RayTracedShadowQuality RayTracedQuality {RayTracedShadowQuality::Performance};
    // Cascade filter for Mode == Cascades. Default matches the render feature's
    // own default, so a volume that predates this field keeps its look.
    DirectionalShadowFilter Filter {DirectionalShadowFilter::PCSS};
    // Far distance (world units) at which directional shadows fade out, over its
    // authored DistanceFadeFraction (GE_ShadowDistanceFade). The last cascade is fit to
    // min(cameraFarPlane, MaxShadowDistance). Shorter distances pack the same
    // cascade resolution into a tighter range => sharper near shadows and a large
    // GPU saving (fewer casters per cascade, tighter culling).
    float32 MaxShadowDistance {200.0f};
    // Fraction of the range over which directional shadows fade to lit, 0 to
    // kDistanceFadeFractionMax. Zero keeps full strength until the range ends (a hard edge);
    // 0.5 fades over the far half.
    float32 DistanceFadeFraction {0.1f};
    // Cascade split blend between a uniform (0.0) and a logarithmic (1.0) partition
    // of [near, MaxShadowDistance]. Higher values concentrate resolution nearer the
    // camera. Mirrors the .rendergraph "splitLambda".
    float32 SplitLambda       {0.75f};
    // NDC receiver depth bias added before the reverse-Z shadow comparison
    // (GreaterOrEqual). House rule: this is a small POSITIVE NDC offset that the
    // shader adds to push the receiver toward the light and mask reverse-Z /
    // D32_SFLOAT rasterization noise. Mirrors the .rendergraph "depthBias".
    float32 DepthBias         {0.0001f};
    // Maximum world-space normal offset. The receiver shader caps it by the
    // cascade's texel/filter footprint and blends that footprint across splits
    // to reduce detached contact shadows. Mirrors "normalBias".
    float32 NormalBias        {0.5f};
    // Optional depth-only contact/detail supplement for the primary sun. Only
    // visible depth can occlude; offscreen geometry still relies on shadow maps.
    bool ScreenSpaceShadows {false};
    // Assumed thickness of each traced surface, as a fraction of the
    // receiver's distance from the camera: 0.005 is about five centimetres at
    // ten metres. Scale-free by construction, so one value holds from a prop
    // on a table to a building.
    float32 ScreenSpaceShadowThickness {0.005f};
};

} // namespace Components
} // namespace GameEngine
