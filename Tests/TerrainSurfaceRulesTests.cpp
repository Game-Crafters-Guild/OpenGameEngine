// Terrain SURFACE RULES: authored condition rows that assign materials — the only
// thing that places material on a terrain (terrain-surface-rules-design.html §3-§4).
//
// Two kinds of oracle here, in the order they are worth trusting:
//
//  1. EVALUATOR — pure maths over one row. Band edges, curve shapes, unit kinds
//     and the AND product are all hand-checkable, so they are checked against
//     hand-computed numbers rather than against a second implementation.
//
//  2. BAKE — that a rules effect reaches the splatmap, composites with paint's
//     exact semantics, honours stack order, and refuses the GPU splat path.
//     The paint effect is the oracle: a rule row with NO conditions is by
//     definition a paint stroke, so the two must bake byte-identically.

#include <gtest/gtest.h>

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "SplineECS/SplineService.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainSurfaceRuleEval.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;
using Components::TerrainRuleCondition;
using Components::TerrainRuleConditionKind;
using Components::TerrainRuleFalloffCurve;
using Components::TerrainSurfaceRule;
using Components::TerrainSurfaceRulesEffect;

// The noise field a rule sees in the evaluator tests: a value known by
// construction at every point, so an assertion pins the plumbing (does the
// condition reach the sampler with ITS frequency and seed?) rather than the
// terrain's fBM. Encodes its inputs into the returned value.
float32 ProbeNoise(float32 worldX, float32 worldZ, float32 frequency, uint32 seed)
{
    return std::clamp(worldX * 0.001f + worldZ * 0.002f + frequency + static_cast<float32>(seed),
                      0.0f, 1.0f);
}

float32 ZeroNoise(float32, float32, float32, uint32) { return 0.0f; }

TerrainRuleCondition Band(TerrainRuleConditionKind kind, float32 min, float32 max, float32 feather,
                          TerrainRuleFalloffCurve curve = TerrainRuleFalloffCurve::ClampedLinear)
{
    TerrainRuleCondition c{};
    c.Kind = kind;
    c.Min = min;
    c.Max = max;
    c.Feather = feather;
    c.FalloffCurve = curve;
    return c;
}

TerrainRuleSample SampleWithSlopeNormalized(float32 slope)
{
    TerrainRuleSample s{};
    s.SlopeNormalized = slope;
    return s;
}

TerrainRuleSample SampleWithHeightNormalized(float32 heightNorm)
{
    TerrainRuleSample s{};
    s.HeightNormalized = heightNorm;
    return s;
}

// ---- Bake harness ---------------------------------------------------------
// Mirrors TerrainModifierVolumeTests: one complete bake over its own terrain and
// services, so two arms differing only in HOW the surface is authored compare
// without their handles colliding.

constexpr float32 kWorldSize = 256.0f;
constexpr float32 kHeightScale = 64.0f;
constexpr uint32 kHeightmapDim = 129;
constexpr float32 kVolumeRadius = 70.0f;
constexpr float32 kVolumeFalloff = 15.0f;

struct ScopedTerrainService
{
    ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
    }
    ~ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }
};

struct ScopedSplineService
{
    ScopedSplineService()
    {
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
        SplineECS::SplineService::Initialize();
    }
    ~ScopedSplineService()
    {
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
    }
};

Terrain::TerrainConfig MakeTestConfig(uint32 dim = kHeightmapDim)
{
    Terrain::TerrainConfig cfg{};
    cfg.HeightmapWidth = dim;
    cfg.HeightmapHeight = dim;
    cfg.WorldSizeX = kWorldSize;
    cfg.WorldSizeZ = kWorldSize;
    cfg.HeightScale = kHeightScale;
    cfg.LODLevels = 4;
    return cfg;
}

TerrainHandle CreateBakedTerrain(TerrainService& svc, uint32 dim = kHeightmapDim)
{
    const TerrainHandle handle = svc.CreateTerrain(MakeTestConfig(dim));
    auto* data = svc.GetTerrainData(handle);
    FillHeightfieldBaseRegion(data->Heightfield, Components::TerrainBaseSource::ProceduralNoise,
                              nullptr, 0, 0,
                              static_cast<int32>(dim) - 1,
                              static_cast<int32>(dim) - 1);
    data->MarkFullDirty();
    svc.RebuildQuadtree(handle);
    data->ResetSplatmapAndCommitRange();
    return handle;
}

Components::WorldTransform IdentityAt(float32 x, float32 y, float32 z)
{
    Components::WorldTransform xf{};
    xf.matrix[12] = x;
    xf.matrix[13] = y;
    xf.matrix[14] = z;
    return xf;
}

ECS::EntityHandle CreateTerrainEntity(ECS::World& world, TerrainHandle handle)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = kWorldSize;
    terrain.SizeZ = kWorldSize;
    terrain.HeightScale = kHeightScale;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    terrain.BaseSource = Components::TerrainBaseSource::ProceduralNoise;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    return e;
}

// A circle volume centred on the terrain, big enough that its footprint covers a
// wide spread of slopes and heights.
ECS::EntityHandle CreateCircleVolume(ECS::World& world)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Circle;
    vol.Radius = kVolumeRadius;
    vol.Falloff = kVolumeFalloff;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
    return e;
}

struct BakeArm
{
    std::vector<uint8> BaseSplat;
    std::vector<uint8> BakedSplat;
};

BakeArm Bake(const std::function<void(ECS::World&)>& author)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();

    BakeArm arm{};
    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    if (!data)
        return arm;
    arm.BaseSplat = data->Splatmap;

    ECS::World world;
    CreateTerrainEntity(world, handle);
    author(world);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    arm.BakedSplat = data->Splatmap;
    return arm;
}

bool BytewiseEqual(const std::vector<uint8>& a, const std::vector<uint8>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// Every engine log line a bake emits, so a cap refusal can be asserted on
// directly instead of through whatever the overflow happens to read.
//
// Two ways this instrument reads a false zero, both armed against here: the
// logger's effective level is Off until it is configured, so an unconfigured
// capture makes a "did not warn" assertion vacuously true; and delivery is
// asynchronous, so a read that does not Flush() first races the drain thread.
class ScopedLogCapture
{
  public:
    ScopedLogCapture()
    {
        Logger::Log::Initialize({Logger::LogLevel::Info, false});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        m_Sink = sink.get();
        Logger::Log::AddSink(std::move(sink));
        m_CallbackId = m_Sink->RegisterCallback([this](const Logger::LogMessage& message) {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Lines.push_back(std::string(message.Message));
        });
    }

    ~ScopedLogCapture() { m_Sink->UnregisterCallback(m_CallbackId); }

    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

    std::size_t CountContaining(const std::string& needle)
    {
        Logger::Log::Flush();
        std::lock_guard<std::mutex> lock(m_Mutex);
        return static_cast<std::size_t>(
            std::count_if(m_Lines.begin(), m_Lines.end(), [&needle](const std::string& line) {
                return line.find(needle) != std::string::npos;
            }));
    }

  private:
    Logger::CallbackSink* m_Sink = nullptr;
    uint64 m_CallbackId = 0;
    std::mutex m_Mutex;
    std::vector<std::string> m_Lines;
};

// The SET of texels the bake changed. An aggregate, because a hand-picked texel
// can read unmodified however well the rule resolved.
std::size_t ChangedTexelCount(const std::vector<uint8>& before, const std::vector<uint8>& after)
{
    std::size_t changed = 0;
    for (std::size_t i = 0; i + 3 < before.size() && i + 3 < after.size(); i += 4)
        if (std::memcmp(&before[i], &after[i], 4) != 0)
            ++changed;
    return changed;
}

} // namespace

