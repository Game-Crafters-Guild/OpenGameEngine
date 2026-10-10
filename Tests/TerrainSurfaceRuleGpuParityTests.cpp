// CPU/GPU PARITY for terrain surface rules (F10 S3).
//
// The GPU splat bake composites authored rule rows on top of the procedural classification, and
// the CPU bake is the authority it must agree with. Nothing gated that agreement before this file:
// the only GPU-vs-CPU bake test in the tree was TerrainGpuSplatBake's oracle over the HARDCODED
// bake, which knows nothing about rules, and a DISABLED_ timing benchmark.
//
// These tests execute the SHIPPED shader code. The three marked blocks of
// Engine/Modules/CBTTerrain/Shaders/terrain_surface_rules.glsl are extracted verbatim at build
// time (ExtractShaderBlock.cmake) and compiled here through GlslShim.h, so an edit to the shader
// is an edit to what these tests measure. A hand-written C++ mirror would pass forever while the
// shader drifted underneath it, which is exactly the failure this arrangement exists to prevent.
//
// WHAT THIS GATE PROVES, AND WHAT IT DOES NOT. It proves the two SOURCES compute the same
// expressions: both arms run on the host, through the same compiler, so equality here is EXACT —
// EXPECT_EQ on floats and on bytes, not a tolerance. It does not prove the DEVICE agrees, because
// a GPU's transcendental functions (sin/cos/atan/sqrt/inversesqrt) are only accurate to a few ULP
// by specification. That second, weaker claim is the device oracle's
// (TerrainGpuSplatBake.SurfaceRulesMatchCpuDerivationWithinEpsilon), which bounds the residual at
// 1 RGBA8 LSB. The two are complementary: an exact source gate would not notice a driver's atan,
// and an epsilon device gate would not notice a wrong-but-close expression.
//
// The host stand-ins below (gSurfaceRules / gSurfaceRuleConditions) are the ONE thing here that is
// not shader source. They exist because the block addresses its rows in a buffer rather than
// copying them into locals — a dynamically indexed local array would spill the whole struct to
// per-invocation scratch on the GPU — so the host has to supply something with the same names.

#include <gtest/gtest.h>

#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainRuleNoise.h"
#include "TerrainECS/TerrainSplatComposite.h"
#include "TerrainECS/TerrainSurfaceRuleEval.h"

#include "GlslShim.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
// The shim's overloads have to be visible where the extracted block is compiled: ADL cannot find
// them for calls on plain floats and uints.
using GameEngine::GlslShim::abs;
using GameEngine::GlslShim::atan;
using GameEngine::GlslShim::clamp;
using GameEngine::GlslShim::cos;
using GameEngine::GlslShim::floor;
using GameEngine::GlslShim::max;
using GameEngine::GlslShim::min;
using GameEngine::GlslShim::sin;
using GameEngine::GlslShim::sqrt;
using GameEngine::GlslShim::uint;
using GameEngine::GlslShim::vec4;

// The shipped shader, compiled as C++. Order matters and mirrors the shader's own: types, then
// the buffers the rows are addressed in, then the maths, then the row functions that read both.
namespace Glsl
{
#include "TerrainSurfaceRuleTypesExtracted.h"

// Host stand-ins for the two SSBOs the splat kernel binds, with the names the block uses.
struct SurfaceRuleBufferHost
{
    std::vector<GESurfaceRule> Items;
};
struct SurfaceRuleConditionBufferHost
{
    std::vector<GESurfaceRuleCondition> Items;
};
SurfaceRuleBufferHost gSurfaceRules;
SurfaceRuleConditionBufferHost gSurfaceRuleConditions;

#include "TerrainSurfaceRuleMathExtracted.h"
#include "TerrainSurfaceRuleRowsExtracted.h"
} // namespace Glsl

using namespace GameEngine;
using namespace GameEngine::TerrainECS;
using Components::TerrainRuleCondition;
using Components::TerrainRuleConditionKind;
using Components::TerrainRuleFalloffCurve;
using Components::TerrainSurfaceRule;

// ---- Authoring helpers -----------------------------------------------------

TerrainRuleCondition Band(TerrainRuleConditionKind kind, float32 min, float32 max, float32 feather,
                          TerrainRuleFalloffCurve curve = TerrainRuleFalloffCurve::ClampedLinear,
                          float32 noiseFrequency = 0.02f, uint32 noiseSeed = 0)
{
    TerrainRuleCondition c{};
    c.Kind = kind;
    c.Min = min;
    c.Max = max;
    c.Feather = feather;
    c.FalloffCurve = curve;
    c.NoiseFrequency = noiseFrequency;
    c.NoiseSeed = noiseSeed;
    return c;
}

// A volume with the shape framing the cases share; its rows are attached by RulesVolume::Finish,
// which has to own them because ModifierSurfaceRulesParams carries a pointer.
ResolvedModifier MakeVolume(Components::TerrainModifierShape shape, bool global = false)
{
    ResolvedModifier mod{};
    mod.ModType = ResolvedModifier::Type::Volume;
    mod.Shape = shape;
    mod.GlobalScope = global;
    mod.Position = Mathematics::Vector3(10.0f, 0.0f, -20.0f);
    mod.YawRadians = 0.37f;
    mod.Radius = 40.0f;
    mod.RectHalfX = 30.0f;
    mod.RectHalfZ = 18.0f;
    mod.Falloff = 12.0f;
    mod.Weight = 1.0f;
    return mod;
}

// Storage for the rows a resolved effect points at: ModifierSurfaceRulesParams carries a pointer,
// so the vector has to outlive the pack.
struct RulesVolume
{
    ResolvedModifier Mod;
    std::vector<TerrainSurfaceRule> Rows;

    void Finish()
    {
        ResolvedEffect fx{};
        fx.EffectKind = ResolvedEffect::Kind::Rules;
        fx.Rules.Rules = Rows.data();
        fx.Rules.RuleCount = static_cast<uint32>(Rows.size());
        Mod.Effects.push_back(fx);
    }
};

