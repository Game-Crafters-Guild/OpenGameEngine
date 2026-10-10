#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// ---- Terrain surface RULES -------------------------------------------------
//
// A rule is one authored row: N conditions that AND together into a per-texel
// weight, and the material that weight is written with. Conditions AND by
// MULTIPLYING their falloffs, so a row's weight is the product of its
// conditions' — a slope band and a height band overlap where both are open, and
// their soft edges compound into one soft corner rather than a stair.
//
// The vocabulary is deliberately three condition KINDS (slope, height, noise)
// with two unit kinds each for the two that have them. METRES and DEGREES are
// what authoring wants and what the shipped defaults use. The NORMALIZED kinds
// chase the terrain's own live range instead of naming an absolute, which is the
// right choice for a band that must read correctly at any HeightScale (the
// default snow line) and the wrong one everywhere else — a threshold in
// `1 - N.y` means a different real angle on every terrain.
//
// Shore distance is NOT here: no shore-distance field exists in the engine, and
// the "height above sea level" proxy reads a cliff one metre from the water as
// far inland. It arrives with the field, as its own slice.

// A rule set is a fixed-capacity block inside the effect component: rules
// modifiers are FEW (one global plus a handful of shaped overrides), so a
// service-side store would buy no-caps at the price of a lifetime, a handle and
// a load-time re-creation. Overflow is a loud error at every entry point, never
// a silent drop.
static constexpr uint32 kMaxTerrainSurfaceRules = 8;
static constexpr uint32 kMaxTerrainRuleConditions = 4;

// What a condition measures, and in which unit. The unit is part of the kind
// rather than a separate field so no invalid pair exists to validate.
enum class TerrainRuleConditionKind : uint8
{
    // Surface angle from horizontal, in DEGREES over [0, 90]. The authoring
    // default: a band reads the way a slope is spoken about.
    SlopeDegrees = 0,
    // `1 - max(N.y, 0)` over [0, 1]. Squashed by the terrain's HeightScale, so a
    // threshold here is not a fixed angle — prefer SlopeDegrees unless you
    // specifically want the normalized domain.
    //
    // It is NOT the same measurement as SlopeDegrees rescaled: the heightfield
    // normal is taken from heights normalized to [0, 1] against metre spacing,
    // so this quantity is flattened by the terrain's HeightScale. A band
    // authored here means nothing in degrees, and vice versa.
    SlopeNormalized = 1,
    // METRES ABOVE THE TERRAIN'S BASE — not world-space Y. The terrain entity's
    // own Y translation is not part of it, which is the convention the other
    // modifier effects already use (Flatten takes a world-space target and
    // divides it by HeightScale, treating the result as terrain-local too).
    // Predictable to author and stable under sculpting: a snow line at 400 m
    // stays at 400 m when the mountain grows under it.
    HeightMetres = 2,
    // `(h - minH) / heightRange` over [0, 1], against the terrain's live global
    // height range — so the band CHASES the heightfield, which is what a band
    // that must read correctly at any HeightScale wants (the default snow line).
    HeightNormalized = 3,
    // fBM value noise over world XZ, remapped to [0, 1]. Breaks up the banding
    // the other kinds produce.
    Noise = 4,
};

// The shape of a condition's edge ramp.
enum class TerrainRuleFalloffCurve : uint8
{
    // Straight ramp across the feather. The DEFAULT: a band's edge is exactly as
    // soft as its authored Feather, with no easing to reason about.
    ClampedLinear = 0,
    // Cubic `t*t*(3-2t)`, the shape the volume falloff ramp uses. Softer corners.
    Smoothstep = 1,
};

// One condition: a band in the kind's own unit, with a ramp at each end.
//
// The weight is 1 inside [Min, Max] and ramps to 0 over `Feather` OUTSIDE each
// edge — not inside it. That is what lets a DEGENERATE band (Min == Max) be a
// triangular peak (a row that fires hardest at one exact slope and fades both
// ways), and it keeps a band's plateau exactly as wide as it was authored.
//
// A one-sided band puts the unused edge at (or beyond) the domain edge: slope
// `> 34 deg` is [34, 90], height `below 400 m` is [-inf, 400] in practice a Min
// far under the terrain. There is no separate "one-sided" flag to keep in step.
struct TerrainRuleCondition
{
    TerrainRuleConditionKind Kind = TerrainRuleConditionKind::SlopeDegrees;
    TerrainRuleFalloffCurve FalloffCurve = TerrainRuleFalloffCurve::ClampedLinear;
    uint8 _Pad0[2] = {};

    // The band, in the unit Kind names. Min > Max is an empty band: no texel is
    // inside it and only the ramps can contribute.
    float32 Min = 0.0f;
    float32 Max = 1.0f;

    // Ramp width beyond EACH edge, in the same unit. 0 is a hard edge.
    float32 Feather = 0.0f;

    // Kind::Noise only: the field this condition bands. Per condition rather
    // than per rule so a coarse blotch rule and a fine speckle rule can coexist;
    // the other kinds ignore both.
    float32 NoiseFrequency = 0.02f;
    uint32 NoiseSeed = 0;
};

static_assert(std::is_trivially_copyable_v<TerrainRuleCondition>);
static_assert(std::is_standard_layout_v<TerrainRuleCondition>);

// One authored row: conditions AND into a weight, the weight writes a material.
struct TerrainSurfaceRule
{
    // Which material the row resolves to. TODAY this is a splat CHANNEL index
    // ([0, kMaxTerrainMaterialLayers)), which the terrain's channel-role
    // indirection maps to a material record — the splat stores four fixed
    // channels and nothing else is addressable yet. It is not named LayerIndex
    // because the value becomes a direct library slot when indexed splatting
    // lands, and only this field's meaning changes when it does.
    uint32 MaterialSlot = 0;

    // Master strength for the row, multiplied into the conditions' product.
    float32 Strength = 1.0f;

    // How the row's weight enters the splat, with TerrainPaintLayerEffect's
    // semantics exactly: false adds weight and renormalizes, true lerps every
    // channel toward the pure material so the row reads as that material alone.
    bool Replace = false;

    uint8 ConditionCount = 0;
    uint8 _Pad0[2] = {};

    // A row with ZERO conditions is unconditional: it covers its whole volume at
    // full strength. That is the honest reading of "no conditions restrict me",
    // and it makes a freshly added row visible instead of inert.
    TerrainRuleCondition Conditions[kMaxTerrainRuleConditions] = {};
};

static_assert(std::is_trivially_copyable_v<TerrainSurfaceRule>);
static_assert(std::is_standard_layout_v<TerrainSurfaceRule>);

} // namespace GameEngine::Components