// ---------------------------------------------------------------------------
// 1 · Condition evaluation
// ---------------------------------------------------------------------------

TEST(TerrainSurfaceRuleCondition, InsideTheBandIsFullWeight)
{
    const auto band = Band(TerrainRuleConditionKind::SlopeDegrees, 20.0f, 40.0f, 5.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 20.0f), 1.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 30.0f), 1.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 40.0f), 1.0f);
}

TEST(TerrainSurfaceRuleCondition, ZeroFeatherIsAHardEdge)
{
    const auto band = Band(TerrainRuleConditionKind::SlopeDegrees, 20.0f, 40.0f, 0.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 40.0f), 1.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 40.001f), 0.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 19.999f), 0.0f);
}

TEST(TerrainSurfaceRuleCondition, FeatherRampsLinearlyOutsideBothEdges)
{
    const auto band = Band(TerrainRuleConditionKind::SlopeDegrees, 20.0f, 40.0f, 10.0f);
    // Half a feather past each edge is half weight; a full feather is zero.
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 45.0f), 0.5f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 15.0f), 0.5f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 50.0f), 0.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 10.0f), 0.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 100.0f), 0.0f);
}

TEST(TerrainSurfaceRuleCondition, DegenerateBandIsATriangularPeak)
{
    // Min == Max with a feather is a triangular peak: the shape a row takes when it
    // has. It only works because the feather ramps OUTSIDE the band.
    //
    // The ramp foot is NEAR zero, not exactly zero, whenever the band and the
    // feather are not exactly representable: 0.3f + 0.2f is 0.5f - 1 ulp, so the
    // ramp has a sliver left at 0.5. Asserted as such rather than papered over —
    // the exactly-representable case below shows the expression itself is clean,
    // and a residue of 6e-8 cannot survive quantization to a uint8 channel.
    const auto band = Band(TerrainRuleConditionKind::SlopeNormalized, 0.3f, 0.3f, 0.2f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 0.3f), 1.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 0.4f), 0.5f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 0.2f), 0.5f);
    EXPECT_NEAR(TerrainRuleConditionWeightForValue(band, 0.5f), 0.0f, 1e-6f);
    EXPECT_NEAR(TerrainRuleConditionWeightForValue(band, 0.1f), 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(band, 0.55f), 0.0f);

    // Binary-exact band and feather: the ramp foot lands on exactly zero.
    const auto exact = Band(TerrainRuleConditionKind::SlopeNormalized, 0.25f, 0.25f, 0.25f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(exact, 0.5f), 0.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(exact, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(exact, 0.375f), 0.5f);
}

TEST(TerrainSurfaceRuleCondition, SmoothstepCurveDiffersFromLinearAwayFromTheMidpoint)
{
    const auto linear = Band(TerrainRuleConditionKind::SlopeDegrees, 0.0f, 0.0f, 10.0f);
    const auto smooth = Band(TerrainRuleConditionKind::SlopeDegrees, 0.0f, 0.0f, 10.0f,
                             TerrainRuleFalloffCurve::Smoothstep);
    // Both curves pin the endpoints and the midpoint...
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(smooth, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(smooth, 10.0f), 0.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(smooth, 5.0f),
                    TerrainRuleConditionWeightForValue(linear, 5.0f));
    // ...and separate everywhere else, which is the whole point of the choice.
    // t = 0.75 -> linear 0.75, smoothstep 0.84375.
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(linear, 2.5f), 0.75f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(smooth, 2.5f), 0.84375f);
}

TEST(TerrainSurfaceRuleCondition, EachKindReadsItsOwnMeasurement)
{
    TerrainRuleSample sample{};
    sample.SlopeNormalized = 0.25f;
    sample.SlopeDegrees = 34.0f;
    sample.HeightMetres = 410.0f;
    sample.HeightNormalized = 0.8f;

    EXPECT_FLOAT_EQ(TerrainRuleConditionValue(TerrainRuleConditionKind::SlopeNormalized, sample), 0.25f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionValue(TerrainRuleConditionKind::SlopeDegrees, sample), 34.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionValue(TerrainRuleConditionKind::HeightMetres, sample), 410.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionValue(TerrainRuleConditionKind::HeightNormalized, sample), 0.8f);
}

TEST(TerrainSurfaceRuleCondition, MetresAndNormalizedHeightAreIndependentInputs)
{
    // The pair that makes a permanent migration possible: a metres band and a
    // normalized band over the SAME texel answer differently, because one tracks
    // the terrain's live range and the other does not.
    TerrainRuleSample sample{};
    sample.HeightMetres = 410.0f;
    sample.HeightNormalized = 0.2f;

    const auto metres = Band(TerrainRuleConditionKind::HeightMetres, 400.0f, 500.0f, 0.0f);
    const auto normalized = Band(TerrainRuleConditionKind::HeightNormalized, 0.6f, 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(
                        metres, TerrainRuleConditionValue(metres.Kind, sample)), 1.0f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(
                        normalized, TerrainRuleConditionValue(normalized.Kind, sample)), 0.0f);
}

// ---------------------------------------------------------------------------
// 2 · Rule evaluation — conditions AND by multiplying
// ---------------------------------------------------------------------------

TEST(TerrainSurfaceRule, ConditionsMultiplyRatherThanMinOrAdd)
{
    // Two conditions each at exactly half weight. The product is 0.25; the two
    // plausible wrong answers are min (0.5) and a clamped sum (1.0), and both
    // are excluded by the same assertion.
    TerrainSurfaceRule rule{};
    rule.ConditionCount = 2;
    rule.Conditions[0] = Band(TerrainRuleConditionKind::SlopeNormalized, 0.0f, 0.0f, 0.4f);
    rule.Conditions[1] = Band(TerrainRuleConditionKind::HeightNormalized, 0.0f, 0.0f, 0.6f);

    TerrainRuleSample sample{};
    sample.SlopeNormalized = 0.2f;    // half of the 0.4 feather
    sample.HeightNormalized = 0.3f;   // half of the 0.6 feather

    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(rule.Conditions[0], 0.2f), 0.5f);
    EXPECT_FLOAT_EQ(TerrainRuleConditionWeightForValue(rule.Conditions[1], 0.3f), 0.5f);
    EXPECT_FLOAT_EQ(EvaluateTerrainSurfaceRuleWeight(rule, sample, ZeroNoise), 0.25f);
}