// Pack `mods` and load the result into the host stand-ins, so the extracted block sees exactly the
// bytes the kernel would. Returns the packer's verdict.
bool LoadPackedRules(const std::vector<ResolvedModifier>& mods)
{
    std::vector<SurfaceRuleGpu> rules;
    std::vector<SurfaceRuleConditionGpu> conditions;
    const bool ok = PackSurfaceRulesForGpuSplat(mods, rules, conditions);

    Glsl::gSurfaceRules.Items.assign(rules.size(), Glsl::GESurfaceRule{});
    Glsl::gSurfaceRuleConditions.Items.assign(conditions.size(), Glsl::GESurfaceRuleCondition{});
    // The two structs are declared field for field in the two languages and both are trivially
    // copyable scalars, so a byte copy is the honest transfer: if the layouts ever disagree the
    // static_asserts on the C++ side and the size check below both fail loudly.
    static_assert(sizeof(Glsl::GESurfaceRule) == sizeof(SurfaceRuleGpu));
    static_assert(sizeof(Glsl::GESurfaceRuleCondition) == sizeof(SurfaceRuleConditionGpu));
    if (!rules.empty())
        std::memcpy(Glsl::gSurfaceRules.Items.data(), rules.data(),
                    rules.size() * sizeof(SurfaceRuleGpu));
    if (!conditions.empty())
        std::memcpy(Glsl::gSurfaceRuleConditions.Items.data(), conditions.data(),
                    conditions.size() * sizeof(SurfaceRuleConditionGpu));
    return ok;
}

Glsl::GESurfaceRuleSample ToGlsl(const TerrainRuleSample& s)
{
    Glsl::GESurfaceRuleSample g;
    g.SlopeNormalized = s.SlopeNormalized;
    g.SlopeDegrees = s.SlopeDegrees;
    g.HeightMetres = s.HeightMetres;
    g.HeightNormalized = s.HeightNormalized;
    g.WorldX = s.WorldX;
    g.WorldZ = s.WorldZ;
    return g;
}

