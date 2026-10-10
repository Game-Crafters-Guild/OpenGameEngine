#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Plain data model for the lens-flare system, shared by the runtime assets
// (FlareAtlasAsset / LensFlareDefinitionAsset), the importer that transcodes
// external authoring formats into engine-native JSON, and the render-side
// extraction system. Deliberately free of engine math/asset types so it can be
// included from any layer without pulling in heavy dependencies.
namespace GameEngine::LensFlare
{

struct Color
{
    float R = 1.0f;
    float G = 1.0f;
    float B = 1.0f;
    float A = 1.0f;
};

// One keyframe of an authored animation curve (time/value + Hermite slopes),
// kept as plain data here; the extraction layer converts to Math::CurveKey for
// evaluation so this header stays free of engine math types.
struct CurveKeyData
{
    float Time = 0.0f;
    float Value = 0.0f;
    float InTangent = 0.0f;
    float OutTangent = 0.0f;
};

// A named sub-rect of the atlas texture, stored as a normalized UV rect with the
// origin at the top-left of the image (TexturePacker convention). The single
// v-flip needed for the engine's sampler is resolved in the shader, in one place.
struct AtlasSprite
{
    std::string Name;
    float U = 0.0f; // left, in [0,1]
    float V = 0.0f; // top, in [0,1]
    float W = 1.0f; // width, in [0,1]
    float H = 1.0f; // height, in [0,1]
};

// Sentinel for "no per-element override — use the flare's global coefficient".
// Far outside any plausible authored boost value.
inline constexpr float kInheritGlobalBoost = -1.0e9f;

// One drawable sprite within a flare. Placed along the optical axis (the line
// from the light's screen point through screen-center) at Position, then scaled,
// rotated, tinted, and faded per the globals + these per-element values.
struct FlareElement
{
    std::string SpriteName;     // atlas sprite this element draws
    int32_t     SpriteIndex = -1; // resolved index into FlareAtlas::Sprites (-1 = unresolved)

    bool  Visible    = true;
    float Brightness = 1.0f;
    float Scale      = 1.0f;
    float SizeX      = 1.0f;    // per-axis base size (sprite aspect / streak shape)
    float SizeY      = 1.0f;

    float Position = 0.0f;      // along the source->center axis: 0 = at source, 1 = center, 2 = mirrored
    float OffsetX  = 0.0f;      // additional screen-space offset (NDC-ish units)
    float OffsetY  = 0.0f;

    float AnamorphicX = 0.0f;   // per-axis pull of the position back toward the source (1 = pinned)
    float AnamorphicY = 0.0f;

    float Angle          = 0.0f; // base rotation, degrees
    bool  UseStarRotation = false; // orient along the optical axis
    bool  RotateToFlare   = false; // face the source
    float RotationSpeed   = 0.0f;  // degrees/sec accumulated over time

    // Per-element dynamic-boost overrides. kInheritGlobalBoost (the default) =
    // inherit the flare's global coefficient; any other value (including
    // negative ones — authored content dims elements with negative boosts) is
    // this element's override.
    float EdgeBrightnessBoost   = kInheritGlobalBoost;
    float CenterBrightnessBoost = kInheritGlobalBoost;
    float EdgeScaleBoost        = kInheritGlobalBoost;
    float CenterScaleBoost      = kInheritGlobalBoost;

    Color Tint{1.0f, 1.0f, 1.0f, 1.0f};
};

// Per-flare settings that apply to all of its elements.
struct FlareGlobals
{
    float GlobalScale      = 1.0f;
    float GlobalBrightness = 1.0f;
    Color GlobalTint{1.0f, 1.0f, 1.0f, 1.0f};

    bool  UseDistanceFade  = false;
    bool  UseDistanceScale = false;
    bool  UseMaxDistance   = false;
    float MaxDistance      = 150.0f;

    bool  UseAngleLimit      = false; // angle between the flare's forward and the camera direction
    float MaxAngle           = 90.0f;
    bool  UseAngleScale      = false; // angle falloff shrinks elements
    bool  UseAngleBrightness = false; // angle falloff dims elements
    bool  UseAngleCurve      = false; // remap the angle falloff through AngleCurveKeys
    std::vector<CurveKeyData> AngleCurveKeys;

    float OffScreenFadeDist = 0.4f;

    // Dynamic brightness ramp as the source approaches the screen edge / center.
    bool  UseDynamicEdgeBoost    = false;
    float DynamicEdgeBrightness  = 0.1f;
    float DynamicEdgeRange       = 0.3f;
    float DynamicEdgeBias        = -0.1f;
    // Shapes the edge amount over the band (empty = the default 0-1-0 bell).
    std::vector<CurveKeyData> DynamicEdgeCurveKeys;

    // Dynamic size ramp as the source approaches the screen edge.
    bool  UseDynamicEdgeScale    = false;
    float DynamicEdgeScale       = 0.0f; // additive multiplier at full edge; 0.5 = 1.5x
    float DynamicEdgeScaleRange  = 0.3f;
    float DynamicEdgeScaleBias   = 0.0f;

    bool  UseDynamicCenterBoost  = false;
    float DynamicCenterBrightness = 0.0f;
    float DynamicCenterScale      = 0.0f; // additive size boost at screen center
    float DynamicCenterRange      = 0.3f;
    float DynamicCenterBias       = 0.0f;

    // Multiply element size by the source entity's transform scale (X axis).
    bool MultiplyScaleByTransformScale = false;

    bool NeverCull = false;
};

} // namespace GameEngine::LensFlare