TEST(TerrainSurfaceRule, ThreeConditionsCompoundTheirFalloffs)
{
    TerrainSurfaceRule rule{};
    rule.ConditionCount = 3;
    rule.Conditions[0] = Band(TerrainRuleConditionKind::SlopeNormalized, 0.0f, 0.0f, 0.4f);
    rule.Conditions[1] = Band(TerrainRuleConditionKind::HeightNormalized, 0.0f, 0.0f, 0.6f);
    rule.Conditions[2] = Band(TerrainRuleConditionKind::HeightMetres, 0.0f, 0.0f, 100.0f);

    TerrainRuleSample sample{};
    sample.SlopeNormalized = 0.2f;
    sample.HeightNormalized = 0.3f;
    sample.HeightMetres = 50.0f;
    EXPECT_FLOAT_EQ(EvaluateTerrainSurfaceRuleWeight(rule, sample, ZeroNoise), 0.125f);
}

TEST(TerrainSurfaceRule, AnyZeroConditionZerosTheRow)
{
    TerrainSurfaceRule rule{};
    rule.ConditionCount = 2;
    rule.Conditions[0] = Band(TerrainRuleConditionKind::SlopeNormalized, 0.0f, 0.2f, 0.0f);
    rule.Conditions[1] = Band(TerrainRuleConditionKind::HeightNormalized, 0.0f, 1.0f, 0.0f);

    TerrainRuleSample sample{};
    sample.SlopeNormalized = 0.9f; // outside a hard band
    sample.HeightNormalized = 0.5f;
    EXPECT_FLOAT_EQ(EvaluateTerrainSurfaceRuleWeight(rule, sample, ZeroNoise), 0.0f);
}

TEST(TerrainSurfaceRule, NoConditionsIsUnconditional)
{
    TerrainSurfaceRule rule{};
    rule.ConditionCount = 0;
    EXPECT_FLOAT_EQ(EvaluateTerrainSurfaceRuleWeight(rule, TerrainRuleSample{}, ZeroNoise), 1.0f);
}

TEST(TerrainSurfaceRule, StrengthScalesTheProduct)
{
    TerrainSurfaceRule rule{};
    rule.Strength = 0.5f;
    rule.ConditionCount = 1;
    rule.Conditions[0] = Band(TerrainRuleConditionKind::SlopeNormalized, 0.0f, 0.0f, 0.4f);

    EXPECT_FLOAT_EQ(EvaluateTerrainSurfaceRuleWeight(rule, SampleWithSlopeNormalized(0.2f), ZeroNoise),
                    0.25f);
}

TEST(TerrainSurfaceRule, ConditionCountAboveTheCapEvaluatesOnlyTheStoredConditions)
{
    // The evaluator is the last line of defence for a count that reached it by
    // some route the loader and gather did not police: it must read the array,
    // never past it.
    TerrainSurfaceRule rule{};
    rule.ConditionCount = 200;
    for (uint32 i = 0; i < Components::kMaxTerrainRuleConditions; ++i)
        rule.Conditions[i] = Band(TerrainRuleConditionKind::SlopeNormalized, 0.0f, 1.0f, 0.0f);

    EXPECT_FLOAT_EQ(EvaluateTerrainSurfaceRuleWeight(rule, SampleWithSlopeNormalized(0.5f), ZeroNoise),
                    1.0f);
}

TEST(TerrainSurfaceRule, NoiseConditionReachesTheSamplerWithItsOwnFrequencyAndSeed)
{
    TerrainSurfaceRule rule{};
    rule.ConditionCount = 1;
    auto noise = Band(TerrainRuleConditionKind::Noise, 0.0f, 1.0f, 0.0f);
    noise.NoiseFrequency = 0.25f;
    noise.NoiseSeed = 0;
    rule.Conditions[0] = noise;

    TerrainRuleSample sample{};
    sample.WorldX = 100.0f;
    sample.WorldZ = 200.0f;
    // ProbeNoise returns 0.1 + 0.4 + 0.25 = 0.75, inside the [0,1] band.
    EXPECT_FLOAT_EQ(EvaluateTerrainSurfaceRuleWeight(rule, sample, ProbeNoise), 1.0f);

    // A band that excludes that value zeroes the row — so the sampler's answer
    // is genuinely being banded, not ignored.
    rule.Conditions[0].Min = 0.0f;
    rule.Conditions[0].Max = 0.5f;
    EXPECT_FLOAT_EQ(EvaluateTerrainSurfaceRuleWeight(rule, sample, ProbeNoise), 0.0f);
}

// ---------------------------------------------------------------------------
// 3 · Bake integration
// ---------------------------------------------------------------------------

TEST(TerrainSurfaceRulesBake, AnUnconditionalRowBakesExactlyLikeThePaintEffectItIs)
{
    // A rule row with no conditions is a paint stroke by definition: its weight
    // is the volume ramp times its strength, which is precisely what paint
    // applies. Byte-identity is therefore the right assertion, and it is what
    // pins rules to paint's blend/replace semantics rather than a copy of them.
    for (const bool replace : {false, true})
    {
        const BakeArm paint = Bake([&](ECS::World& w) {
            const auto volume = CreateCircleVolume(w);
            Components::TerrainPaintLayerEffect fx{};
            fx.LayerIndex = 2;
            fx.Strength = 0.6f;
            fx.Replace = replace;
            w.AddComponentImmediate<Components::TerrainPaintLayerEffect>(volume, fx);
        });

        const BakeArm rules = Bake([&](ECS::World& w) {
            const auto volume = CreateCircleVolume(w);
            TerrainSurfaceRulesEffect fx{};
            fx.RuleCount = 1;
            fx.Rules[0].MaterialSlot = 2;
            fx.Rules[0].Strength = 0.6f;
            fx.Rules[0].Replace = replace;
            fx.Rules[0].ConditionCount = 0;
            w.AddComponentImmediate<TerrainSurfaceRulesEffect>(volume, fx);
        });

        ASSERT_FALSE(paint.BakedSplat.empty());
        EXPECT_GT(ChangedTexelCount(paint.BaseSplat, paint.BakedSplat), 0u)
            << "the control arm must actually paint something";
        EXPECT_TRUE(BytewiseEqual(paint.BakedSplat, rules.BakedSplat))
            << "replace=" << replace;
    }
}

TEST(TerrainSurfaceRulesBake, AConditionRestrictsWhereTheRowWrites)
{
    const BakeArm unconditional = Bake([](ECS::World& w) {
        const auto volume = CreateCircleVolume(w);
        TerrainSurfaceRulesEffect fx{};
        fx.RuleCount = 1;
        fx.Rules[0].MaterialSlot = 1;
        fx.Rules[0].Replace = true;
        w.AddComponentImmediate<TerrainSurfaceRulesEffect>(volume, fx);
    });

    const BakeArm banded = Bake([](ECS::World& w) {
        const auto volume = CreateCircleVolume(w);
        TerrainSurfaceRulesEffect fx{};
        fx.RuleCount = 1;
        fx.Rules[0].MaterialSlot = 1;
        fx.Rules[0].Replace = true;
        fx.Rules[0].ConditionCount = 1;
        // Only the upper part of the terrain's range.
        fx.Rules[0].Conditions[0] =
            Band(TerrainRuleConditionKind::HeightNormalized, 0.75f, 1.0f, 0.05f);
        w.AddComponentImmediate<TerrainSurfaceRulesEffect>(volume, fx);
    });

    const std::size_t all = ChangedTexelCount(unconditional.BaseSplat, unconditional.BakedSplat);
    const std::size_t some = ChangedTexelCount(banded.BaseSplat, banded.BakedSplat);
    EXPECT_GT(all, 0u);
    EXPECT_GT(some, 0u) << "the band must still admit part of the terrain";
    EXPECT_LT(some, all) << "a height band must write strictly fewer texels than no band at all";
}

