#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

/// Selects how the ambient floor is shaped. Flat is a single constant color; Gradient is the
/// Sky/Equator/Ground ramp blended by world-normal.y (up -> Sky, horizon -> Equator,
/// down -> Ground) — the same three-way ramp as SkyEnvironment's AmbientTint.
enum class AmbientLightMode : uint8
{
    Flat = 0,
    Gradient = 1,
};

/// Default ambient-floor luminance in nits (cd/m^2) on the shared kReferenceWhiteNits (203) anchor.
/// A soft fill well under the key: the gradient-sky arc found a full-dome ambient around this level
/// reads as a believable night-sky fill. Color x this / 203 = the scene-linear irradiance floor.
inline constexpr float32 kDefaultAmbientLightIntensityNits = 60.0f;

/// Opt-in authored ambient IRRADIANCE FLOOR added to the diffuse term at IBL sampling time
/// (ibl.glsl GE_AmbientFloor), ON TOP of the sky/probe-derived irradiance. It is ADDITIVE and live
/// (a UBO value, so editing it needs no IBL rebake and it is immune to bake settling) — the additive
/// analogue of SkyEnvironment's MULTIPLICATIVE AmbientTint*. With no AmbientLight in the world the
/// floor is exactly zero and every surface renders byte-identically to a build without this feature.
/// Diffuse-only by default (AffectSpecular = false): specular reflections keep coming from the
/// sky/probes so a floor cannot wash out the physical reflection lobe.
struct AmbientLight {
    AmbientLightMode Mode = AmbientLightMode::Flat;

    /// Flat mode: the single ambient color (authored linear RGB, 0..1).
    float32 Color[3] = {1.0f, 1.0f, 1.0f};

    /// Gradient mode: up -> Sky, horizon -> Equator, down -> Ground (authored linear RGB, 0..1).
    float32 SkyColor[3]     = {1.0f, 1.0f, 1.0f};
    float32 EquatorColor[3] = {1.0f, 1.0f, 1.0f};
    float32 GroundColor[3]  = {1.0f, 1.0f, 1.0f};

    /// Floor luminance in nits on the 203-nit reference-white anchor. Color x Intensity / 203 gives
    /// the scene-linear irradiance floor, on the SAME anchor as the physical sky, gradient sky, the
    /// physical-unit lights, and camera auto-exposure — so a floor authored in nits balances against
    /// the rest of the frame instead of floating on an arbitrary scale.
    float32 Intensity = kDefaultAmbientLightIntensityNits;

    /// When false (default) the floor lifts the DIFFUSE term only; specular reflections continue to
    /// come from the sky/probes. When true the floor also fills the primary specular reflection lobe
    /// (a constant-radiance probe fallback) for pure scenes with no meaningful specular environment.
    bool AffectSpecular = false;
};

} // namespace Components
} // namespace GameEngine
