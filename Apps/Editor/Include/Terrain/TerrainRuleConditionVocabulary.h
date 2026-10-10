#pragma once

#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Types/Types.h"

#include <string>
#include <string_view>

namespace GameEngine::Editor::TerrainRuleVocabulary
{

// What a surface-rule condition is CALLED, what unit it reads in, what domain a
// band spans, and what a row that writes nothing has to say for itself.
//
// Free of UI types on purpose. Every string here is authoring copy that a test
// pins by phrase, and a widget that had to be laid out to read its own labels
// would put that copy out of reach — EditorTests runs no layout pass.
//
// The one word this file exists to keep out of the Inspector is "world Y".
// HeightMetres is measured from the TERRAIN'S BASE: the terrain entity's own Y
// translation is not part of it, so a band does not move when the terrain does.
// Calling it world Y would be wrong in exactly the way an author only discovers
// after moving a terrain and watching the snow line stay put.

using Kind = Components::TerrainRuleConditionKind;
using Curve = Components::TerrainRuleFalloffCurve;

// The band's authoring range for a kind — the ends of the slider the handles
// travel between, not a clamp on the stored value.
struct ConditionDomain
{
    float32 Min = 0.0f;
    float32 Max = 1.0f;
};

// Dropdown label. Both slope kinds and both height kinds name their UNIT,
// because the pair differ only by it and picking the wrong one is silent.
std::string_view ConditionKindLabel(Kind kind);

// Unit shown after a value: "°", " m", or nothing for the [0, 1] kinds.
std::string_view ConditionUnitSuffix(Kind kind);

// Tooltip copy: what the kind measures, and for the two migration-exact units,
// why they are here at all rather than the one the author wants.
std::string_view ConditionKindHelp(Kind kind);

// `terrainHeightMetres` is the terrain's HeightScale — the top of the metres
// domain. Zero or negative falls back to a usable range rather than a
// zero-width slider no handle can move on.
ConditionDomain ConditionAuthoringDomain(Kind kind, float32 terrainHeightMetres);

// Digits this kind shows, in a caption AND in a numeric field. ONE source, so a
// caption reading "34" can never sit beside a field reading "34.4": that is the
// same disagreement two roundings produce, one step smaller.
int ConditionDecimals(Kind kind);

// A value with its unit, at that precision.
std::string FormatConditionValue(Kind kind, float32 value);

// Just the band's endpoints, for a control that has room for the numbers but not
// the sentence: "0 - 35".
std::string ConditionBandRangeText(const Components::TerrainRuleCondition& condition);

// A freshly added condition of this kind, spanning its kind's WHOLE domain.
//
// Not the component's zero-initialized default: that is [0, 1], which for a
// degrees band is [0 deg, 1 deg] — very nearly the empty band, and the one
// almost-inert state with no notice attached, because the row is neither
// rule-less nor condition-less. Adding a condition should be a no-op until the
// author narrows it, so the band starts wide open.
Components::TerrainRuleCondition MakeDefaultCondition(Kind kind, float32 terrainHeightMetres);

// One sentence describing the band the way the evaluator reads it: where the
// weight is 1, and how far past each edge it takes to reach 0, followed by one
// sentence saying what `owningRule` does with that weight.
//
// The reach is stated as a WIDTH rather than as the two absolute values it
// implies, because those land outside the domain for any band touching an end
// (a slope band to 90° with a 12° feather "reaches" 102°) and a number no
// handle can travel to reads as a bug.
//
// The rule is passed whole rather than as the two fields read from it, because a
// caller holding a rule can never hand this its strength and its mode from
// different rows — and the mode is what decides whether "full weight" is a claim
// about sharing the texel or about taking it.
//
// DECISION, because the alternative is defensible and this one has to be stated:
// the strip and this sentence describe THE CONDITION, not the rule. Rule strength
// multiplies the product of every condition in the row, so scaling each
// condition's own plot by it would show one multiplication happening N times for
// an N-condition rule. Instead the plot stays the condition's weight and the
// closing sentence carries everything the rule does to it.
std::string ConditionBandSummary(const Components::TerrainRuleCondition& condition,
                                 const Components::TerrainSurfaceRule& owningRule);

// Why this row writes nothing, or empty when it writes.
//
// Only states the two conditions that are ALWAYS zero for every possible texel —
// a zero strength, and a band with no plateau and no feather. A band that merely
// sits outside the terrain's current heights is not here: it is a function of
// the heightfield, not of the row, and a warning that clears when someone
// sculpts is a warning nobody trusts.
std::string RuleInertReason(const Components::TerrainSurfaceRule& rule);

// What an unconditional row does. A row with no conditions is not broken — it is
// the honest reading of "nothing restricts me" — but it covers the whole volume,
// which is a surprise worth spending a line on.
std::string_view UnconditionalRuleNote();

// The Surface Rules section's standing help: how rows combine, and what they do
// to what is already on the ground.
//
// The last sentence is the one an author needs BEFORE the first bake rather than
// after it: a rules effect composites into the same splat the volume's other
// effects wrote, so a rule can take back ground a paint layer earlier in the
// stack put down, and nothing else in the panel says so.
std::string_view SurfaceRulesSectionHelp();

} // namespace GameEngine::Editor::TerrainRuleVocabulary
