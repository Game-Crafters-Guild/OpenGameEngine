#pragma once

#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Types/Types.h"

namespace GameEngine::TerrainECS
{

// ---- The default surface rule rows ------------------------------------------
//
// The rows a new terrain's global rules modifier carries. They are an ORDINARY
// authored value, not machinery: the modifier that holds them is deletable, the
// editor shows these rows in the condition widget, and every number below is a
// one-line edit there. Nothing re-creates them.
//
// SLOPE IS AUTHORED IN DEGREES, deliberately, and this is the trap the choice
// avoids. The normalized slope domain (`1 - max(N.y, 0)`) is taken off a
// heightfield whose samples are normalized to [0,1] but differenced over METRE
// spacing, so a threshold in it is divided by the terrain's HeightScale and means
// a different real angle on every terrain: a band that reads as 33.6 degrees at
// HeightScale 1 sits at 88.6 at HeightScale 60, where no ground reaches it and
// the row silently paints nothing. On a measured 512 m terrain the steepest texel
// reaches 0.000129 in that domain against a 0.1667 threshold — a 1292x shortfall.
// SlopeDegrees has no such dependence, and these rows are placed against a
// MEASURED distribution (TerrainDefaultSurfaceRulesTests asserts every band still
// falls inside it). The snow row is the one normalized band, for the scale reason
// given on its constants.
//
// ROW ORDER IS LOAD-BEARING. CompositeSplatTexel renormalizes as each row lands,
// so where two rows are both live the LATER one dominates. The order is base
// coverage first, then accents, then the cap: grass, dirt, rock, snow. Reordering
// the rows in the widget changes the surface — that is the intended control, not
// a hazard to design around.

// Slope bands, in degrees from horizontal. Placed against the MEASURED slope
// distribution of a 512 m terrain (TerrainDefaultSurfaceRulesTests prints it):
// p50 13.5, p75 18.8, p90 23.9, p95 26.9, p99 32.5, max 44.0 degrees.
inline constexpr float32 kDefaultGrassSlopeMax = 16.0f;     // full grass over the gentle half
inline constexpr float32 kDefaultGrassSlopeFeather = 10.0f; // gone by 26 deg, the p95 slope
inline constexpr float32 kDefaultDirtSlopeMin = 18.0f;      // the p75 shoulder, where soil thins
inline constexpr float32 kDefaultDirtSlopeMax = 27.0f;      // to the p95
inline constexpr float32 kDefaultDirtSlopeFeather = 6.0f;
inline constexpr float32 kDefaultRockSlopeMin = 30.0f;      // the p99: the steepest ground only
inline constexpr float32 kDefaultRockSlopeFeather = 9.0f;   // blending in from the p90

// Snow: an altitude cap, with a slope limit so it does not cling to cliff faces.
//
// NORMALIZED, not metres, and that is a deliberate exception to "defaults author
// in absolute units". A default ships on every new terrain, and new terrains do
// not share a height scale — the editor's presets are HeightScale 128 and 512
// (TerrainProvisioning.cpp) while the island content is 60 — so a snow line in
// metres would sit near the summit on one and underwater on the next. The
// normalized band chases the terrain's own range and reads correctly at any
// scale. Changing a terrain's snow line to true metres is a one-line Kind edit
// in the condition widget, which is the right place for a per-terrain decision.
// Measured normalized height: p50 0.597, p75 0.709, p90 0.798, p95 0.849.
inline constexpr float32 kDefaultSnowHeightMin = 0.85f;     // full snow above the p95
inline constexpr float32 kDefaultSnowHeightFeather = 0.15f; // blending down to the p75
inline constexpr float32 kDefaultSnowSlopeMax = 25.0f;
inline constexpr float32 kDefaultSnowSlopeFeather = 8.0f;

// The far edge of a one-sided band. A band is [Min, Max] with ramps OUTSIDE each
// edge, so a one-sided band puts its unused edge past the domain: slope cannot
// exceed 90 degrees, and no terrain is this tall.
inline constexpr float32 kDefaultSlopeDomainMax = 90.0f;
inline constexpr float32 kDefaultNormalizedHeightDomainMax = 1.0f;

// Splat channels, in the order the shipped default palette binds them
// (Terrain::kDefaultTerrainMaterials): 0 grass, 1 rock, 2 dirt, 3 snow.
inline constexpr uint32 kDefaultGrassSlot = 0;
inline constexpr uint32 kDefaultRockSlot = 1;
inline constexpr uint32 kDefaultDirtSlot = 2;
inline constexpr uint32 kDefaultSnowSlot = 3;

inline Components::TerrainRuleCondition SlopeBand(float32 minDeg, float32 maxDeg, float32 feather)
{
    Components::TerrainRuleCondition c{};
    c.Kind = Components::TerrainRuleConditionKind::SlopeDegrees;
    c.FalloffCurve = Components::TerrainRuleFalloffCurve::ClampedLinear;
    c.Min = minDeg;
    c.Max = maxDeg;
    c.Feather = feather;
    return c;
}

inline Components::TerrainRuleCondition NormalizedHeightBand(float32 min, float32 max,
                                                             float32 feather)
{
    Components::TerrainRuleCondition c{};
    c.Kind = Components::TerrainRuleConditionKind::HeightNormalized;
    c.FalloffCurve = Components::TerrainRuleFalloffCurve::ClampedLinear;
    c.Min = min;
    c.Max = max;
    c.Feather = feather;
    return c;
}

// The rows themselves. Returns the whole effect rather than a row array so the
// caller cannot forget RuleCount.
inline Components::TerrainSurfaceRulesEffect MakeDefaultTerrainSurfaceRules()
{
    Components::TerrainSurfaceRulesEffect fx{};
    fx.RuleCount = 4;

    // 0 · Grass — the base coverage. Everything flat enough to hold soil.
    fx.Rules[0].MaterialSlot = kDefaultGrassSlot;
    fx.Rules[0].ConditionCount = 1;
    fx.Rules[0].Conditions[0] =
        SlopeBand(0.0f, kDefaultGrassSlopeMax, kDefaultGrassSlopeFeather);

    // 1 · Dirt — the shoulder between grass and bare rock, where soil thins but
    // has not gone. A band rather than a ceiling, so it reads as a transition.
    fx.Rules[1].MaterialSlot = kDefaultDirtSlot;
    fx.Rules[1].ConditionCount = 1;
    fx.Rules[1].Conditions[0] =
        SlopeBand(kDefaultDirtSlopeMin, kDefaultDirtSlopeMax, kDefaultDirtSlopeFeather);

    // 2 · Rock — steep ground, and steeper wins over the shoulder because this
    // row lands after it.
    fx.Rules[2].MaterialSlot = kDefaultRockSlot;
    fx.Rules[2].ConditionCount = 1;
    fx.Rules[2].Conditions[0] =
        SlopeBand(kDefaultRockSlopeMin, kDefaultSlopeDomainMax, kDefaultRockSlopeFeather);

    // 3 · Snow — altitude, capped by slope so it does not stick to cliff faces.
    // Last, so the summit reads as snow over whatever else is live there.
    fx.Rules[3].MaterialSlot = kDefaultSnowSlot;
    fx.Rules[3].ConditionCount = 2;
    fx.Rules[3].Conditions[0] = NormalizedHeightBand(
        kDefaultSnowHeightMin, kDefaultNormalizedHeightDomainMax, kDefaultSnowHeightFeather);
    fx.Rules[3].Conditions[1] =
        SlopeBand(0.0f, kDefaultSnowSlopeMax, kDefaultSnowSlopeFeather);

    return fx;
}

} // namespace GameEngine::TerrainECS
