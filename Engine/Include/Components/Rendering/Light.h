#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// Volume is reserved as a distinct light type; rendering support can specialize it later.
enum class LightType : uint32 { Directional=0, Point=1, Spot=2, Ambient=3, Area=4, Volume=5 };

enum class AreaLightShape : uint32 { Rectangle=0, Disc=1, Sphere=2, Cylinder=3 };

enum class LightFalloff : uint32 { PhysicalInverseSquare=0, Linear=1, SmoothRange=2, Custom=3 };

// How Intensity is interpreted. Unitless = legacy (pass-through). Physical units are converted
// to the engine's intensity on the CPU at extraction (LightPhotometry.h) — zero GPU cost.
enum class LightUnit : uint32 { Unitless=0, Lux=1, Lumen=2, Candela=3 };

// Opt-in punctual (point/spot/area) shadow-map resolution bucket. Inherit keeps
// the render pipeline node's punctual resolution (backward-compatible: existing
// content renders at the same resolution as before — no silent degrade). The
// tiers map to fixed sizes (Low 256 / Medium 512 / High 1024) and let distant or
// small lights shrink their VRAM footprint without touching the directional CSM.
enum class LightShadowTier : uint32 { Inherit=0, Low=1, Medium=2, High=3 };

// Directional and spot lights shine along the entity's +Z (forward) axis.
// Default sun authoring pose (inspector Euler XYZ degrees).
inline constexpr float kDefaultDirectionalLightEulerXDeg = 50.0f;
inline constexpr float kDefaultDirectionalLightEulerYDeg = -30.0f;
inline constexpr float kDefaultDirectionalLightEulerZDeg = 0.0f;

struct Light {
    LightType Type {LightType::Directional};
    float32 Color[3] {1.0f, 1.0f, 1.0f};
    float32 Intensity {1.0f};
    float32 Range {10.0f};       // point/spot
    float32 InnerAngle {0.5f};   // radians (spot)
    float32 OuterAngle {0.8f};   // radians (spot)
    AreaLightShape AreaShape {AreaLightShape::Rectangle};
    float32 AreaWidth {1.0f};
    float32 AreaHeight {1.0f};
    float32 AreaRadius {0.5f};
    LightFalloff Falloff {LightFalloff::PhysicalInverseSquare};
    float32 Decay {2.0f};
    float32 FogContribution {1.0f};
    float32 FogDensityBoost {0.0f};
    float32 FogAnisotropy {0.25f};
    float32 FogOriginFade {0.2f};
    bool CastsLight {true};
    bool CastsShadows {false};
    uint32 CascadeCount {4};     // dir
    LightShadowTier ShadowResolutionTier {LightShadowTier::Inherit}; // punctual shadow-map size bucket
    // Angular diameter of the light's disc seen from the receiver, in DEGREES.
    // Directional only. A light at infinity casts a penumbra of
    // depthDelta * tan(halfAngle), so this is the sole physical control on
    // penumbra width. The default is the physical solar disc; raise it for a
    // softer, stylized sun (2 is a common soft look). 0 means perfectly
    // collimated (hard shadows) and is legal — never divide by it.
    float32 ShadowAngularDiameter {0.53f};
    // Photometric controls — resolved into Color/Intensity at extraction (zero GPU cost).
    bool UseColorTemperature {false};      // tint Color by ColorTemperature (Kelvin -> RGB)
    float32 ColorTemperature {6500.0f};    // Kelvin; 6500 = neutral D65
    LightUnit IntensityUnit {LightUnit::Unitless}; // how Intensity is interpreted
};

} // namespace Components
} // namespace GameEngine