TEST(TerrainSurfaceRulesBake, RulesAndPaintCompositeInStackOrder)
{
    // Same two effects on one volume, only their stack order differs. Paint-last
    // and rules-last must not agree, or the order the inspector shows is a lie.
    auto authorAt = [](int32 rulesOrder, int32 paintOrder) {
        return [rulesOrder, paintOrder](ECS::World& w) {
            const auto volume = CreateCircleVolume(w);
            TerrainSurfaceRulesEffect rules{};
            rules.StackOrder = rulesOrder;
            rules.RuleCount = 1;
            rules.Rules[0].MaterialSlot = 1;
            rules.Rules[0].Replace = true;
            rules.Rules[0].Strength = 0.7f;
            w.AddComponentImmediate<TerrainSurfaceRulesEffect>(volume, rules);

            Components::TerrainPaintLayerEffect paint{};
            paint.StackOrder = paintOrder;
            paint.LayerIndex = 3;
            paint.Replace = true;
            paint.Strength = 0.7f;
            w.AddComponentImmediate<Components::TerrainPaintLayerEffect>(volume, paint);
        };
    };

    const BakeArm rulesFirst = Bake(authorAt(0, 1));
    const BakeArm paintFirst = Bake(authorAt(1, 0));

    EXPECT_GT(ChangedTexelCount(rulesFirst.BaseSplat, rulesFirst.BakedSplat), 0u);
    EXPECT_FALSE(BytewiseEqual(rulesFirst.BakedSplat, paintFirst.BakedSplat))
        << "swapping StackOrder between a rules effect and a paint effect must change the bake";
}

TEST(TerrainSurfaceRulesBake, RuleCountAboveTheCapIsRefusedOutLoudAndBakesTheStoredRows)
{
    // The clamp is asserted through the ERROR IT LOGS, not through the bytes the
    // overflow reads. Byte-identity alone does not gate this: rows past the cap
    // land on whatever follows the array, that memory is zeroed in practice, and
    // a zero Strength contributes nothing — so identity holds with the clamp
    // removed, by allocator accident rather than by the code being right.
    //
    // Nor does a second rules volume fix that. Each resolved block is ~900 bytes
    // and MSVC's deque puts one element per block above 8 bytes, so two authored
    // blocks are separate allocations; making the overflow land on a neighbour's
    // live rows would be another accident, not a gate.
    //
    // The log line is layout-independent, and the capped arm is the negative
    // control that keeps the assertion honest: an unconditional warning fails it.
    auto author = [](uint32 declaredCount) {
        return [declaredCount](ECS::World& w) {
            const auto volume = CreateCircleVolume(w);
            TerrainSurfaceRulesEffect fx{};
            fx.RuleCount = declaredCount;
            for (uint32 i = 0; i < Components::kMaxTerrainSurfaceRules; ++i)
            {
                fx.Rules[i].MaterialSlot = i % 4u;
                fx.Rules[i].Strength = 0.25f;
            }
            w.AddComponentImmediate<TerrainSurfaceRulesEffect>(volume, fx);
        };
    };

    ScopedLogCapture log;

    const BakeArm capped = Bake(author(Components::kMaxTerrainSurfaceRules));
    EXPECT_EQ(log.CountContaining("declares"), 0u) << "the honest count must not warn";

    const BakeArm overflowing = Bake(author(Components::kMaxTerrainSurfaceRules + 4u));
    EXPECT_EQ(log.CountContaining("rules; the cap is"), 1u)
        << "an over-cap count must be refused by name, once";

    EXPECT_GT(ChangedTexelCount(capped.BaseSplat, capped.BakedSplat), 0u);
    EXPECT_TRUE(BytewiseEqual(capped.BakedSplat, overflowing.BakedSplat));
}

TEST(TerrainSurfaceRulesBake, ConditionCountAboveTheCapIsRefusedOutLoud)
{
    // The condition cap has its own message, because its consequence is the
    // opposite of the rule cap's: dropped conditions widen a rule instead of
    // removing it, so the rule paints ground it was never authored to cover.
    ScopedLogCapture log;

    const BakeArm honest = Bake([](ECS::World& w) {
        const auto volume = CreateCircleVolume(w);
        TerrainSurfaceRulesEffect fx{};
        fx.RuleCount = 1;
        fx.Rules[0].MaterialSlot = 1;
        fx.Rules[0].Strength = 1.0f;
        fx.Rules[0].ConditionCount = static_cast<uint8>(Components::kMaxTerrainRuleConditions);
        w.AddComponentImmediate<TerrainSurfaceRulesEffect>(volume, fx);
    });
    EXPECT_EQ(log.CountContaining("declares"), 0u) << "the honest count must not warn";

    const BakeArm overflowing = Bake([](ECS::World& w) {
        const auto volume = CreateCircleVolume(w);
        TerrainSurfaceRulesEffect fx{};
        fx.RuleCount = 1;
        fx.Rules[0].MaterialSlot = 1;
        fx.Rules[0].Strength = 1.0f;
        fx.Rules[0].ConditionCount = static_cast<uint8>(Components::kMaxTerrainRuleConditions + 3u);
        w.AddComponentImmediate<TerrainSurfaceRulesEffect>(volume, fx);
    });
    EXPECT_EQ(log.CountContaining("conditions; the cap is"), 1u)
        << "an over-cap condition count must be refused by name, once";

    EXPECT_TRUE(BytewiseEqual(honest.BakedSplat, overflowing.BakedSplat));
}

TEST(TerrainSurfaceRulesBake, ACapRefusalWarnsAgainAfterTheCountIsFixedAndRebroken)
{
    // The warning is deduped per entity so a persistently over-cap volume does
    // not spam every bake. Dedupe that never resets would report the SECOND
    // breakage as clean, which is the bake the author is looking at.
    ScopedLogCapture log;

    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);

    ECS::World world;
    CreateTerrainEntity(world, handle);
    const auto volume = CreateCircleVolume(world);
    TerrainSurfaceRulesEffect fx{};
    fx.RuleCount = Components::kMaxTerrainSurfaceRules + 4u;
    fx.Rules[0].MaterialSlot = 1;
    fx.Rules[0].Strength = 1.0f;
    world.AddComponentImmediate<TerrainSurfaceRulesEffect>(volume, fx);

    // One system across all three bakes: the dedupe lives on the system, so a
    // fresh one per bake would pass this test without any reset logic at all.
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(log.CountContaining("rules; the cap is"), 1u) << "first breakage warns";

    system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(log.CountContaining("rules; the cap is"), 1u) << "still broken: no spam";

    world.GetComponentForWrite<TerrainSurfaceRulesEffect>(volume)->RuleCount = 1;
    system.Update(world, 1.0f / 60.0f);

    world.GetComponentForWrite<TerrainSurfaceRulesEffect>(volume)->RuleCount =
        Components::kMaxTerrainSurfaceRules + 4u;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(log.CountContaining("rules; the cap is"), 2u) << "rebroken: warns again";
}