// A spread of texel measurements wide enough that every condition kind lands inside, outside and
// on the feather of the bands the cases author.
std::vector<TerrainRuleSample> SweepSamples()
{
    std::vector<TerrainRuleSample> out;
    const float32 slopeNorms[] = {0.0f, 0.05f, 0.2f, 0.3f, 0.45f, 0.7f, 1.0f};
    const float32 heightNorms[] = {0.0f, 0.3f, 0.649f, 0.65f, 0.7f, 0.95f, 1.0f};
    const float32 worlds[] = {-1234.5f, -37.0f, 0.0f, 61.25f, 4096.0f};
    for (float32 sn : slopeNorms)
        for (float32 hn : heightNorms)
            for (float32 w : worlds)
            {
                TerrainRuleSample s{};
                s.SlopeNormalized = sn;
                s.SlopeDegrees = sn * 90.0f;
                s.HeightMetres = hn * 800.0f - 100.0f;
                s.HeightNormalized = hn;
                s.WorldX = w;
                s.WorldZ = -w * 0.5f + 13.0f;
                out.push_back(s);
            }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// 1 · The noise basis — the kind most likely to diverge, because it hashes
//     integers derived from a floor() and wraps uint arithmetic
// ---------------------------------------------------------------------------

TEST(TerrainSurfaceRuleGpuParity, NoiseFieldMatchesExactly)
{
    // Negative coordinates are the case that matters: int(floor(x)) and a bare truncation disagree
    // there, and a terrain routinely sits at negative world XZ.
    const float32 coords[] = {-4096.5f, -101.25f, -1.0f, -0.5f, 0.0f, 0.25f, 37.75f, 8192.0f};
    const float32 frequencies[] = {0.002f, 0.02f, 0.5f, 3.0f};
    const uint32 seeds[] = {0u, 1u, 12345u, 0xFFFFFFFFu};

    size_t compared = 0;
    for (float32 x : coords)
        for (float32 z : coords)
            for (float32 f : frequencies)
                for (uint32 seed : seeds)
                {
                    const float32 cpu = SurfaceRuleNoiseSample(x, z, f, seed);
                    const float32 gpu = Glsl::GE_SurfaceRuleNoise(x, z, f, seed);
                    ASSERT_EQ(cpu, gpu) << "noise divergence at (" << x << ", " << z
                                        << ") freq=" << f << " seed=" << seed;
                    ++compared;
                }
    EXPECT_EQ(compared, 1024u) << "the sweep did not run the case count it declares";
}

TEST(TerrainSurfaceRuleGpuParity, NoiseFieldIsNotConstant)
{
    // Teeth for the test above: two implementations that both return a constant would agree
    // perfectly and prove nothing.
    float32 lo = 2.0f, hi = -1.0f;
    for (int i = 0; i < 64; ++i)
    {
        const float32 v = Glsl::GE_SurfaceRuleNoise(static_cast<float32>(i) * 7.3f - 100.0f,
                                                    static_cast<float32>(i) * -3.1f, 0.05f, 7u);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    EXPECT_GT(hi - lo, 0.25f) << "the noise field barely varies; the parity sweep would be vacuous";
}

// ---------------------------------------------------------------------------
// 2 · The texel measurements — where the unit conventions live
// ---------------------------------------------------------------------------

TEST(TerrainSurfaceRuleGpuParity, SampleConstructionMatchesExactly)
{
    struct Case { float32 nx, ny, nz, h, scale, minH, range, wx, wz; };
    const Case cases[] = {
        {0.0f, 1.0f, 0.0f, 0.5f, 100.0f, 0.0f, 1.0f, 0.0f, 0.0f},          // flat
        {-0.6f, 0.7f, -0.39f, 0.25f, 64.0f, -0.1f, 1.2f, 512.0f, -128.0f}, // steep
        {0.3f, 0.95f, -0.1f, 0.9f, 800.0f, 0.2f, 0.7f, -4000.0f, 900.0f},  // high + gentle
        {0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.001f, 1.0f, 1.0f},          // degenerate range
    };
    for (const Case& c : cases)
    {
        const TerrainRuleSample cpu = MakeTerrainRuleSample(c.nx, c.ny, c.nz, c.h, c.scale,
                                                            c.minH, c.range, c.wx, c.wz);
        const Glsl::GESurfaceRuleSample gpu = Glsl::GE_MakeSurfaceRuleSample(
            c.nx, c.ny, c.nz, c.h, c.scale, c.minH, c.range, c.wx, c.wz);
        EXPECT_EQ(cpu.SlopeNormalized, gpu.SlopeNormalized);
        EXPECT_EQ(cpu.SlopeDegrees, gpu.SlopeDegrees);
        EXPECT_EQ(cpu.HeightMetres, gpu.HeightMetres);
        EXPECT_EQ(cpu.HeightNormalized, gpu.HeightNormalized);
        EXPECT_EQ(cpu.WorldX, gpu.WorldX);
        EXPECT_EQ(cpu.WorldZ, gpu.WorldZ);
    }
}

TEST(TerrainSurfaceRuleGpuParity, HeightMetresIsTerrainLocalNotWorldY)
{
    // The convention both arms state in comments, asserted rather than trusted: HeightMetres is
    // the normalized sample times HeightScale, with no terrain translation folded in. A mirror
    // that quietly added a world Y would still pass every parity sweep above, because both arms
    // would be handed the same numbers — this is the case that pins WHICH numbers.
    const float32 heightScale = 250.0f;
    const Glsl::GESurfaceRuleSample gpu =
        Glsl::GE_MakeSurfaceRuleSample(0.0f, 1.0f, 0.0f, 0.4f, heightScale, 0.0f, 1.0f,
                                       0.0f, 0.0f);
    EXPECT_FLOAT_EQ(gpu.HeightMetres, 0.4f * heightScale);
    EXPECT_FLOAT_EQ(gpu.HeightMetres, 100.0f);
}

// ---------------------------------------------------------------------------
// 3 · Row weight — bands, curves, the AND product and the caps
// ---------------------------------------------------------------------------

TEST(TerrainSurfaceRuleGpuParity, RowWeightMatchesExactlyAcrossTheConditionVocabulary)
{
    // One case per condition shape worth distinguishing. The bands are placed so the sweep's
    // samples land inside, on both feathers and outside each of them.
    std::vector<std::vector<TerrainRuleCondition>> conditionSets = {
        {},                                                                          // unconditional
        {Band(TerrainRuleConditionKind::SlopeNormalized, 0.0f, 0.333f, 0.0f)},       // hard edge
        {Band(TerrainRuleConditionKind::SlopeNormalized, 0.167f, 1.0f, 0.2f)},       // feathered
        {Band(TerrainRuleConditionKind::SlopeDegrees, 34.0f, 90.0f, 8.0f)},          // one-sided
        {Band(TerrainRuleConditionKind::HeightMetres, 400.0f, 1.0e9f, 60.0f)},       // metres
        {Band(TerrainRuleConditionKind::HeightNormalized, 0.65f, 1.0f, 0.1f)},       // normalized
        {Band(TerrainRuleConditionKind::SlopeNormalized, 0.3f, 0.3f, 0.2f)},         // degenerate peak
        {Band(TerrainRuleConditionKind::SlopeNormalized, 0.8f, 0.2f, 0.15f)},        // inverted
        {Band(TerrainRuleConditionKind::HeightNormalized, 0.4f, 0.9f, 0.2f,
              TerrainRuleFalloffCurve::Smoothstep)},                                 // smoothstep
        {Band(TerrainRuleConditionKind::Noise, 0.55f, 1.0f, 0.1f,
              TerrainRuleFalloffCurve::ClampedLinear, 0.013f, 99u)},                 // noise
        // Four conditions ANDing, mixing units and curves — the product, at the cap.
        {Band(TerrainRuleConditionKind::SlopeDegrees, 10.0f, 60.0f, 5.0f),
         Band(TerrainRuleConditionKind::HeightNormalized, 0.2f, 0.9f, 0.15f,
              TerrainRuleFalloffCurve::Smoothstep),
         Band(TerrainRuleConditionKind::Noise, 0.3f, 0.8f, 0.2f,
              TerrainRuleFalloffCurve::ClampedLinear, 0.05f, 3u),
         Band(TerrainRuleConditionKind::SlopeNormalized, 0.0f, 0.6f, 0.1f)},
    };

    const std::vector<TerrainRuleSample> samples = SweepSamples();
    size_t nonZero = 0, partial = 0, compared = 0;

    for (float32 strength : {1.0f, 0.5f, 0.0f})
    {
        for (const auto& conditions : conditionSets)
        {
            RulesVolume vol;
            vol.Mod = MakeVolume(Components::TerrainModifierShape::Rectangle, /*global*/ true);
            TerrainSurfaceRule rule{};
            rule.MaterialSlot = 1;
            rule.Strength = strength;
            rule.ConditionCount = static_cast<uint8>(conditions.size());
            for (size_t i = 0; i < conditions.size(); ++i)
                rule.Conditions[i] = conditions[i];
            vol.Rows.push_back(rule);
            vol.Finish();
            ASSERT_TRUE(LoadPackedRules({vol.Mod}));
            ASSERT_EQ(Glsl::gSurfaceRules.Items.size(), 1u);

            for (const TerrainRuleSample& s : samples)
            {
                const float32 cpu =
                    EvaluateTerrainSurfaceRuleWeight(rule, s, SurfaceRuleNoiseSample);
                const float32 gpu = Glsl::GE_EvaluateSurfaceRuleWeight(0u, ToGlsl(s));
                ASSERT_EQ(cpu, gpu)
                    << "row weight divergence: strength=" << strength
                    << " conditions=" << conditions.size() << " slopeNorm=" << s.SlopeNormalized
                    << " heightNorm=" << s.HeightNormalized << " worldX=" << s.WorldX;
                if (cpu > 0.0f)
                    ++nonZero;
                if (cpu > 0.0f && cpu < 1.0f)
                    ++partial;
                ++compared;
            }
        }
    }

    // Teeth: a sweep that only ever produced 0 (or only ever 1) would agree trivially. The partial
    // count is the one that matters — it is where the feather ramps and the AND product live.
    EXPECT_GT(compared, 5000u);
    EXPECT_GT(nonZero, 500u) << "the sweep never activated a rule; parity would be vacuous";
    EXPECT_GT(partial, 200u) << "the sweep never landed on a feather; the ramps are untested";
}

TEST(TerrainSurfaceRuleGpuParity, ConditionCountCapIsAppliedIdenticallyOnBothArms)
{
    // A row that declares more conditions than the cap must evaluate exactly the cap's worth on
    // both arms — the failure mode being one arm reading a neighbouring row's condition.
    RulesVolume vol;
    vol.Mod = MakeVolume(Components::TerrainModifierShape::Rectangle, /*global*/ true);
    TerrainSurfaceRule rule{};
    rule.Strength = 1.0f;
    rule.ConditionCount = static_cast<uint8>(Components::kMaxTerrainRuleConditions + 3u);
    for (uint32 i = 0; i < Components::kMaxTerrainRuleConditions; ++i)
        rule.Conditions[i] = Band(TerrainRuleConditionKind::SlopeNormalized, 0.0f, 0.9f, 0.05f);
    vol.Rows.push_back(rule);
    vol.Finish();
    ASSERT_TRUE(LoadPackedRules({vol.Mod}));
    ASSERT_EQ(Glsl::gSurfaceRules.Items[0].ConditionCount, Components::kMaxTerrainRuleConditions);

    for (const TerrainRuleSample& s : SweepSamples())
        ASSERT_EQ(EvaluateTerrainSurfaceRuleWeight(rule, s, SurfaceRuleNoiseSample),
                  Glsl::GE_EvaluateSurfaceRuleWeight(0u, ToGlsl(s)));
}

// ---------------------------------------------------------------------------
// 4 · Volume scope — the weight a row inherits from the volume it lives in
// ---------------------------------------------------------------------------

TEST(TerrainSurfaceRuleGpuParity, ShapeWeightMatchesComputeWeightExactly)
{
    // Outward is broken out because ShapeFalloffWeight BRANCHES on it: `falloff > 0 &&
    // falloffInward > 0` is one ramp across the edge, `falloff <= 0` outside the edge is a hard 0,
    // and `falloffInward <= 0` inside is a flat 1. A sweep that only ever authors a positive
    // outward falloff compares three of those arms by reading them, not by running them — and a
    // hard edge (Falloff 0) is a documented authoring mode, not a corner case.
    struct Shape
    {
        Components::TerrainModifierShape S;
        bool Global;
        float32 Outward, Inward, Master;
    };
    const Shape shapes[] = {
        {Components::TerrainModifierShape::Circle, false, 12.0f, 0.0f, 1.0f},
        {Components::TerrainModifierShape::Circle, false, 12.0f, 6.0f, 1.0f},  // both halves
        {Components::TerrainModifierShape::Circle, false, 12.0f, 0.0f, 0.35f}, // master strength
        {Components::TerrainModifierShape::Rectangle, false, 12.0f, 0.0f, 1.0f},
        {Components::TerrainModifierShape::Rectangle, false, 12.0f, 4.0f, 0.6f},
        {Components::TerrainModifierShape::Rectangle, true, 12.0f, 9.0f, 0.8f}, // global: no edge
        // Falloff 0 — a HARD edge. Binary in/out with no ramp at all.
        {Components::TerrainModifierShape::Circle, false, 0.0f, 0.0f, 1.0f},
        {Components::TerrainModifierShape::Rectangle, false, 0.0f, 0.0f, 0.5f},
        // Falloff 0 with an INWARD feather: no outer ramp, but a rim that rises to full strength
        // `Inward` metres inside the edge — the third branch, which neither of the above reaches.
        {Components::TerrainModifierShape::Circle, false, 0.0f, 8.0f, 1.0f},
        {Components::TerrainModifierShape::Rectangle, false, 0.0f, 5.0f, 0.75f},
    };

    size_t inside = 0, onRamp = 0, outside = 0, hardEdgeSamples = 0;
    for (const Shape& sh : shapes)
    {
        RulesVolume vol;
        vol.Mod = MakeVolume(sh.S, sh.Global);
        vol.Mod.Falloff = sh.Outward;
        vol.Mod.FalloffInward = sh.Inward;
        vol.Mod.Weight = sh.Master;
        TerrainSurfaceRule rule{};
        rule.Strength = 1.0f;
        vol.Rows.push_back(rule);
        vol.Finish();
        ASSERT_TRUE(LoadPackedRules({vol.Mod}));

        for (int ix = -12; ix <= 12; ++ix)
            for (int iz = -12; iz <= 12; ++iz)
            {
                const float32 wx = 10.0f + static_cast<float32>(ix) * 5.5f;
                const float32 wz = -20.0f + static_cast<float32>(iz) * 4.25f;
                const float32 cpu = ComputeWeight(vol.Mod, wx, wz);
                const float32 gpu =
                    Glsl::GE_SurfaceRuleShapeWeight(Glsl::gSurfaceRules.Items[0], wx, wz);
                ASSERT_EQ(cpu, gpu) << "shape weight divergence at (" << wx << ", " << wz << ")";
                if (cpu <= 0.0f)
                    ++outside;
                else if (cpu >= sh.Master)
                    ++inside;
                else
                    ++onRamp;
                if (sh.Outward <= 0.0f && !sh.Global)
                    ++hardEdgeSamples;
            }
    }
    // Teeth: all three regions must have been visited, or the comparison saw one branch only.
    EXPECT_GT(inside, 0u);
    EXPECT_GT(onRamp, 0u) << "no sample landed on a falloff ramp";
    EXPECT_GT(outside, 0u);
    EXPECT_GT(hardEdgeSamples, 0u) << "no hard-edge (Falloff 0) volume was swept";
}

TEST(TerrainSurfaceRuleGpuParity, AHardEdgeIsBinaryAndAnInwardRimStillRamps)
{
    // Discriminators for the case above: without these, a mirror that silently treated Falloff 0
    // as "some small ramp" would still pass the sweep wherever both arms agreed on the same wrong
    // value. These pin WHAT the falloff<=0 branches must produce, not merely that they agree.
    RulesVolume hard;
    hard.Mod = MakeVolume(Components::TerrainModifierShape::Circle);
    hard.Mod.Falloff = 0.0f;
    hard.Mod.FalloffInward = 0.0f;
    hard.Mod.Weight = 1.0f;
    TerrainSurfaceRule row{};
    row.Strength = 1.0f;
    hard.Rows.push_back(row);
    hard.Finish();
    ASSERT_TRUE(LoadPackedRules({hard.Mod}));

    // Radius is 40 about (10, -20): just inside is exactly 1, just outside exactly 0, with no
    // intermediate value anywhere along the crossing.
    const Glsl::GESurfaceRule& g = Glsl::gSurfaceRules.Items[0];
    EXPECT_FLOAT_EQ(Glsl::GE_SurfaceRuleShapeWeight(g, 10.0f + 39.5f, -20.0f), 1.0f);
    EXPECT_FLOAT_EQ(Glsl::GE_SurfaceRuleShapeWeight(g, 10.0f + 40.5f, -20.0f), 0.0f);
    size_t intermediate = 0;
    for (int i = 0; i <= 400; ++i)
    {
        const float32 wx = 10.0f + 38.0f + static_cast<float32>(i) * 0.01f;
        const float32 w = Glsl::GE_SurfaceRuleShapeWeight(g, wx, -20.0f);
        EXPECT_FLOAT_EQ(w, ComputeWeight(hard.Mod, wx, -20.0f));
        if (w > 0.0f && w < 1.0f)
            ++intermediate;
    }
    EXPECT_EQ(intermediate, 0u) << "a hard edge produced a ramp";

    // Falloff 0 + an inward feather: still 0 outside, but a rim that ramps inward.
    RulesVolume rim;
    rim.Mod = MakeVolume(Components::TerrainModifierShape::Circle);
    rim.Mod.Falloff = 0.0f;
    rim.Mod.FalloffInward = 8.0f;
    rim.Mod.Weight = 1.0f;
    rim.Rows.push_back(row);
    rim.Finish();
    ASSERT_TRUE(LoadPackedRules({rim.Mod}));
    const Glsl::GESurfaceRule& r = Glsl::gSurfaceRules.Items[0];
    EXPECT_FLOAT_EQ(Glsl::GE_SurfaceRuleShapeWeight(r, 10.0f + 40.5f, -20.0f), 0.0f);
    size_t rimRamp = 0;
    for (int i = 0; i <= 400; ++i)
    {
        const float32 wx = 10.0f + 32.0f + static_cast<float32>(i) * 0.02f;
        const float32 w = Glsl::GE_SurfaceRuleShapeWeight(r, wx, -20.0f);
        EXPECT_FLOAT_EQ(w, ComputeWeight(rim.Mod, wx, -20.0f));
        if (w > 0.0f && w < 1.0f)
            ++rimRamp;
    }
    EXPECT_GT(rimRamp, 50u) << "an inward feather produced no rim ramp";
}

// ---------------------------------------------------------------------------
// 5 · The splat write — byte for byte, through the quantization
// ---------------------------------------------------------------------------

TEST(TerrainSurfaceRuleGpuParity, SplatCompositeMatchesByteForByte)
{
    const std::array<std::array<uint8, 4>, 5> pixels = {{
        {{0, 0, 0, 0}},
        {{255, 0, 0, 0}},
        {{64, 64, 64, 63}},
        {{200, 30, 20, 5}},
        {{1, 2, 3, 250}},
    }};
    const float32 weights[] = {0.0f, 0.0009f, 0.25f, 0.5f, 0.7333f, 1.0f, 2.0f};

    size_t changed = 0, compared = 0;
    for (const auto& start : pixels)
        for (uint32 layer = 0; layer < 4; ++layer)
            for (float32 w : weights)
                for (bool replace : {false, true})
                {
                    uint8 cpu[4] = {start[0], start[1], start[2], start[3]};
                    CompositeSplatTexel(cpu, layer, w, replace);

                    vec4 gpu(static_cast<float>(start[0]), static_cast<float>(start[1]),
                             static_cast<float>(start[2]), static_cast<float>(start[3]));
                    gpu = Glsl::GE_CompositeSplatTexel(gpu, layer, w, replace);

                    for (int c = 0; c < 4; ++c)
                        ASSERT_EQ(static_cast<int>(cpu[c]), static_cast<int>(gpu[c]))
                            << "composite byte " << c << " diverged: layer=" << layer
                            << " weight=" << w << " replace=" << replace;
                    if (std::memcmp(cpu, start.data(), 4) != 0)
                        ++changed;
                    ++compared;
                }
    EXPECT_EQ(compared, 280u);
    EXPECT_GT(changed, 100u) << "the composite sweep barely wrote anything";
}

// The claim mask does not add a divergence point INSIDE the composite.
//
// Masking is applied to the weight BEFORE the composite is called — the CPU
// splat pass multiplies by (1 - claim) at the effect's own slot — so the shared
// expression is untouched by the feature and neither arm needed a new branch.
// This sweeps the masked weights the pass can actually produce (fully owned,
// part-owned, unowned) and pins that the two implementations still agree byte
// for byte on every one.
//
// The GPU arm never sees a claim-masked weight in a shipped bake, because the
// packer refuses a claim-respecting rules block outright
// (TerrainSurfaceRulesGpuGate.AClaimRespectingRulesVolumeRefusesTheGpuSplatPass).
// That refusal is what holds parity; this is the belt to its braces, and it is
// what would catch a later slice that wires a kernel twin and gets the ORDER of
// the mask and the composite wrong.
TEST(TerrainSurfaceRuleGpuParity, ClaimMaskedWeightsCompositeIdenticallyOnBothArms)
{
    const std::array<std::array<uint8, 4>, 4> pixels = {{
        {{0, 0, 0, 0}},
        {{255, 0, 0, 0}},
        {{64, 64, 64, 63}},
        {{200, 30, 20, 5}},
    }};
    // The authored row weight, before ownership is taken out of it.
    const float32 rowWeights[] = {0.25f, 0.6f, 1.0f};
    // Ownership as the buffer holds it: saturated into [0, 1].
    const float32 claims[] = {0.0f, 0.1f, 0.5f, 0.9999f, 1.0f};

    size_t compared = 0, fullClaimCases = 0, additiveDrifted = 0;
    int worstAdditiveDrift = 0;
    for (const auto& start : pixels)
        for (uint32 layer = 0; layer < 4; ++layer)
            for (float32 rowWeight : rowWeights)
                for (float32 owned : claims)
                    for (bool replace : {false, true})
                    {
                        const float32 masked = rowWeight * (1.0f - owned);

                        uint8 cpu[4] = {start[0], start[1], start[2], start[3]};
                        CompositeSplatTexel(cpu, layer, masked, replace);

                        vec4 gpu(static_cast<float>(start[0]), static_cast<float>(start[1]),
                                 static_cast<float>(start[2]), static_cast<float>(start[3]));
                        gpu = Glsl::GE_CompositeSplatTexel(gpu, layer, masked, replace);

                        for (int c = 0; c < 4; ++c)
                            ASSERT_EQ(static_cast<int>(cpu[c]), static_cast<int>(gpu[c]))
                                << "claim-masked composite byte " << c << " diverged: layer="
                                << layer << " rowWeight=" << rowWeight << " claim=" << owned
                                << " replace=" << replace;

                        if (owned == 1.0f)
                        {
                            ++fullClaimCases;
                            if (replace)
                            {
                                // lerp by zero, channel by channel: an exact no-op.
                                EXPECT_EQ(0, std::memcmp(cpu, start.data(), 4))
                                    << "the replace arm at zero weight must be exact";
                            }
                            else if (std::memcmp(cpu, start.data(), 4) != 0)
                            {
                                // The additive arm RENORMALIZES, so it is not a
                                // no-op at zero weight even though it adds
                                // nothing: dividing by a channel sum that is not
                                // exactly 1.0f in float and truncating back to
                                // uint8 can drop an LSB. Bounded, and reported.
                                ++additiveDrifted;
                                for (int c = 0; c < 4; ++c)
                                    worstAdditiveDrift = std::max(
                                        worstAdditiveDrift,
                                        std::abs(static_cast<int>(cpu[c])
                                                 - static_cast<int>(start[c])));
                            }
                        }
                        ++compared;
                    }

    std::printf("[CLAIM-COMPOSITE] additive arm re-quantized %zu of %zu zero-weight cases, "
                "worst drift %d LSB\n",
                additiveDrifted, fullClaimCases / 2, worstAdditiveDrift);
    std::fflush(stdout);

    EXPECT_EQ(compared, 4u * 4u * 3u * 5u * 2u);
    EXPECT_EQ(fullClaimCases, 4u * 4u * 3u * 2u)
        << "every fully-claimed case must have been exercised, or the mask arm is not covered";
    EXPECT_LE(worstAdditiveDrift, 1)
        << "zero-weight renormalization must stay within one LSB; more than that is a composite "
           "bug, not quantization";
}

// WHY THE ZERO-WEIGHT EARLY-OUT IN THE SPLAT PASS IS LOAD-BEARING.
//
// The composite's ADDITIVE arm renormalizes, so calling it with zero weight is
// NOT a no-op on a texel whose channels do not divide 255 exactly — it can drop
// an LSB, and a region bake replays the whole stack, so a claimed texel under a
// deferring additive stroke would drift a little further every bake.
//
// Both apply paths therefore early-out on `weight <= 0` BEFORE compositing, and
// that is what makes "a fully claimed texel is left exactly as it was" true at
// the pass level (TerrainSplatClaim.ClaimedGroundKeepsTheMaterialItsClaimantPainted
// measures it end to end). This test is the counter-example that keeps the
// early-out from looking like an optimization someone may delete.
TEST(TerrainSurfaceRuleGpuParity, TheAdditiveArmIsNotANoOpAtZeroWeight)
{
    // Channels that do not survive a divide-and-truncate round trip.
    const uint8 start[4] = {64, 64, 64, 63};

    uint8 composited[4] = {start[0], start[1], start[2], start[3]};
    CompositeSplatTexel(composited, /*layerIdx=*/0, /*weight=*/0.0f, /*replace=*/false);

    EXPECT_NE(0, std::memcmp(composited, start, 4))
        << "if the additive arm ever becomes an exact no-op at zero weight, the splat pass's "
           "`weight <= 0` early-out stops being load-bearing and this test should be deleted "
           "along with the comment that cites it";

    // The replace arm, for contrast, IS exact at zero.
    uint8 replaced[4] = {start[0], start[1], start[2], start[3]};
    CompositeSplatTexel(replaced, /*layerIdx=*/0, /*weight=*/0.0f, /*replace=*/true);
    EXPECT_EQ(0, std::memcmp(replaced, start, 4));
}

// ---------------------------------------------------------------------------
// 6 · THE GATE — a full stack of rows over a texel, both arms, byte compare
// ---------------------------------------------------------------------------

TEST(TerrainSurfaceRuleGpuParity, WholeRowStackProducesIdenticalSplatBytes)
{
    // Two volumes, four rows between them, mixing shapes, both paint semantics, every condition
    // kind and both curves — the shape a real authored terrain has. The CPU arm below is the
    // shipped ApplySplatModifiers row loop, expression for expression; the GPU arm is the shipped
    // shader. Every row composites THROUGH the byte quantization, which is the property that makes
    // an exact byte comparison possible rather than an epsilon.
    RulesVolume ground;
    ground.Mod = MakeVolume(Components::TerrainModifierShape::Rectangle, /*global*/ true);
    ground.Mod.Weight = 0.9f;
    {
        TerrainSurfaceRule r{};
        r.MaterialSlot = 0;
        r.Strength = 1.0f;
        r.ConditionCount = 1;
        r.Conditions[0] = Band(TerrainRuleConditionKind::SlopeNormalized, 0.0f, 0.333f, 0.0f);
        ground.Rows.push_back(r);

        TerrainSurfaceRule snow{};
        snow.MaterialSlot = 3;
        snow.Strength = 0.8f;
        snow.ConditionCount = 2;
        snow.Conditions[0] = Band(TerrainRuleConditionKind::HeightNormalized, 0.65f, 1.0f, 0.1f,
                                  TerrainRuleFalloffCurve::Smoothstep);
        snow.Conditions[1] = Band(TerrainRuleConditionKind::SlopeDegrees, 0.0f, 40.0f, 10.0f);
        ground.Rows.push_back(snow);
    }
    ground.Finish();

    RulesVolume patch;
    patch.Mod = MakeVolume(Components::TerrainModifierShape::Circle);
    patch.Mod.FalloffInward = 5.0f;
    patch.Mod.Weight = 0.75f;
    {
        TerrainSurfaceRule rock{};
        rock.MaterialSlot = 1;
        rock.Strength = 1.0f;
        rock.ConditionCount = 1;
        rock.Conditions[0] = Band(TerrainRuleConditionKind::Noise, 0.5f, 1.0f, 0.25f,
                                  TerrainRuleFalloffCurve::ClampedLinear, 0.03f, 17u);
        patch.Rows.push_back(rock);

        TerrainSurfaceRule road{};
        road.MaterialSlot = 2;
        road.Strength = 1.0f;
        road.Replace = true; // the other paint arm
        road.ConditionCount = 1;
        road.Conditions[0] = Band(TerrainRuleConditionKind::HeightMetres, -1.0e9f, 0.25f, 0.1f);
        patch.Rows.push_back(road);
    }
    patch.Finish();

    ASSERT_TRUE(LoadPackedRules({ground.Mod, patch.Mod}));
    ASSERT_EQ(Glsl::gSurfaceRules.Items.size(), 4u);

    // A SMALL height scale is what puts SlopeDegrees inside the snow row's band: the gradient it
    // is taken from is normalized-height per world metre, so a large scale drives every sample to
    // ~89 degrees and the row never fires. (Found by red-arming: with a large scale the
    // two-condition row was inert, and a broken AND product went unnoticed here.)
    const float32 heightScale = 0.5f;
    const float32 splatMinH = -0.1f;
    const float32 heightRange = 1.2f;
    const std::array<uint8, 4> base = {{140, 60, 40, 15}}; // a plausible already-baked texel

    // Per-row activation counts: a row that never fires contributes nothing to the comparison, so
    // "the bytes matched" would be a statement about the other rows only.
    std::array<size_t, 4> rowFired = {0, 0, 0, 0};
    std::array<size_t, 4> rowPartial = {0, 0, 0, 0};
    size_t changed = 0, compared = 0;
    for (int ix = -14; ix <= 14; ++ix)
    {
        for (int iz = -14; iz <= 14; ++iz)
        {
            const float32 wx = 10.0f + static_cast<float32>(ix) * 4.5f;
            const float32 wz = -20.0f + static_cast<float32>(iz) * 3.5f;
            // A normal and a height that vary across the sweep, so rows switch on and off.
            const float32 ny = 0.55f + 0.4f * std::sin(wx * 0.02f) * std::cos(wz * 0.017f);
            const float32 nx = std::sqrt(std::max(0.0f, 1.0f - ny * ny)) * 0.8f;
            const float32 nz = std::sqrt(std::max(0.0f, 1.0f - ny * ny - nx * nx));
            const float32 sampleH = 0.5f + 0.45f * std::sin(wx * 0.01f + wz * 0.013f);

            const TerrainRuleSample sample = MakeTerrainRuleSample(
                nx, ny, nz, sampleH, heightScale, splatMinH, heightRange, wx, wz);

            // CPU arm — the ApplySplatModifiers row loop.
            uint8 cpu[4] = {base[0], base[1], base[2], base[3]};
            size_t rowIndex = 0;
            for (const RulesVolume* vol : {&ground, &patch})
            {
                const float32 volumeWeight =
                    ComputeWeight(vol->Mod, wx, wz);
                for (const TerrainSurfaceRule& row : vol->Rows)
                {
                    const size_t thisRow = rowIndex++;
                    if (volumeWeight <= 0.0f)
                        continue;
                    const float32 weight =
                        EvaluateTerrainSurfaceRuleWeight(row, sample, SurfaceRuleNoiseSample) *
                        volumeWeight;
                    if (weight <= 0.0f)
                        continue;
                    ++rowFired[thisRow];
                    if (weight < 1.0f)
                        ++rowPartial[thisRow];
                    CompositeSplatTexel(cpu, row.MaterialSlot, weight, row.Replace);
                }
            }

            // GPU arm — the shipped shader over the packed rows, in buffer order.
            vec4 gpu(static_cast<float>(base[0]), static_cast<float>(base[1]),
                     static_cast<float>(base[2]), static_cast<float>(base[3]));
            const Glsl::GESurfaceRuleSample gs = ToGlsl(sample);
            for (uint32 r = 0; r < Glsl::gSurfaceRules.Items.size(); ++r)
                gpu = Glsl::GE_ApplySurfaceRuleRow(gpu, r, gs);

            for (int c = 0; c < 4; ++c)
                ASSERT_EQ(static_cast<int>(cpu[c]), static_cast<int>(gpu[c]))
                    << "splat byte " << c << " diverged at (" << wx << ", " << wz << ")";
            if (std::memcmp(cpu, base.data(), 4) != 0)
                ++changed;
            ++compared;
        }
    }

    EXPECT_EQ(compared, 841u);
    // Teeth: if no texel changed, both arms agreed on "do nothing" and the gate proved nothing.
    EXPECT_GT(changed, 200u) << "the rule stack barely touched the splat; the gate is vacuous";
    for (size_t r = 0; r < rowFired.size(); ++r)
    {
        EXPECT_GT(rowFired[r], 10u) << "row " << r << " never fired — it is not under test here";
        EXPECT_GT(rowPartial[r], 5u)
            << "row " << r << " only ever fired at full weight; its ramp is not under test";
    }
}

// ---------------------------------------------------------------------------
// 7 · The packer — order, contents, and what it refuses
// ---------------------------------------------------------------------------

TEST(TerrainSurfaceRuleGpuParity, RowsPackInPriorityThenStackOrder)
{
    // The kernel applies rows in buffer order and cannot re-sort them, so the packer's order IS
    // the composite order. Two volumes with an interleaved stack: the flattened rows must come out
    // volume-by-volume in the order the resolved list holds them, and rows within a volume in the
    // order its effect stack holds them.
    RulesVolume first;
    first.Mod = MakeVolume(Components::TerrainModifierShape::Rectangle, /*global*/ true);
    TerrainSurfaceRule a{};
    a.MaterialSlot = 0;
    a.Strength = 0.11f;
    TerrainSurfaceRule b{};
    b.MaterialSlot = 1;
    b.Strength = 0.22f;
    first.Rows = {a, b};
    first.Finish();

    RulesVolume second;
    second.Mod = MakeVolume(Components::TerrainModifierShape::Circle);
    TerrainSurfaceRule c{};
    c.MaterialSlot = 2;
    c.Strength = 0.33f;
    second.Rows = {c};
    second.Finish();

    std::vector<SurfaceRuleGpu> rules;
    std::vector<SurfaceRuleConditionGpu> conditions;
    ASSERT_TRUE(PackSurfaceRulesForGpuSplat({first.Mod, second.Mod}, rules, conditions));
    ASSERT_EQ(rules.size(), 3u);
    EXPECT_FLOAT_EQ(rules[0].Strength, 0.11f);
    EXPECT_FLOAT_EQ(rules[1].Strength, 0.22f);
    EXPECT_FLOAT_EQ(rules[2].Strength, 0.33f);
    EXPECT_EQ(rules[0].Shape, static_cast<uint32>(SurfaceRuleGpuShape::Global));
    EXPECT_EQ(rules[2].Shape, static_cast<uint32>(SurfaceRuleGpuShape::Circle));
}

TEST(TerrainSurfaceRuleGpuParity, ConditionBaseAddressesEachRowsOwnConditions)
{
    RulesVolume vol;
    vol.Mod = MakeVolume(Components::TerrainModifierShape::Rectangle, /*global*/ true);
    TerrainSurfaceRule one{};
    one.ConditionCount = 1;
    one.Conditions[0] = Band(TerrainRuleConditionKind::SlopeDegrees, 1.0f, 2.0f, 0.5f);
    TerrainSurfaceRule three{};
    three.ConditionCount = 3;
    three.Conditions[0] = Band(TerrainRuleConditionKind::HeightMetres, 10.0f, 20.0f, 1.0f);
    three.Conditions[1] = Band(TerrainRuleConditionKind::HeightNormalized, 0.1f, 0.2f, 0.01f);
    three.Conditions[2] = Band(TerrainRuleConditionKind::Noise, 0.3f, 0.4f, 0.05f);
    vol.Rows = {one, three};
    vol.Finish();

    std::vector<SurfaceRuleGpu> rules;
    std::vector<SurfaceRuleConditionGpu> conditions;
    ASSERT_TRUE(PackSurfaceRulesForGpuSplat({vol.Mod}, rules, conditions));
    ASSERT_EQ(rules.size(), 2u);
    ASSERT_EQ(conditions.size(), 4u);
    EXPECT_EQ(rules[0].ConditionBase, 0u);
    EXPECT_EQ(rules[0].ConditionCount, 1u);
    EXPECT_EQ(rules[1].ConditionBase, 1u);
    EXPECT_EQ(rules[1].ConditionCount, 3u);
    EXPECT_FLOAT_EQ(conditions[0].Min, 1.0f);
    EXPECT_FLOAT_EQ(conditions[1].Min, 10.0f);
    EXPECT_FLOAT_EQ(conditions[3].Min, 0.3f);
    EXPECT_EQ(conditions[3].Kind, static_cast<uint32>(TerrainRuleConditionKind::Noise));
}

TEST(TerrainSurfaceRuleGpuParity, MaterialSlotIsClampedTheSameWayTheCpuClampsIt)
{
    RulesVolume vol;
    vol.Mod = MakeVolume(Components::TerrainModifierShape::Rectangle, /*global*/ true);
    TerrainSurfaceRule r{};
    r.MaterialSlot = 99;
    vol.Rows.push_back(r);
    vol.Finish();

    std::vector<SurfaceRuleGpu> rules;
    std::vector<SurfaceRuleConditionGpu> conditions;
    ASSERT_TRUE(PackSurfaceRulesForGpuSplat({vol.Mod}, rules, conditions));
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].MaterialSlot, Terrain::kMaxTerrainMaterialLayers - 1u);
}
