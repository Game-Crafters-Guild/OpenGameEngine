#pragma once

#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Types/Types.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::TerrainECS
{

// ---- Surface rule evaluation ------------------------------------------------
//
// Pure maths over one authored rule row: no terrain, no modifier, no splat. The
// bake supplies a texel's measurements and gets back a weight; what that weight
// then does to the splat belongs to the bake.
//
// Split out of the bake TU for two reasons. It is the part worth testing on its
// own — band edges, curve shapes, unit conversions and the AND product are all
// hand-checkable — and it is the part the GPU splat kernel has to reproduce
// expression for expression when rules reach it, which is far easier against a
// function than against a loop nested in a texel walk.

// A texel's measurements, computed once per texel by the bake and reused by
// every rule row and condition that reads them.
struct TerrainRuleSample
{
    // `1 - max(N.y, 0)` off the heightfield's own normal — 0 flat, 1 steep. NOT a
    // geometric cosine: the heightfield stores height normalized to [0, 1] while the
    // sample spacing is in metres, so this quantity is squashed by the terrain's
    // HeightScale and a threshold in it means a different ANGLE on every terrain.
    // Authored rows should band SlopeDegrees below; this is kept only because the
    // condition kind is shipped and a scene may already carry it.
    float32 SlopeNormalized = 0.0f;
    // The true surface angle from horizontal, in degrees over [0, 90] — measured
    // from the gradient with height scaled back into metres. Independently
    // computed rather than derived from SlopeNormalized, because the conversion
    // between them is the terrain's HeightScale and a rule row does not carry it.
    float32 SlopeDegrees = 0.0f;
    // Metres above the terrain's base — the normalized height sample times
    // HeightScale. NOT world-space Y: the terrain entity's own Y translation is
    // not folded in, matching the other modifier effects.
    float32 HeightMetres = 0.0f;
    // `(h - minH) / heightRange` against the terrain's live global range.
    float32 HeightNormalized = 0.0f;
    // World XZ, for the noise field.
    float32 WorldX = 0.0f;
    float32 WorldZ = 0.0f;
};

// Build a texel's measurements from what the bake already has in hand: the
// heightfield normal at the texel, the normalized height sample, and the two
// scales that turn normalized quantities into authorable ones.
//
// A function rather than nine assignments in the texel walk, because the unit
// conversions are the part most likely to drift from the GPU mirror and the
// mirror is far easier to hold against a function. It stays free of terrain and
// modifier types for the same reason the rest of this header does — everything
// it needs is a float.
//
// `heightRange` is the caller's already-guarded max(splatMaxH - splatMinH, eps);
// passing it in rather than re-deriving it keeps the one guard at the one site.
inline TerrainRuleSample MakeTerrainRuleSample(float32 normalX, float32 normalY, float32 normalZ,
                                               float32 sampleHeight, float32 heightScale,
                                               float32 splatMinHeight, float32 heightRange,
                                               float32 worldX, float32 worldZ)
{
    TerrainRuleSample sample{};
    sample.SlopeNormalized = 1.0f - std::max(normalY, 0.0f);
    // The gradient the normal came from, with height taken back into metres —
    // the angle a person means by "34 degrees". normalY is strictly positive (it
    // is 1 before normalizing).
    const float32 invY = 1.0f / normalY;
    const float32 gradX = -normalX * invY * heightScale;
    const float32 gradZ = -normalZ * invY * heightScale;
    sample.SlopeDegrees =
        std::atan(std::sqrt(gradX * gradX + gradZ * gradZ)) * (180.0f / 3.14159265f);
    // Terrain-LOCAL metres: the normalized sample scaled back up, with no terrain
    // entity translation folded in.
    sample.HeightMetres = sampleHeight * heightScale;
    sample.HeightNormalized = (sampleHeight - splatMinHeight) / heightRange;
    sample.WorldX = worldX;
    sample.WorldZ = worldZ;
    return sample;
}

// The condition's own measurement, in the unit its kind names. Noise is absent
// here because it is the one kind that needs a field sampled at (X, Z) rather
// than a value the texel already carries.
inline float32 TerrainRuleConditionValue(Components::TerrainRuleConditionKind kind,
                                         const TerrainRuleSample& sample)
{
    using Kind = Components::TerrainRuleConditionKind;
    switch (kind)
    {
    case Kind::SlopeNormalized:
        return sample.SlopeNormalized;
    case Kind::SlopeDegrees:
        return sample.SlopeDegrees;
    case Kind::HeightMetres:
        return sample.HeightMetres;
    case Kind::HeightNormalized:
        return sample.HeightNormalized;
    case Kind::Noise:
        break;
    }
    return 0.0f;
}

// Ramp shape across an already-clamped [0, 1] edge parameter.
inline float32 TerrainRuleCurve(Components::TerrainRuleFalloffCurve curve, float32 t)
{
    if (curve == Components::TerrainRuleFalloffCurve::Smoothstep)
        return t * t * (3.0f - 2.0f * t);
    return t;
}

// Weight [0, 1] for one condition given its already-measured value.
//
// 1 inside [Min, Max]; outside, a ramp of width Feather down to 0. Feather 0 is
// a hard edge. A degenerate band (Min == Max) is therefore a symmetric peak, and
// an inverted band (Min > Max) has no plateau — only the two ramps, which is the
// honest reading of an empty band rather than a silently corrected one.
inline float32 TerrainRuleConditionWeightForValue(const Components::TerrainRuleCondition& condition,
                                                  float32 value)
{
    if (value >= condition.Min && value <= condition.Max)
        return 1.0f;
    if (condition.Feather <= 0.0f)
        return 0.0f;

    const float32 distance = (value < condition.Min) ? (condition.Min - value)
                                                     : (value - condition.Max);
    const float32 t = std::clamp(1.0f - distance / condition.Feather, 0.0f, 1.0f);
    return TerrainRuleCurve(condition.FalloffCurve, t);
}

// Weight [0, 1] for one rule row: its conditions ANDed by multiplication, times
// the row's strength. A row with no conditions is unconditional.
//
// `noiseSampler` is invoked as `noise(worldX, worldZ, frequency, seed)` and only
// for Kind::Noise. It is a parameter rather than a call into the terrain noise
// basis so this header stays free of it — and so a test can drive a rule with a
// field whose value at a point is known by construction.
template <typename NoiseSampler>
float32 EvaluateTerrainSurfaceRuleWeight(const Components::TerrainSurfaceRule& rule,
                                         const TerrainRuleSample& sample,
                                         NoiseSampler&& noiseSampler)
{
    float32 weight = rule.Strength;
    const uint32 conditionCount =
        std::min<uint32>(rule.ConditionCount, Components::kMaxTerrainRuleConditions);

    for (uint32 i = 0; i < conditionCount && weight > 0.0f; ++i)
    {
        const Components::TerrainRuleCondition& condition = rule.Conditions[i];
        const float32 value =
            (condition.Kind == Components::TerrainRuleConditionKind::Noise)
                ? noiseSampler(sample.WorldX, sample.WorldZ, condition.NoiseFrequency,
                               condition.NoiseSeed)
                : TerrainRuleConditionValue(condition.Kind, sample);
        weight *= TerrainRuleConditionWeightForValue(condition, value);
    }

    return std::clamp(weight, 0.0f, 1.0f);
}

} // namespace GameEngine::TerrainECS