TEST(TerrainSurfaceRulesBake, MaterialSlotAboveTheChannelCountClampsRatherThanCorrupts)
{
    const BakeArm lastChannel = Bake([](ECS::World& w) {
        const auto volume = CreateCircleVolume(w);
        TerrainSurfaceRulesEffect fx{};
        fx.RuleCount = 1;
        fx.Rules[0].MaterialSlot = Terrain::kMaxTerrainMaterialLayers - 1u;
        fx.Rules[0].Replace = true;
        w.AddComponentImmediate<TerrainSurfaceRulesEffect>(volume, fx);
    });

    const BakeArm outOfRange = Bake([](ECS::World& w) {
        const auto volume = CreateCircleVolume(w);
        TerrainSurfaceRulesEffect fx{};
        fx.RuleCount = 1;
        fx.Rules[0].MaterialSlot = 99u;
        fx.Rules[0].Replace = true;
        w.AddComponentImmediate<TerrainSurfaceRulesEffect>(volume, fx);
    });

    EXPECT_GT(ChangedTexelCount(lastChannel.BaseSplat, lastChannel.BakedSplat), 0u);
    EXPECT_TRUE(BytewiseEqual(lastChannel.BakedSplat, outOfRange.BakedSplat));
}

// An authoring mistake gets one warning per bake, however the splat pass is
// split: whole on the calling thread, or in row bands across a pool (a 513 x 513
// terrain is 17 bands), each of which runs the stack that holds the mistake.
// A global paint volume at `layer` on `world`, the authoring mistake the clamp warns about.
ECS::EntityHandle AddOutOfRangePaint(ECS::World& world, uint32 layer)
{
    const auto volume = world.CreateEntity();
    Components::TerrainModifierVolume global{};
    global.Shape = Components::TerrainVolumeShape::Global;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(volume, global);
    world.AddComponentImmediate<Components::WorldTransform>(volume, IdentityAt(0.0f, 0.0f, 0.0f));
    Components::TerrainPaintLayerEffect paint{};
    paint.LayerIndex = layer;
    paint.Strength = 1.0f;
    world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(volume, paint);
    return volume;
}

// The once-per-update latch opens again for the next update that bakes: a mistake
// still present at a later bake is reported again, never once per system lifetime.
TEST(TerrainSurfaceRulesBake, AnOutOfRangePaintLayerWarnsAgainAtTheNextBake)
{
    ScopedLogCapture log;
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    const TerrainHandle handle = CreateBakedTerrain(TerrainService::Get());
    ECS::World world;
    CreateTerrainEntity(world, handle);
    const auto volume = AddOutOfRangePaint(world, 99u);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    world.GetComponentForWrite<Components::TerrainPaintLayerEffect>(volume)->Strength = 0.5f;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(log.CountContaining("layer index 99"), 2u) << "one warning per bake that meets the mistake";
}

// Each distinct mistake is named once per update: a second out-of-range layer in
// the same bake is its own warning, not swallowed by the first.
TEST(TerrainSurfaceRulesBake, EachOutOfRangePaintLayerIsNamedInTheSameBake)
{
    ScopedLogCapture log;
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    const TerrainHandle handle = CreateBakedTerrain(TerrainService::Get());
    ECS::World world;
    CreateTerrainEntity(world, handle);
    AddOutOfRangePaint(world, 99u);
    AddOutOfRangePaint(world, 77u);
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(log.CountContaining("layer index 99"), 1u);
    EXPECT_EQ(log.CountContaining("layer index 77"), 1u);
}

TEST(TerrainSurfaceRulesBake, AnOutOfRangePaintLayerWarnsOncePerBakeWholeOrInBands)
{
    const auto warningsFromOneBake = [](JobSystem::WorkStealingThreadPool* pool) {
        ScopedLogCapture log;
        ScopedTerrainService terrainScope;
        ScopedSplineService splineScope;
        const TerrainHandle handle = CreateBakedTerrain(TerrainService::Get(), 513);
        ECS::World world;
        if (pool)
            world.SetJobSystem(pool);
        CreateTerrainEntity(world, handle);
        const auto volume = world.CreateEntity();
        Components::TerrainModifierVolume global{};
        global.Shape = Components::TerrainVolumeShape::Global;
        world.AddComponentImmediate<Components::TerrainModifierVolume>(volume, global);
        world.AddComponentImmediate<Components::WorldTransform>(volume, IdentityAt(0.0f, 0.0f, 0.0f));
        Components::TerrainPaintLayerEffect paint{};
        paint.LayerIndex = 99u;
        paint.Strength = 1.0f;
        world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(volume, paint);
        TerrainModifierSystem system;
        system.Update(world, 1.0f / 60.0f);
        return log.CountContaining("layer index 99");
    };

    EXPECT_EQ(warningsFromOneBake(nullptr), 1u) << "whole";
    std::size_t pooled = 0;
    {
        JobSystem::WorkStealingThreadPool pool(4);
        JobSystem::JobCounter counter;
        std::promise<void> started;
        auto workerStarted = started.get_future();
        pool.Run([&]() {
            started.set_value();
            pooled = warningsFromOneBake(&pool);
        }, counter);
        // The bake runs on a worker, as the scheduler runs it; the main thread only joins.
        workerStarted.wait();
        pool.Wait(counter);
    }
    EXPECT_EQ(pooled, 1u) << "in row bands across the pool";
}

TEST(TerrainSurfaceRulesBake, ADisabledRulesEffectBakesNothing)
{
    const BakeArm disabled = Bake([](ECS::World& w) {
        const auto volume = CreateCircleVolume(w);
        TerrainSurfaceRulesEffect fx{};
        fx.Enabled = false;
        fx.RuleCount = 1;
        fx.Rules[0].MaterialSlot = 1;
        fx.Rules[0].Replace = true;
        w.AddComponentImmediate<TerrainSurfaceRulesEffect>(volume, fx);
    });
    EXPECT_EQ(ChangedTexelCount(disabled.BaseSplat, disabled.BakedSplat), 0u);
}

// ---------------------------------------------------------------------------
// 5 · The GPU splat gate — what the kernel can express, and what it cannot
// ---------------------------------------------------------------------------

TEST(TerrainSurfaceRulesGpuGate, ARulesVolumeIsGpuExpressible)
{
    // The splat kernel evaluates rule rows out of an SSBO, so a rules volume no
    // longer forces the CPU splat: the packer accepts it and hands the kernel
    // its rows. (Value parity between the two arms is
    // TerrainSurfaceRuleGpuParityTests' subject, not this file's.)
    TerrainSurfaceRule row{};
    row.MaterialSlot = 2;
    row.Strength = 0.5f;

    ResolvedModifier rulesVolume{};
    rulesVolume.ModType = ResolvedModifier::Type::Volume;
    rulesVolume.Shape = Components::TerrainModifierShape::Circle;
    rulesVolume.Radius = 10.0f;
    ResolvedEffect fx{};
    fx.EffectKind = ResolvedEffect::Kind::Rules;
    fx.Rules.Rules = &row;
    fx.Rules.RuleCount = 1;
    rulesVolume.Effects.push_back(fx);

    EXPECT_TRUE(ResolvedEffect::IsSplat(ResolvedEffect::Kind::Rules));
    EXPECT_TRUE(rulesVolume.TouchesSplat());

    std::vector<SurfaceRuleGpu> rules;
    std::vector<SurfaceRuleConditionGpu> conditions;
    EXPECT_TRUE(PackSurfaceRulesForGpuSplat({rulesVolume}, rules, conditions));
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].MaterialSlot, 2u);
    EXPECT_FLOAT_EQ(rules[0].Strength, 0.5f);
}

TEST(TerrainSurfaceRulesGpuGate, PaintStillRefusesTheGpuSplatPass)
{
    // Paint is the writer the kernel has no twin for — no mask sampler, no zone
    // payload — so a paint-carrying bake must still keep the CPU splat, and must
    // pack nothing.
    std::vector<SurfaceRuleGpu> rules;
    std::vector<SurfaceRuleConditionGpu> conditions;

    ResolvedModifier paintZone{};
    paintZone.ModType = ResolvedModifier::Type::PaintZone;
    EXPECT_FALSE(PackSurfaceRulesForGpuSplat({paintZone}, rules, conditions));
    EXPECT_TRUE(rules.empty());

    // A paint EFFECT on a volume refuses too, and takes its sibling rules rows
    // with it: the two interleave by stack order, so half the composite on the
    // GPU would be worse than none of it.
    TerrainSurfaceRule row{};
    ResolvedModifier mixed{};
    mixed.ModType = ResolvedModifier::Type::Volume;
    ResolvedEffect rulesFx{};
    rulesFx.EffectKind = ResolvedEffect::Kind::Rules;
    rulesFx.Rules.Rules = &row;
    rulesFx.Rules.RuleCount = 1;
    ResolvedEffect paintFx{};
    paintFx.EffectKind = ResolvedEffect::Kind::PaintLayer;
    mixed.Effects = {rulesFx, paintFx};
    EXPECT_FALSE(PackSurfaceRulesForGpuSplat({mixed}, rules, conditions));
    EXPECT_TRUE(rules.empty()) << "a refusal must leave no half-packed rows behind";
}

// THE SECOND PARITY SURFACE, held by a refusal rather than by a kernel twin.
//
// Claim masking is a CPU-side accumulate ACROSS the modifier stack — ownership
// folded in at each claiming volume's own slot — and the splat kernel evaluates
// one rule row per texel with no ownership buffer to read. There is no expression
// for GE_CompositeSplatTexel to mirror, so the packer must refuse: a packed
// claim-respecting block would paint straight through claimed ground on the GPU
// and stop at it on the CPU, and the settle re-splat would then disagree with the
// frame the author was looking at.
TEST(TerrainSurfaceRulesGpuGate, AClaimRespectingRulesVolumeRefusesTheGpuSplatPass)
{
    TerrainSurfaceRule row{};
    row.MaterialSlot = 2;
    row.Strength = 1.0f;

    const auto makeVolume = [&row](bool respectClaims) {
        ResolvedModifier vol{};
        vol.ModType = ResolvedModifier::Type::Volume;
        vol.Shape = Components::TerrainModifierShape::Circle;
        vol.Radius = 10.0f;
        ResolvedEffect fx{};
        fx.EffectKind = ResolvedEffect::Kind::Rules;
        fx.Rules.Rules = &row;
        fx.Rules.RuleCount = 1;
        fx.Rules.RespectClaims = respectClaims;
        vol.Effects.push_back(fx);
        return vol;
    };

    std::vector<SurfaceRuleGpu> rules;
    std::vector<SurfaceRuleConditionGpu> conditions;

    EXPECT_FALSE(PackSurfaceRulesForGpuSplat({makeVolume(true)}, rules, conditions))
        << "a rules block that stops at claimed ground has no kernel twin and must refuse";
    EXPECT_TRUE(rules.empty()) << "a refusal must leave no half-packed rows behind";
    EXPECT_TRUE(conditions.empty());

    // THE DISCRIMINATOR: the identical volume with the flag off still packs, so
    // the refusal is about the claim mask and not about rules volumes at large.
    EXPECT_TRUE(PackSurfaceRulesForGpuSplat({makeVolume(false)}, rules, conditions));
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].MaterialSlot, 2u);

    // And one claim-respecting block takes the whole batch with it: half the
    // composite on the GPU would be worse than none of it.
    EXPECT_FALSE(PackSurfaceRulesForGpuSplat({makeVolume(false), makeVolume(true)},
                                             rules, conditions));
    EXPECT_TRUE(rules.empty());
}

TEST(TerrainSurfaceRulesGpuGate, ASplineShapedRulesVolumeRefusesTheGpuSplatPass)
{
    // The kernel knows circles, rectangles and the global scope. A spline
    // volume's weight comes from a signed-distance field it cannot evaluate.
    TerrainSurfaceRule row{};
    ResolvedModifier splineVolume{};
    splineVolume.ModType = ResolvedModifier::Type::Volume;
    splineVolume.Shape = Components::TerrainModifierShape::Spline;
    ResolvedEffect fx{};
    fx.EffectKind = ResolvedEffect::Kind::Rules;
    fx.Rules.Rules = &row;
    fx.Rules.RuleCount = 1;
    splineVolume.Effects.push_back(fx);

    std::vector<SurfaceRuleGpu> rules;
    std::vector<SurfaceRuleConditionGpu> conditions;
    EXPECT_FALSE(PackSurfaceRulesForGpuSplat({splineVolume}, rules, conditions));

    // Discriminator: a height-only bake packs nothing and still allows the pass,
    // so the refusals above are about the splat writer, not about volumes.
    ResolvedModifier heightVolume{};
    heightVolume.ModType = ResolvedModifier::Type::Volume;
    ResolvedEffect height{};
    height.EffectKind = ResolvedEffect::Kind::Flatten;
    heightVolume.Effects.push_back(height);
    EXPECT_TRUE(PackSurfaceRulesForGpuSplat({heightVolume}, rules, conditions));
    EXPECT_TRUE(rules.empty());
}

TEST(TerrainSurfaceRulesGpuGate, ARulesOnlyVolumeContributesNoHeight)
{
    ResolvedModifier rulesVolume{};
    rulesVolume.ModType = ResolvedModifier::Type::Volume;
    ResolvedEffect fx{};
    fx.EffectKind = ResolvedEffect::Kind::Rules;
    rulesVolume.Effects.push_back(fx);
    EXPECT_TRUE(rulesVolume.IsSplatOnlyModifier());
}

// ---------------------------------------------------------------------------
// 6 · Splat claims — claimed ground keeps the material its claimant painted
// ---------------------------------------------------------------------------
//
// The height side lets a volume say "this ground is mine" and hold other regions
// off it. The splat side does too now, with ONE requirement the height side does
// not have: the claimant has to PAINT. The height pass has a meaningful "what is
// already there" — the base heightfield plus every earlier modifier — and the
// splat pass does not. It starts from an all-zero splatmap every bake, and an
// all-zero texel resolves to channel 0, the FIRST material, by shipped
// convention. So masking without claimant paint gives a default-material patch
// rather than preservation, and the warning below is what stands between an
// author and that discovery.

namespace
{

// A hard-edged circle volume: falloff 0, so the shape weight is a binary in/out
// mask and every expected texel below is a whole 255 rather than a product of
// two ramps.
ECS::EntityHandle CreateHardCircleVolume(ECS::World& world, float32 radius, float32 priority)
{
    auto e = world.CreateEntity();
    Components::TerrainModifierVolume vol{};
    vol.Shape = Components::TerrainVolumeShape::Circle;
    vol.Radius = radius;
    vol.Falloff = 0.0f;
    vol.Priority = priority;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, vol);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
    return e;
}

Components::TerrainPaintLayerEffect MakePaint(uint32 layer, int32 stackOrder, bool respectClaims)
{
    Components::TerrainPaintLayerEffect fx{};
    fx.LayerIndex = layer;
    fx.Strength = 1.0f;
    fx.Replace = true; // the texel IS the state; replace makes each arm's answer whole
    fx.StackOrder = stackOrder;
    fx.RespectClaims = respectClaims;
    return fx;
}

// The splat grid matches the heightmap grid in this fixture (one config, one
// dim), so world XZ maps to a texel exactly as it does on the height side.
int32 SplatSampleAt(float32 world)
{
    constexpr float32 spacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1);
    return static_cast<int32>(std::lround((world + kWorldSize * 0.5f) / spacing));
}

// The stride assumption above, asserted rather than assumed: a splat grid that
// stopped matching the heightmap grid would silently move every probe below to a
// different texel, and the tests would keep passing on whatever they landed on.
constexpr std::size_t kExpectedSplatBytes =
    static_cast<std::size_t>(kHeightmapDim) * kHeightmapDim * 4;

std::array<uint8, 4> TexelAt(const std::vector<uint8>& splat, float32 worldX, float32 worldZ)
{
    EXPECT_EQ(splat.size(), kExpectedSplatBytes)
        << "the splat grid no longer matches the heightmap grid; every probe below is addressing "
           "the wrong texel";
    const std::size_t i =
        (static_cast<std::size_t>(SplatSampleAt(worldZ)) * kHeightmapDim + SplatSampleAt(worldX))
        * 4;
    if (i + 3 >= splat.size())
        return {{0, 0, 0, 0}};
    return {{splat[i], splat[i + 1], splat[i + 2], splat[i + 3]}};
}

// The claimed road, and the surface that covers it. The claimant paints material
// 2 at priority 0 and claims the same ground; the surface paints material 1 over
// everything at priority 10 and either stops at the claim or does not.
//
// Stack order inside the claimant matters and is the intended shape: the paint
// runs FIRST, so it is not held off by its own claim, and the claim then enters
// the buffer for everything that applies after it.
constexpr float32 kSplatClaimRadius = 20.0f;
constexpr float32 kSplatSurfaceRadius = 70.0f;
constexpr float32 kOnClaim = 0.0f;   // inside the claimant
constexpr float32 kOffClaim = 50.0f; // outside it, still inside the surface

BakeArm BakeClaimedRoad(bool surfaceRespectsClaims, bool claimantPaints)
{
    return Bake([&](ECS::World& world) {
        const ECS::EntityHandle claimant =
            CreateHardCircleVolume(world, kSplatClaimRadius, 0.0f);
        if (claimantPaints)
            world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(
                claimant, MakePaint(/*layer=*/2, /*stackOrder=*/0, /*respectClaims=*/false));
        Components::TerrainGroundClaimEffect claim{};
        claim.Strength = 1.0f;
        claim.StackOrder = 1;
        world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(claimant, claim);

        const ECS::EntityHandle surface =
            CreateHardCircleVolume(world, kSplatSurfaceRadius, 10.0f);
        world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(
            surface, MakePaint(/*layer=*/1, /*stackOrder=*/0, surfaceRespectsClaims));
    });
}

} // namespace

// THE RED ARM. Both arms author the same scene; the only difference is whether
// the surface stops at the claim, and the two produce different materials on the
// claimed ground.
TEST(TerrainSplatClaim, ClaimedGroundKeepsTheMaterialItsClaimantPainted)
{
    const BakeArm respecting = BakeClaimedRoad(/*surfaceRespectsClaims=*/true,
                                               /*claimantPaints=*/true);
    const BakeArm ignoring = BakeClaimedRoad(/*surfaceRespectsClaims=*/false,
                                             /*claimantPaints=*/true);

    const auto onRespecting = TexelAt(respecting.BakedSplat, kOnClaim, kOnClaim);
    const auto onIgnoring = TexelAt(ignoring.BakedSplat, kOnClaim, kOnClaim);

    // On the claim the claimant's material 2 survives; the arm that does not
    // defer overwrites it with the surface's material 1.
    EXPECT_EQ(static_cast<int>(onRespecting[2]), 255)
        << "the claimant's material must survive on ground it owns";
    EXPECT_EQ(static_cast<int>(onRespecting[1]), 0)
        << "the surface must not have painted through the claim";
    ASSERT_NE(onRespecting, onIgnoring)
        << "the two arms must differ on the claimed ground, or this is not a red arm";
    EXPECT_EQ(static_cast<int>(onIgnoring[1]), 255)
        << "with the flag off the surface paints straight through, as it always did";

    // Off the claim both arms are identical: the mask is local to owned ground,
    // not a switch that turns the surface off.
    EXPECT_EQ(TexelAt(respecting.BakedSplat, kOffClaim, kOnClaim),
              TexelAt(ignoring.BakedSplat, kOffClaim, kOnClaim));
    EXPECT_EQ(static_cast<int>(TexelAt(respecting.BakedSplat, kOffClaim, kOnClaim)[1]), 255)
        << "off the claim the surface paints normally";
}

// THE LAYER-0 TRAP, made visible rather than described: the same masking with a
// claimant that paints nothing preserves nothing, it cuts a hole. The texel is
// left all-zero, which the shipped convention resolves to channel 0.
TEST(TerrainSplatClaim, AClaimWithNoPaintLeavesADefaultMaterialPatchAndWarns)
{
    ScopedLogCapture logs;

    const BakeArm noPaint = BakeClaimedRoad(/*surfaceRespectsClaims=*/true,
                                            /*claimantPaints=*/false);

    const auto onClaim = TexelAt(noPaint.BakedSplat, kOnClaim, kOnClaim);
    const std::array<uint8, 4> empty{{0, 0, 0, 0}};
    EXPECT_EQ(onClaim, empty)
        << "this is the trap, not a wish: an unpainted claim leaves an EMPTY texel, which "
           "resolves to channel 0 — if it ever stops doing so, the warning below is lying";
    EXPECT_EQ(static_cast<int>(TexelAt(noPaint.BakedSplat, kOffClaim, kOnClaim)[1]), 255)
        << "and the surface still paints everywhere it is not held off, so the patch is a HOLE "
           "in the surface rather than a bake that did nothing";

    EXPECT_GE(logs.CountContaining("paints nothing"), 1u)
        << "the claim-without-paint trap must be reported, naming the entity that has to change";
    EXPECT_GE(logs.CountContaining("channel 0"), 1u)
        << "the warning must state WHAT goes wrong, not merely that something is odd";
}

// The negative control for the warning: the shape the feature is FOR must be
// silent. A warning that fires on correct authoring is worse than none.
TEST(TerrainSplatClaim, AClaimantThatPaintsDoesNotWarn)
{
    ScopedLogCapture logs;

    const BakeArm painted = BakeClaimedRoad(/*surfaceRespectsClaims=*/true,
                                            /*claimantPaints=*/true);
    ASSERT_FALSE(painted.BakedSplat.empty()) << "precondition: the bake ran";

    EXPECT_EQ(logs.CountContaining("paints nothing"), 0u)
        << "a claimant that paints its material is the intended shape and must not be warned at";
}

// The bit-inertness spine for the splat side: a claim the effect ignores must
// leave the splatmap byte-identical to a bake with no claim at all. The mask
// multiplies by exactly 1.0f, which is not a rounding step.
TEST(TerrainSplatClaim, TheMaskIsBitInertWhereTheEffectDoesNotRespectClaims)
{
    const BakeArm withClaim = BakeClaimedRoad(/*surfaceRespectsClaims=*/false,
                                              /*claimantPaints=*/false);
    const BakeArm withoutClaim = Bake([](ECS::World& world) {
        const ECS::EntityHandle surface =
            CreateHardCircleVolume(world, kSplatSurfaceRadius, 10.0f);
        world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(
            surface, MakePaint(/*layer=*/1, /*stackOrder=*/0, /*respectClaims=*/false));
    });

    EXPECT_TRUE(BytewiseEqual(withClaim.BakedSplat, withoutClaim.BakedSplat))
        << "a claim nothing defers to must not move a single splat byte";
    // Not vacuous: the bake wrote a large region in both arms.
    EXPECT_GT(ChangedTexelCount(withoutClaim.BaseSplat, withoutClaim.BakedSplat), 100u);
}

// Ordering is the same rule the height side uses, and it is worth pinning
// separately because the splat pass walks a DIFFERENT list: it skips modifiers
// that write no splat, so a claim-only volume had to be let back into the walk
// for its ownership to reach the buffer at its own slot at all.
TEST(TerrainSplatClaim, AClaimAboveTheEffectInPriorityHoldsNothingBack)
{
    // NEITHER arm paints from the claimant, so what the texel holds is decided
    // entirely by whether the surface was held off. A claimant that painted would
    // hide the difference behind its own stroke, which is the trap this test
    // would otherwise fall into: the claimant paints last in the claim-above
    // arrangement, so its material would stand there either way.
    const auto bake = [](float32 surfacePriority, float32 claimPriority) {
        return Bake([&](ECS::World& world) {
            const ECS::EntityHandle surface =
                CreateHardCircleVolume(world, kSplatSurfaceRadius, surfacePriority);
            world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(
                surface, MakePaint(/*layer=*/1, /*stackOrder=*/0, /*respectClaims=*/true));

            const ECS::EntityHandle claimant =
                CreateHardCircleVolume(world, kSplatClaimRadius, claimPriority);
            Components::TerrainGroundClaimEffect claim{};
            claim.Strength = 1.0f;
            world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(claimant, claim);
        });
    };

    // Claim ABOVE the surface: the surface paints before the claim exists, so
    // nothing is held back and its material is there.
    const BakeArm claimAbove = bake(/*surfacePriority=*/0.0f, /*claimPriority=*/10.0f);
    EXPECT_EQ(static_cast<int>(TexelAt(claimAbove.BakedSplat, kOnClaim, kOnClaim)[1]), 255)
        << "a claim that applies AFTER the effect holds nothing back, so the surface painted here";

    // Claim BELOW: in place first, so the surface stops and the texel is left
    // empty — the layer-0 patch, reached through ORDERING instead of the flag.
    const BakeArm claimBelow = bake(/*surfacePriority=*/10.0f, /*claimPriority=*/0.0f);
    const std::array<uint8, 4> empty{{0, 0, 0, 0}};
    EXPECT_EQ(TexelAt(claimBelow.BakedSplat, kOnClaim, kOnClaim), empty)
        << "a claim that applies BEFORE the effect holds it off, and with no claimant paint that "
           "leaves the empty texel the layer-0 trap is about";

    // Both arms paint normally off the claim, so neither is a bake that did nothing.
    EXPECT_EQ(static_cast<int>(TexelAt(claimAbove.BakedSplat, kOffClaim, kOnClaim)[1]), 255);
    EXPECT_EQ(static_cast<int>(TexelAt(claimBelow.BakedSplat, kOffClaim, kOnClaim)[1]), 255);
}

// THE RULES ARM of the splat claim, end to end: a claim-respecting surface-rules
// block defers to claimed ground exactly as a paint stroke does. Pinned
// separately because the bake masks the two apply paths at two different sites —
// applyPaint masks the stroke's own weight, applyRules masks the volume weight
// every row is scaled by — and the packer-refusal gate never executes the CPU
// mask at all, so without this test the applyRules mask could be deleted and
// every other claim gate would stay green.
TEST(TerrainSplatClaim, AClaimRespectingRulesBlockDefersLikeAPaintStroke)
{
    const auto bake = [](bool rulesRespectClaims) {
        return Bake([&](ECS::World& world) {
            const ECS::EntityHandle claimant =
                CreateHardCircleVolume(world, kSplatClaimRadius, 0.0f);
            world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(
                claimant, MakePaint(/*layer=*/2, /*stackOrder=*/0, /*respectClaims=*/false));
            Components::TerrainGroundClaimEffect claim{};
            claim.Strength = 1.0f;
            claim.StackOrder = 1;
            world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(claimant, claim);

            const ECS::EntityHandle surface =
                CreateHardCircleVolume(world, kSplatSurfaceRadius, 10.0f);
            Components::TerrainSurfaceRulesEffect rules{};
            rules.RuleCount = 1;
            rules.Rules[0].MaterialSlot = 1;
            rules.Rules[0].Strength = 1.0f;
            rules.Rules[0].Replace = true; // the texel IS the state, as in the paint twin
            rules.RespectClaims = rulesRespectClaims;
            world.AddComponentImmediate<Components::TerrainSurfaceRulesEffect>(surface, rules);
        });
    };

    const BakeArm respecting = bake(true);
    const BakeArm ignoring = bake(false);

    const auto onRespecting = TexelAt(respecting.BakedSplat, kOnClaim, kOnClaim);
    const auto onIgnoring = TexelAt(ignoring.BakedSplat, kOnClaim, kOnClaim);

    EXPECT_EQ(static_cast<int>(onRespecting[2]), 255)
        << "the claimant's material must survive under a deferring rules block";
    EXPECT_EQ(static_cast<int>(onRespecting[1]), 0)
        << "the rules block must not have ruled through the claim";
    ASSERT_NE(onRespecting, onIgnoring)
        << "the two arms must differ on the claimed ground, or this proves nothing";
    EXPECT_EQ(static_cast<int>(onIgnoring[1]), 255)
        << "with the flag off the rules block paints straight through, as it always did";

    // Off the claim both arms rule normally, so the deferring arm is not a block
    // that simply stopped contributing everywhere.
    EXPECT_EQ(TexelAt(respecting.BakedSplat, kOffClaim, kOnClaim),
              TexelAt(ignoring.BakedSplat, kOffClaim, kOnClaim));
    EXPECT_EQ(static_cast<int>(TexelAt(respecting.BakedSplat, kOffClaim, kOnClaim)[1]), 255)
        << "off the claim the rules block paints normally";
}
