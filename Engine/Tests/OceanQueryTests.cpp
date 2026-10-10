#include "AssetCore/AssetTypes.h"

#include "Ocean/OceanDepthCacheAsset.h"
#include "Ocean/OceanFieldRanges.h"
#include "Ocean/OceanCollisionProvider.h"
#include "Ocean/OceanFFTCollisionAsset.h"
#include "Ocean/OceanGPUQuery.h"
#include "Ocean/OceanInputDrawSource.h"
#include "Ocean/OceanPresetAsset.h"
#include "Ocean/OceanQuery.h"
#include "Ocean/OceanRayTrace.h"
#include "Ocean/OceanRenderFeature.h"
#include "Ocean/OceanSeabedDepth.h"
#include "Ocean/OceanSeabedVisibility.h"
#include "Ocean/OceanSimulationDemand.h"
#include "Ocean/OceanSettingsAsset.h"
#include "Ocean/OceanLightShafts.h"
#include "Ocean/OceanRecentMaximum.h"
#include "Ocean/OceanTime.h"
#include "Ocean/OceanUnderwater.h"
#include "Ocean/OceanValidation.h"
#include "Ocean/Systems/OceanExtractionSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Components/Rendering/Ocean.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "Mathematics/Matrix4x4.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

namespace GameEngine::Ocean
{
namespace
{

// OceanSurface.SubSurfaceDepthMax's default: the depth where shallow terms saturate.
constexpr float32 kDepthBandSaturationMeters = 10.0f;

struct SlopedFloor
{
    float32 Base = 0.0f;
    float32 SlopeX = 0.0f;
    float32 SlopeZ = 0.0f;
};

bool SampleSlopedFloor(float32 worldX, float32 worldZ, float32& outFloorY, void* userData)
{
    const auto* floor = static_cast<const SlopedFloor*>(userData);
    if (!floor)
        return false;
    outFloorY = floor->Base + worldX * floor->SlopeX + worldZ * floor->SlopeZ;
    return true;
}

bool SamplePeriodicCollision(uint32, float32 time, float32 worldX, float32 worldZ,
                             OceanFFTDisplacementSample& out, void* userData)
{
    const float32 period = userData ? *static_cast<const float32*>(userData) : 8.0f;
    const float32 phase = 6.28318530718f * (worldX + 0.25f * worldZ) / period +
                          6.28318530718f * time;
    out.X = 0.0f;
    out.Y = std::sin(phase);
    out.Z = 0.0f;
    return true;
}

TEST(OceanQueryTests, AnalyticSampleDefaultsToSeaLevel)
{
    OceanParamsGPU params{};
    params.SeaLevel = 12.5f;

    const OceanSurfaceSample sample = SampleOceanSurfaceAnalytic(params, 4.0f, -2.0f);

    EXPECT_TRUE(sample.Valid);
    EXPECT_EQ(sample.Source, OceanQuerySource::AnalyticGerstner);
    EXPECT_FLOAT_EQ(sample.Height, 12.5f);
    EXPECT_FLOAT_EQ(sample.PositionWS[0], 4.0f);
    EXPECT_FLOAT_EQ(sample.PositionWS[1], 12.5f);
    EXPECT_FLOAT_EQ(sample.PositionWS[2], -2.0f);
    EXPECT_FLOAT_EQ(sample.NormalWS[0], 0.0f);
    EXPECT_FLOAT_EQ(sample.NormalWS[1], 1.0f);
    EXPECT_FLOAT_EQ(sample.NormalWS[2], 0.0f);
    EXPECT_FLOAT_EQ(sample.DisplacementWS[1], 0.0f);
}

TEST(OceanQueryTests, AnalyticSampleReportsWaveHeight)
{
    OceanParamsGPU params{};
    params.SeaLevel = 2.0f;
    params.GerstnerWaveCount = 1;
    params.Waves[0].DirectionX = 1.0f;
    params.Waves[0].DirectionZ = 0.0f;
    params.Waves[0].Amplitude = 3.0f;
    params.Waves[0].Wavelength = 4.0f;
    params.Waves[0].Speed = 1.0f;

    const OceanSurfaceSample sample = SampleOceanSurfaceAnalytic(params, 1.0f, 0.0f);

    EXPECT_TRUE(sample.Valid);
    EXPECT_NEAR(sample.Height, 5.0f, 1e-4f);
    EXPECT_NEAR(sample.DisplacementWS[1], 3.0f, 1e-4f);
}

TEST(OceanQueryTests, AnalyticSampleReportsHorizontalDisplacement)
{
    OceanParamsGPU params{};
    params.SeaLevel = 2.0f;
    params.ChoppyScale = 1.0f;
    params.GerstnerWaveCount = 1;
    params.Waves[0].DirectionX = 1.0f;
    params.Waves[0].DirectionZ = 0.0f;
    params.Waves[0].Amplitude = 3.0f;
    params.Waves[0].Wavelength = 4.0f;
    params.Waves[0].Steepness = 0.5f;
    params.Waves[0].Speed = 1.0f;

    const OceanSurfaceSample sample = SampleOceanSurfaceAnalytic(params, 0.0f, 0.0f);

    EXPECT_TRUE(sample.Valid);
    EXPECT_NEAR(sample.Height, 2.0f, 1e-4f);
    EXPECT_NEAR(sample.DisplacementWS[0], 1.5f, 1e-4f);
    EXPECT_NEAR(sample.DisplacementWS[1], 0.0f, 1e-4f);
    EXPECT_NEAR(sample.DisplacementWS[2], 0.0f, 1e-4f);
    EXPECT_NEAR(sample.PositionWS[0], 1.5f, 1e-4f);
    EXPECT_NEAR(sample.PositionWS[1], 2.0f, 1e-4f);
    EXPECT_NEAR(sample.PositionWS[2], 0.0f, 1e-4f);
}

TEST(OceanQueryTests, AnalyticNormalIsUnitLength)
{
    OceanParamsGPU params{};
    params.GerstnerWaveCount = 1;
    params.Waves[0].DirectionX = 0.6f;
    params.Waves[0].DirectionZ = 0.8f;
    params.Waves[0].Amplitude = 1.25f;
    params.Waves[0].Wavelength = 9.0f;
    params.Waves[0].Speed = 1.0f;

    const OceanSurfaceSample sample = SampleOceanSurfaceAnalytic(params, 2.0f, 3.0f);
    const float32 len = std::sqrt(sample.NormalWS[0] * sample.NormalWS[0] +
                                  sample.NormalWS[1] * sample.NormalWS[1] +
                                  sample.NormalWS[2] * sample.NormalWS[2]);

    EXPECT_TRUE(sample.Valid);
    EXPECT_NEAR(len, 1.0f, 1e-4f);
}

TEST(OceanQueryTests, AnalyticBatchMatchesSingleSamples)
{
    OceanParamsGPU params{};
    params.SeaLevel = -1.0f;
    params.GerstnerWaveCount = 1;
    params.Waves[0].DirectionX = 1.0f;
    params.Waves[0].DirectionZ = 0.0f;
    params.Waves[0].Amplitude = 2.0f;
    params.Waves[0].Wavelength = 8.0f;
    params.Waves[0].Speed = 1.0f;

    OceanSurfaceQueryPoint points[3] = {{0.0f, 0.0f}, {2.0f, 0.0f}, {4.0f, 1.0f}};
    OceanSurfaceSample samples[3]{};
    SampleOceanSurfaceAnalyticBatch(params, points, 3, samples);

    for (uint32 i = 0; i < 3; ++i)
    {
        const OceanSurfaceSample single = SampleOceanSurfaceAnalytic(params, points[i].X, points[i].Z);
        EXPECT_TRUE(samples[i].Valid);
        EXPECT_EQ(samples[i].Source, OceanQuerySource::AnalyticGerstner);
        EXPECT_NEAR(samples[i].Height, single.Height, 1e-5f);
        EXPECT_NEAR(samples[i].NormalWS[0], single.NormalWS[0], 1e-5f);
        EXPECT_NEAR(samples[i].NormalWS[1], single.NormalWS[1], 1e-5f);
        EXPECT_NEAR(samples[i].NormalWS[2], single.NormalWS[2], 1e-5f);
    }
}

TEST(OceanQueryTests, RenderFeatureUsesAnalyticFallbackBeforeFftReadback)
{
    OceanRenderFeature feature;
    OceanParamsGPU params{};
    params.SeaLevel = 2.0f;
    params.GerstnerWaveCount = 1u;
    params.Waves[0].DirectionX = 1.0f;
    params.Waves[0].Amplitude = 3.0f;
    params.Waves[0].Wavelength = 4.0f;
    params.Waves[0].Speed = 1.0f;
    feature.SetParams(params);

    const OceanSurfaceSample sample = feature.SampleSurface(1.0f, 0.0f);
    EXPECT_TRUE(sample.Valid);
    EXPECT_EQ(sample.Source, OceanQuerySource::AnalyticGerstner);
    EXPECT_NEAR(sample.Height, 5.0f, 1e-4f);
}

TEST(OceanQueryTests, DynamicWaveQuiescenceAdvancesOnlyWithSimulationTime)
{
    OceanDynWaves waves;
    OceanWaveImpulseGPU impulse{};
    impulse.CenterRadiusAmp[2] = 1.0f;
    impulse.CenterRadiusAmp[3] = 1.0f;

    waves.SetFrameInputs(&impulse, 1u, 4.0f, 0.2f, 1.0f / 60.0f);
    EXPECT_FALSE(waves.IsQuiescent());

    // Paused/fixed ocean time must preserve the disturbance indefinitely.
    for (uint32 i = 0u; i < 1200u; ++i)
        waves.SetFrameInputs(nullptr, 0u, 4.0f, 0.2f, 0.0f);
    EXPECT_FALSE(waves.IsQuiescent());

    for (uint32 i = 0u; i < 29u; ++i)
        waves.SetFrameInputs(nullptr, 0u, 4.0f, 0.2f, 1.0f);
    EXPECT_FALSE(waves.IsQuiescent());
    waves.SetFrameInputs(nullptr, 0u, 4.0f, 0.2f, 1.0f);
    EXPECT_TRUE(waves.IsQuiescent());
}

TEST(OceanQueryTests, DynamicAndFoamSettingsAccumulateFixedSubsteps)
{
    OceanDynWaves dynamic;
    dynamic.ConfigureSimulation(20.0f, 3u, 0.1f, 0.6f, 9.81f,
                                0.75f, 0.4f, 2.0f, 1u, 3u);
    OceanWaveImpulseGPU impulse{};
    impulse.CenterRadiusAmp[2] = 1.0f;
    impulse.CenterRadiusAmp[3] = 0.5f;
    dynamic.SetFrameInputs(&impulse, 1u, 4.0f, 0.2f, 0.01f);
    EXPECT_EQ(dynamic.GetPendingSubstepCount(), 0u);
    dynamic.SetFrameInputs(nullptr, 0u, 4.0f, 0.2f, 0.04f);
    EXPECT_EQ(dynamic.GetPendingSubstepCount(), 1u);
    dynamic.FinishSubsteps();
    dynamic.SetFrameInputs(nullptr, 0u, 4.0f, 0.2f, 0.5f);
    EXPECT_EQ(dynamic.GetPendingSubstepCount(), 3u);

    OceanFoamSim foam;
    foam.ConfigureSimulation(10.0f, 2u);
    foam.SetFrameInputs({}, {}, 0u, 0.8f, 0.55f, 1.0f, 0.05f,
                        1.0f, 15.0f, 10.0f, 1.0f, 10.0f, 0.0f, 0.0f);
    EXPECT_EQ(foam.GetPendingSubstepCount(), 0u);
    foam.SetFrameInputs({}, {}, 0u, 0.8f, 0.55f, 1.0f, 0.05f,
                        1.0f, 15.0f, 10.0f, 1.0f, 10.0f, 0.0f, 0.0f);
    EXPECT_EQ(foam.GetPendingSubstepCount(), 1u);
    foam.FinishSubsteps();
    foam.SetFrameInputs({}, {}, 0u, 0.8f, 0.55f, 1.0f, 0.35f,
                        1.0f, 15.0f, 10.0f, 1.0f, 10.0f, 0.0f, 0.0f);
    EXPECT_EQ(foam.GetPendingSubstepCount(), 2u);
}

// Frames whose foam step never ran (no water in view) are caught up by the next
// step as one step over the whole gap: it decays the old foam over the gap, and
// its deposits are scaled so each equals the exponential integral over the gap.
// A normal step is unchanged.
TEST(OceanQueryTests, FoamCatchesUpSkippedFramesInOneClosedFormStep)
{
    constexpr float kFadeRate = 0.8f;
    constexpr float kFrame = 0.1f;
    OceanFoamSim foam;
    const auto frameInputs = [&foam]
    {
        foam.SetFrameInputs({}, {}, 0u, kFadeRate, 0.55f, 1.0f, kFrame, 1.0f, 15.0f, 10.0f, 1.0f,
                            10.0f, 0.0f, 0.0f);
    };
    foam.SetShorelineInputs({}, {}, 0.65f, 2.0f, false);
    frameInputs();
    frameInputs();
    frameInputs();

    OceanFoamParamsGPU params{};
    foam.FillParams(params, 0u, 0u);
    const float gap = 3.0f * kFrame;
    EXPECT_FLOAT_EQ(params.DeltaTime, gap);
    const auto integralScale = [gap](float rate) { return (1.0f - std::exp(-rate * gap)) / (rate * gap); };
    EXPECT_NEAR(params.Strength, integralScale(kFadeRate), 1e-6f);
    EXPECT_NEAR(params.ShorelineStrength, 2.0f * integralScale(params.ShorelineFadeRateScale * kFadeRate), 1e-6f);
    foam.FinishSubsteps();

    frameInputs();
    foam.FillParams(params, 0u, 0u);
    EXPECT_FLOAT_EQ(params.DeltaTime, kFrame);
    EXPECT_FLOAT_EQ(params.Strength, 1.0f);
    EXPECT_FLOAT_EQ(params.ShorelineStrength, 2.0f);
}

// The declare runs once per presented window: one window dispatching the waves
// keeps their CPU copy current while a second window, with no water in view,
// declares between its frames. The copy goes stale only once no window has
// dispatched for the hold.
TEST(OceanQueryTests, WaveDataStaysCurrentWhileAnyWindowDispatches)
{
    using Clock = OceanSimulationDemand::Clock;
    OceanSimulationDemand demand;
    Clock::time_point now{};
    const auto halfFrame = std::chrono::microseconds(8333);
    for (int frame = 0; frame < 240; ++frame)
    {
        // Window A: the bookkeeping runs before its views claim the waves.
        now += halfFrame;
        demand.Update(now);
        if (frame > 0)
            EXPECT_FALSE(demand.IsWaveDataStale(now)) << "window A, frame " << frame;
        demand.NoteWaveDispatch(now);
        // Window B: no water in view, no dispatch.
        now += halfFrame;
        demand.Update(now);
        EXPECT_FALSE(demand.IsWaveDataStale(now)) << "window B, frame " << frame;
    }
    const Clock::time_point lastDispatch = now - halfFrame;
    EXPECT_FALSE(demand.IsWaveDataStale(lastDispatch + OceanSimulationDemand::kHold - std::chrono::milliseconds(1)));
    EXPECT_TRUE(demand.IsWaveDataStale(lastDispatch + OceanSimulationDemand::kHold));
}

// A gameplay query holds the wave simulation for a span of time, whatever the
// frame rate: a once-per-0.4 s query at 1000 frames per second never lapses.
TEST(OceanQueryTests, SurfaceQueryHoldIsTimeBased)
{
    using Clock = OceanSimulationDemand::Clock;
    OceanSimulationDemand demand;
    Clock::time_point now{};
    EXPECT_FALSE(demand.HasRecentSurfaceQueries(now));
    for (int frame = 0; frame < 2000; ++frame)
    {
        if (frame % 400 == 0)
            demand.NoteSurfaceQuery();
        now += std::chrono::milliseconds(1);
        demand.Update(now);
        EXPECT_TRUE(demand.HasRecentSurfaceQueries(now)) << "frame " << frame;
    }
    now += OceanSimulationDemand::kHold;
    EXPECT_FALSE(demand.HasRecentSurfaceQueries(now));
}

TEST(OceanQueryTests, FlowSimSamplesRectangularCurrent)
{
    OceanFlowSim flow;
    OceanFlowSourceGPU source{};
    source.OriginExtent[0] = 0.0f;
    source.OriginExtent[1] = 0.0f;
    source.OriginExtent[2] = 10.0f;
    source.OriginExtent[3] = 10.0f;
    source.FlowVelocity[0] = 2.0f;
    source.FlowVelocity[1] = -1.0f;
    flow.SetSources(&source, 1u);

    const OceanCurrentSample center = flow.SampleFlow(0.0f, 0.0f);
    EXPECT_TRUE(center.Valid);
    EXPECT_NEAR(center.FlowX, 2.0f, 1e-5f);
    EXPECT_NEAR(center.FlowZ, -1.0f, 1e-5f);
    EXPECT_EQ(center.RectSourceCount, 1u);

    const OceanCurrentSample edge = flow.SampleFlow(9.5f, 0.0f);
    EXPECT_GT(edge.FlowX, 0.0f);
    EXPECT_LT(edge.FlowX, center.FlowX);

    const OceanCurrentSample outside = flow.SampleFlow(11.0f, 0.0f);
    EXPECT_TRUE(outside.Valid);
    EXPECT_FLOAT_EQ(outside.FlowX, 0.0f);
    EXPECT_FLOAT_EQ(outside.FlowZ, 0.0f);
}

TEST(OceanQueryTests, FlowSimSamplesPolygonCurrent)
{
    OceanFlowSim flow;
    OceanFlowPolygonGPU poly{};
    poly.Meta[0] = 4.0f;
    poly.Bounds[0] = -2.0f;
    poly.Bounds[1] = -2.0f;
    poly.Bounds[2] = 2.0f;
    poly.Bounds[3] = 2.0f;
    poly.FlowVelocity[0] = -0.5f;
    poly.FlowVelocity[1] = 3.0f;
    poly.Points[0][0] = -2.0f;
    poly.Points[0][1] = -2.0f;
    poly.Points[1][0] = 2.0f;
    poly.Points[1][1] = -2.0f;
    poly.Points[2][0] = 2.0f;
    poly.Points[2][1] = 2.0f;
    poly.Points[3][0] = -2.0f;
    poly.Points[3][1] = 2.0f;
    flow.SetPolygonSources(&poly, 1u);

    const OceanCurrentSample inside = flow.SampleFlow(0.0f, 0.0f);
    EXPECT_TRUE(inside.Valid);
    EXPECT_NEAR(inside.FlowX, -0.5f, 1e-5f);
    EXPECT_NEAR(inside.FlowZ, 3.0f, 1e-5f);
    EXPECT_EQ(inside.PolygonSourceCount, 1u);

    const OceanCurrentSample outside = flow.SampleFlow(3.0f, 0.0f);
    EXPECT_FLOAT_EQ(outside.FlowX, 0.0f);
    EXPECT_FLOAT_EQ(outside.FlowZ, 0.0f);
}

TEST(OceanQueryTests, FlowSimReportsTextureFlowMapFootprints)
{
    OceanFlowSim flow;
    OceanFlowMapSourceGPU map{};
    map.OriginExtent[0] = 0.0f;
    map.OriginExtent[1] = 0.0f;
    map.OriginExtent[2] = 5.0f;
    map.OriginExtent[3] = 5.0f;
    map.StrengthFeather[0] = 4.0f;
    map.StrengthFeather[2] = 0.25f;
    map.StrengthFeather[3] = -0.75f;
    ::GameEngine::Rendering::TextureHandle texture{};
    OceanCpuTextureRG cpuTexture{};
    cpuTexture.Width = 1u;
    cpuTexture.Height = 1u;
    cpuTexture.RG = {0.75f, 0.25f};
    flow.SetFlowMapSources(&map, &texture, 1u, &cpuTexture);

    const OceanCurrentSample inside = flow.SampleFlow(0.0f, 0.0f);
    EXPECT_TRUE(inside.Valid);
    EXPECT_TRUE(inside.TextureFlowMapContributes);
    EXPECT_EQ(inside.TextureFlowMapCount, 1u);
    EXPECT_NEAR(inside.FlowX, 2.25f, 1e-5f);
    EXPECT_NEAR(inside.FlowZ, -2.75f, 1e-5f);

    const OceanCurrentSample outside = flow.SampleFlow(6.0f, 0.0f);
    EXPECT_FALSE(outside.TextureFlowMapContributes);
    EXPECT_FLOAT_EQ(outside.FlowX, 0.0f);
    EXPECT_FLOAT_EQ(outside.FlowZ, 0.0f);
}

TEST(OceanQueryTests, WaterBodyPolygonConstrainsSurfaceQueries)
{
    OceanRenderFeature feature;

    OceanRenderFeature::WaterBodyPolygon polygon{};
    polygon.PointCount = 3u;
    polygon.X[0] = 0.0f;
    polygon.Z[0] = 0.0f;
    polygon.X[1] = 10.0f;
    polygon.Z[1] = 0.0f;
    polygon.X[2] = 0.0f;
    polygon.Z[2] = 10.0f;

    feature.SetWaterBodies({}, true);
    feature.SetWaterBodyPolygons({polygon});

    EXPECT_TRUE(feature.IsSurfaceQueryAllowed(1.0f, 1.0f));
    EXPECT_FALSE(feature.IsSurfaceQueryAllowed(8.0f, 8.0f));
    EXPECT_TRUE(feature.SampleSurface(1.0f, 1.0f).Valid);
    EXPECT_FALSE(feature.SampleSurface(8.0f, 8.0f).Valid);
}

TEST(OceanQueryTests, PolygonFootprintsClassifyPointsBesideANearlyHorizontalEdgeExactly)
{
    // The bottom edge rises 5e-7 over 10 units, so at x = 6 it sits at z = 3e-7: the point
    // (6, 2.5e-7) is just below it and outside, while (4, 2.5e-7) is just above it and inside.
    const float32 xs[4] = {0.0f, 10.0f, 10.0f, 0.0f};
    const float32 zs[4] = {0.0f, 5e-7f, 1.0f, 1.0f};

    OceanRenderFeature feature;
    OceanRenderFeature::WaterBodyPolygon body{};
    OceanRenderFeature::UnderwaterVolumePolygon volume{};
    volume.SurfaceY = 2.0f;
    volume.Depth = 5.0f;
    OceanRenderFeature::WaterBodyWavePolygon wave{};
    wave.Weight = 0.25f;
    OceanFlowPolygonGPU flowPolygon{};
    flowPolygon.Meta[0] = 4.0f;
    flowPolygon.Bounds[2] = 10.0f;
    flowPolygon.Bounds[3] = 1.0f;
    flowPolygon.FlowVelocity[0] = 1.0f;
    body.PointCount = volume.PointCount = wave.PointCount = 4u;
    for (uint32 i = 0; i < 4u; ++i)
    {
        body.X[i] = volume.X[i] = wave.X[i] = flowPolygon.Points[i][0] = xs[i];
        body.Z[i] = volume.Z[i] = wave.Z[i] = flowPolygon.Points[i][1] = zs[i];
    }
    feature.SetWaterBodies({}, true);
    feature.SetWaterBodyPolygons({body});
    feature.SetUnderwaterVolumePolygons({volume});
    feature.SetWaterBodyWaveOverrides({}, {wave});
    OceanFlowSim flow;
    flow.SetPolygonSources(&flowPolygon, 1u);

    float32 depth = 0.0f;
    EXPECT_TRUE(feature.IsSurfaceQueryAllowed(4.0f, 2.5e-7f));
    EXPECT_FALSE(feature.IsSurfaceQueryAllowed(6.0f, 2.5e-7f));
    EXPECT_TRUE(feature.TestUnderwaterVolume(4.0f, 0.0f, 2.5e-7f, depth));
    EXPECT_FALSE(feature.TestUnderwaterVolume(6.0f, 0.0f, 2.5e-7f, depth));
    EXPECT_NEAR(feature.SampleWaterBodyWaveOverride(4.0f, 2.5e-7f, 0.0f).Weight, 0.25f, 1e-5f);
    EXPECT_NEAR(feature.SampleWaterBodyWaveOverride(6.0f, 2.5e-7f, 0.0f).Weight, 1.0f, 1e-5f);
    EXPECT_EQ(flow.SampleFlow(4.0f, 2.5e-7f).PolygonSourceCount, 1u);
    EXPECT_EQ(flow.SampleFlow(6.0f, 2.5e-7f).PolygonSourceCount, 0u);
}

TEST(OceanQueryTests, UnderwaterPolygonVolumeMatchesFootprint)
{
    OceanRenderFeature feature;

    OceanRenderFeature::UnderwaterVolumePolygon polygon{};
    polygon.SurfaceY = 2.0f;
    polygon.Depth = 5.0f;
    polygon.PointCount = 3u;
    polygon.X[0] = 0.0f;
    polygon.Z[0] = 0.0f;
    polygon.X[1] = 10.0f;
    polygon.Z[1] = 0.0f;
    polygon.X[2] = 0.0f;
    polygon.Z[2] = 10.0f;

    feature.SetUnderwaterVolumePolygons({polygon});

    float32 depth = 0.0f;
    EXPECT_TRUE(feature.HasUnderwaterVolumes());
    EXPECT_TRUE(feature.TestUnderwaterVolume(1.0f, 0.0f, 1.0f, depth));
    EXPECT_NEAR(depth, 2.0f, 1e-5f);
    EXPECT_FALSE(feature.TestUnderwaterVolume(8.0f, 0.0f, 8.0f, depth));
    EXPECT_FALSE(feature.TestUnderwaterVolume(1.0f, 3.0f, 1.0f, depth));
    EXPECT_FALSE(feature.TestUnderwaterVolume(1.0f, -4.0f, 1.0f, depth));
}

TEST(OceanQueryTests, UnderwaterPortalDataKeepsPolygonVolumesExact)
{
    OceanRenderFeature feature;
    OceanParamsGPU params{};
    params.Underwater = 1u;
    feature.SetParams(params);

    OceanRenderFeature::UnderwaterVolumePolygon polygon{};
    polygon.SurfaceY = 4.0f;
    polygon.Depth = 6.0f;
    polygon.PointCount = 3u;
    polygon.X[0] = 0.0f;
    polygon.Z[0] = 0.0f;
    polygon.X[1] = 8.0f;
    polygon.Z[1] = 0.0f;
    polygon.X[2] = 0.0f;
    polygon.Z[2] = 8.0f;

    feature.SetUnderwaterVolumePolygons({polygon});

    OceanUnderwaterPortalData data;
    feature.FillUnderwaterPortalData(data, kMaxOceanUnderwaterPortalVolumes,
                                     kMaxOceanUnderwaterPortalExclusions, kMaxOceanUnderwaterPortalPolygons);

    EXPECT_TRUE(data.Enabled);
    EXPECT_TRUE(data.Volumes.empty());
    ASSERT_EQ(data.Polygons.size(), 1u);
    EXPECT_FLOAT_EQ(data.Polygons[0].SurfaceY, 4.0f);
    EXPECT_FLOAT_EQ(data.Polygons[0].Depth, 6.0f);
    EXPECT_EQ(data.Polygons[0].PointCount, 3u);
    EXPECT_FLOAT_EQ(data.Polygons[0].X[1], 8.0f);
    EXPECT_FLOAT_EQ(data.Polygons[0].Z[2], 8.0f);
}

TEST(OceanQueryTests, UnderwaterPortalDataCarriesOccludersWithoutExcludingCamera)
{
    OceanRenderFeature feature;
    OceanParamsGPU params{};
    params.Underwater = 1u;
    feature.SetParams(params);

    OceanRenderFeature::UnderwaterVolumeBox volume{};
    volume.CenterX = 0.0f;
    volume.CenterY = 0.0f;
    volume.CenterZ = 0.0f;
    volume.HalfX = 10.0f;
    volume.HalfY = 10.0f;
    volume.HalfZ = 10.0f;

    OceanRenderFeature::UnderwaterVolumeBox occluder{};
    occluder.CenterX = 0.0f;
    occluder.CenterY = 0.0f;
    occluder.CenterZ = -4.0f;
    occluder.HalfX = 4.0f;
    occluder.HalfY = 4.0f;
    occluder.HalfZ = 0.25f;

    feature.SetUnderwaterVolumes({volume});
    feature.SetUnderwaterPortalOccluders({occluder});

    float32 depth = 0.0f;
    EXPECT_FALSE(feature.IsUnderwaterExcluded(0.0f, 0.0f, -4.0f));
    EXPECT_TRUE(feature.TestUnderwaterVolume(0.0f, 0.0f, 0.0f, depth));

    OceanUnderwaterPortalData data;
    feature.FillUnderwaterPortalData(data, kMaxOceanUnderwaterPortalVolumes,
                                     kMaxOceanUnderwaterPortalExclusions, kMaxOceanUnderwaterPortalPolygons,
                                     kMaxOceanUnderwaterPortalOccluders);

    EXPECT_TRUE(data.Enabled);
    ASSERT_EQ(data.Occluders.size(), 1u);
    EXPECT_FLOAT_EQ(data.Occluders[0].CenterZ, -4.0f);
    EXPECT_FLOAT_EQ(data.Occluders[0].HalfZ, 0.25f);
}

// A camera at the origin looking along +Z (left-handed, reverse-Z), 60 degrees
// vertical, 16:9.
std::array<float, 16> ForwardViewProj()
{
    const Mathematics::Matrix4x4 view = Mathematics::Matrix4x4::LookAt(
        Mathematics::Vector3(0.0f, 0.0f, 0.0f), Mathematics::Vector3(0.0f, 0.0f, 1.0f),
        Mathematics::Vector3(0.0f, 1.0f, 0.0f));
    const Mathematics::Matrix4x4 projection =
        Mathematics::Matrix4x4::PerspectiveReverseZ(1.0471976f, 16.0f / 9.0f, 0.5f, 600.0f);
    const Mathematics::Matrix4x4 viewProj = projection * view;
    std::array<float, 16> out{};
    std::memcpy(out.data(), viewProj.Data(), sizeof(float) * 16);
    return out;
}

OceanUnderwaterPortalBox PortalBoxAt(float x, float y, float z)
{
    OceanUnderwaterPortalBox box{};
    box.CenterX = x;
    box.CenterY = y;
    box.CenterZ = z;
    box.HalfX = 5.0f;
    box.HalfY = 5.0f;
    box.HalfZ = 5.0f;
    return box;
}

// A dry camera declares the underwater composite only when a portal volume can
// be on screen: a volume in front is, one behind the camera or beside the view
// cone is not, and one straddling a frustum plane with its centre outside is.
TEST(OceanQueryTests, UnderwaterPortalInViewFollowsTheVolumeBounds)
{
    const std::array<float, 16> viewProj = ForwardViewProj();
    OceanUnderwaterPortalData data;
    EXPECT_FALSE(IsUnderwaterPortalInView(data, viewProj.data()));

    data.Volumes = {PortalBoxAt(0.0f, 0.0f, 50.0f)};
    EXPECT_TRUE(IsUnderwaterPortalInView(data, viewProj.data()));

    data.Volumes = {PortalBoxAt(0.0f, 0.0f, -50.0f), PortalBoxAt(200.0f, 0.0f, 20.0f)};
    EXPECT_FALSE(IsUnderwaterPortalInView(data, viewProj.data()));

    // The right plane crosses x = 51.3 at z = 50 (horizontal half-angle 45.7
    // degrees): the centre at x = 55 is outside, the face at x = 50 inside.
    data.Volumes = {PortalBoxAt(55.0f, 0.0f, 50.0f)};
    EXPECT_TRUE(IsUnderwaterPortalInView(data, viewProj.data()));
    data.Volumes.clear();

    OceanUnderwaterPortalPolygon polygon{};
    polygon.SurfaceY = 0.0f;
    polygon.Depth = 4.0f;
    polygon.PointCount = 3u;
    polygon.X[0] = -5.0f;
    polygon.Z[0] = 40.0f;
    polygon.X[1] = 5.0f;
    polygon.Z[1] = 40.0f;
    polygon.X[2] = 0.0f;
    polygon.Z[2] = 50.0f;
    data.Polygons = {polygon};
    EXPECT_TRUE(IsUnderwaterPortalInView(data, viewProj.data()));

    for (uint32_t i = 0u; i < polygon.PointCount; ++i)
        polygon.Z[i] -= 90.0f;
    data.Polygons = {polygon};
    EXPECT_FALSE(IsUnderwaterPortalInView(data, viewProj.data()));
    data.Polygons.clear();

    // Spline ribbons: the tree's root branch record carries their bounds.
    OceanRibbonTriangleGPU root{};
    root.Meta[0] = 0x80000000u;
    root.A[0] = -5.0f;
    root.A[1] = -10.0f;
    root.A[2] = -60.0f;
    root.B[0] = 5.0f;
    root.B[1] = 0.0f;
    root.B[2] = -40.0f;
    data.Ribbons = {root};
    EXPECT_FALSE(IsUnderwaterPortalInView(data, viewProj.data()));
    root.A[2] = 40.0f;
    root.B[2] = 60.0f;
    data.Ribbons = {root};
    EXPECT_TRUE(IsUnderwaterPortalInView(data, viewProj.data()));
}

// Reflected caustics are declared only when the band around a water body can be
// on screen: a footprint in front is, one behind the camera is not unless the
// margin reaches into the view, and an unconfined ocean always is.
TEST(OceanQueryTests, WaterFootprintInViewFollowsTheWaterBodies)
{
    const std::array<float, 16> viewProj = ForwardViewProj();
    OceanRenderFeature feature;

    OceanRenderFeature::WaterBodyBox ahead{};
    ahead.CenterZ = 50.0f;
    ahead.HalfX = 5.0f;
    ahead.HalfZ = 5.0f;
    feature.SetWaterBodies({ahead}, true);
    EXPECT_TRUE(feature.IsWaterFootprintInView(viewProj.data(), -1.0f, 3.0f, 0.0f));

    OceanRenderFeature::WaterBodyBox behind = ahead;
    behind.CenterZ = -50.0f;
    feature.SetWaterBodies({behind}, true);
    EXPECT_FALSE(feature.IsWaterFootprintInView(viewProj.data(), -1.0f, 3.0f, 0.0f));
    EXPECT_TRUE(feature.IsWaterFootprintInView(viewProj.data(), -1.0f, 3.0f, 60.0f));

    // A body whose footprint lies in view but whose band is far below it.
    feature.SetWaterBodies({ahead}, true);
    EXPECT_FALSE(feature.IsWaterFootprintInView(viewProj.data(), -200.0f, -150.0f, 0.0f));

    OceanRenderFeature::WaterBodyPolygon polygon{};
    polygon.PointCount = 3u;
    polygon.X[0] = -5.0f;
    polygon.Z[0] = -40.0f;
    polygon.X[1] = 5.0f;
    polygon.Z[1] = -40.0f;
    polygon.X[2] = 0.0f;
    polygon.Z[2] = -50.0f;
    feature.SetWaterBodies({}, true);
    feature.SetWaterBodyPolygons({polygon});
    EXPECT_FALSE(feature.IsWaterFootprintInView(viewProj.data(), -1.0f, 3.0f, 0.0f));
    EXPECT_TRUE(feature.IsWaterFootprintInView(viewProj.data(), -1.0f, 3.0f, 60.0f));
    for (uint32_t i = 0u; i < polygon.PointCount; ++i)
        polygon.Z[i] += 90.0f;
    feature.SetWaterBodyPolygons({polygon});
    EXPECT_TRUE(feature.IsWaterFootprintInView(viewProj.data(), -1.0f, 3.0f, 0.0f));

    feature.SetWaterBodyPolygons({});

    OceanRenderFeature::WaterBodyStamp stamp{};
    stamp.CenterZ = -50.0f;
    stamp.HalfWidth = 5.0f;
    feature.SetWaterBodyStamps({stamp});
    EXPECT_FALSE(feature.IsWaterFootprintInView(viewProj.data(), -1.0f, 3.0f, 0.0f));
    stamp.CenterZ = 50.0f;
    feature.SetWaterBodyStamps({stamp});
    EXPECT_TRUE(feature.IsWaterFootprintInView(viewProj.data(), -1.0f, 3.0f, 0.0f));
    feature.SetWaterBodyStamps({});

    // A surface-carrying spline ribbon counts by its XZ bounds (A/B/C: x, z).
    OceanRibbonTriangleGPU ribbon{};
    ribbon.Meta[0] = 16u;
    ribbon.A[0] = -5.0f;
    ribbon.A[1] = -45.0f;
    ribbon.B[0] = 5.0f;
    ribbon.B[1] = -45.0f;
    ribbon.C[0] = 0.0f;
    ribbon.C[1] = -55.0f;
    feature.GetSplineRaster().SetTriangles({ribbon});
    EXPECT_FALSE(feature.IsWaterFootprintInView(viewProj.data(), -1.0f, 3.0f, 0.0f));
    ribbon.A[1] = ribbon.B[1] = 45.0f;
    ribbon.C[1] = 55.0f;
    feature.GetSplineRaster().SetTriangles({ribbon});
    EXPECT_TRUE(feature.IsWaterFootprintInView(viewProj.data(), -1.0f, 3.0f, 0.0f));
    // A floating-origin shift of +100 m on Z moves the ribbon to z = -55 .. -45,
    // behind the camera: the gate follows the shifted bounds.
    feature.GetSplineRaster().RebaseOrigin(0.0f, 100.0f);
    EXPECT_FALSE(feature.IsWaterFootprintInView(viewProj.data(), -1.0f, 3.0f, 0.0f));
    feature.GetSplineRaster().SetTriangles({});

    feature.SetWaterBodies({behind}, false);
    EXPECT_TRUE(feature.IsWaterFootprintInView(viewProj.data(), -1.0f, 3.0f, 0.0f));
    // An unbounded ocean is a band across the whole world: a band above the
    // frustum's reach is out of view.
    EXPECT_FALSE(feature.IsWaterFootprintInView(viewProj.data(), 400.0f, 500.0f, 0.0f));
}

// The reflected-caustics band is measured from sea level, widened by how far the
// shader's surface can move: twice the measured FFT maximum (floored, capped at
// the clamp), the clamp when nothing was measured, or the summed Gerstner
// amplitudes.
TEST(OceanQueryTests, ReflectedCausticsDisplacementBoundCoversTheSurface)
{
    OceanParamsGPU params{};
    params.WaveMode = 1u;
    params.FFTCascadeCount = 3u;
    params.MaxVerticalDisplacement = 10.0f;
    EXPECT_FLOAT_EQ(ReflectedCausticsSurfaceDisplacementBound(params, nullptr), 10.0f);
    const float calm = 0.4f;
    EXPECT_FLOAT_EQ(ReflectedCausticsSurfaceDisplacementBound(params, &calm), 0.8f);
    const float glassy = 0.01f;
    EXPECT_FLOAT_EQ(ReflectedCausticsSurfaceDisplacementBound(params, &glassy),
                    kReflectedCausticsMeasuredBoundFloor);
    const float rough = 7.0f;
    EXPECT_FLOAT_EQ(ReflectedCausticsSurfaceDisplacementBound(params, &rough), 10.0f);

    params.FFTCascadeCount = 0u;
    params.GerstnerWaveCount = 2u;
    params.Waves[0].Amplitude = 0.75f;
    params.Waves[1].Amplitude = -0.5f;
    EXPECT_FLOAT_EQ(ReflectedCausticsSurfaceDisplacementBound(params, &calm), 1.25f);
}

// Reflected caustics are off until a scene raises the strength: a zero strength
// declares no pass, on the component and on the feature's settings before the
// first extraction.
TEST(OceanQueryTests, ReflectedCausticsAreOffByDefault)
{
    EXPECT_EQ(Components::OceanSurface{}.ReflectedCausticsStrength, 0.0f);
    EXPECT_EQ(OceanUnderwaterSettings{}.ReflectedCaustics_Strength, 0.0f);
}

// The measured bound holds the largest sample of at least the last second, and
// forgets samples once two seconds have passed without one.
TEST(OceanQueryTests, RecentMaximumCoversAtLeastTheLastWindow)
{
    using namespace std::chrono_literals;
    const OceanRecentMaximum::Clock::time_point t0{};
    OceanRecentMaximum recent;
    float value = 0.0f;
    EXPECT_FALSE(recent.Get(t0, value));

    recent.Add(3.0f, t0);
    recent.Add(1.0f, t0 + 500ms);
    ASSERT_TRUE(recent.Get(t0 + 900ms, value));
    EXPECT_FLOAT_EQ(value, 3.0f);

    recent.Add(1.0f, t0 + 1200ms);
    ASSERT_TRUE(recent.Get(t0 + 1900ms, value));
    EXPECT_FLOAT_EQ(value, 3.0f);

    recent.Add(0.5f, t0 + 2500ms);
    ASSERT_TRUE(recent.Get(t0 + 2600ms, value));
    EXPECT_FLOAT_EQ(value, 1.0f);

    EXPECT_FALSE(recent.Get(t0 + 6000ms, value));
}

TEST(OceanQueryTests, UnderwaterExclusionOverridesVolume)
{
    OceanRenderFeature feature;

    OceanRenderFeature::UnderwaterVolumeBox volume{};
    volume.CenterX = 0.0f;
    volume.CenterY = 0.0f;
    volume.CenterZ = 0.0f;
    volume.HalfX = 10.0f;
    volume.HalfY = 10.0f;
    volume.HalfZ = 10.0f;

    OceanRenderFeature::UnderwaterVolumeBox exclusion{};
    exclusion.CenterX = 0.0f;
    exclusion.CenterY = 0.0f;
    exclusion.CenterZ = 0.0f;
    exclusion.HalfX = 2.0f;
    exclusion.HalfY = 2.0f;
    exclusion.HalfZ = 2.0f;

    feature.SetUnderwaterVolumes({volume});
    feature.SetUnderwaterExclusionVolumes({exclusion});

    float32 depth = 0.0f;
    EXPECT_TRUE(feature.IsUnderwaterExcluded(0.0f, 0.0f, 0.0f));
    EXPECT_FALSE(feature.TestUnderwaterVolume(0.0f, 0.0f, 0.0f, depth));
    EXPECT_TRUE(feature.TestUnderwaterVolume(5.0f, 0.0f, 0.0f, depth));
    EXPECT_NEAR(depth, 10.0f, 1e-5f);
}

TEST(OceanQueryTests, WaterBodyWaveOverrideScalesQueryHeight)
{
    OceanRenderFeature feature;

    OceanParamsGPU params{};
    params.SeaLevel = 2.0f;
    params.GerstnerWaveCount = 1u;
    params.Waves[0].DirectionX = 1.0f;
    params.Waves[0].Amplitude = 4.0f;
    params.Waves[0].Wavelength = 4.0f;
    params.Waves[0].Speed = 1.0f;
    feature.SetParams(params);

    OceanRenderFeature::WaterBodyWaveBox box{};
    box.CenterX = 1.0f;
    box.CenterZ = 0.0f;
    box.HalfX = 2.0f;
    box.HalfZ = 2.0f;
    box.Weight = 0.25f;
    box.Chop = 0.5f;
    feature.SetWaterBodyWaveOverrides({box}, {});

    const OceanSurfaceSample inside = feature.SampleSurface(1.0f, 0.0f);
    const OceanSurfaceSample outside = feature.SampleSurface(5.0f, 0.0f);

    EXPECT_TRUE(inside.Valid);
    EXPECT_NEAR(inside.Height, 3.0f, 1e-4f);
    EXPECT_NEAR(inside.DisplacementWS[0], 0.0f, 1e-4f);
    EXPECT_NEAR(inside.WaveWeight, 0.25f, 1e-5f);
    EXPECT_NEAR(inside.WaveChop, 0.5f, 1e-5f);
    EXPECT_NEAR(inside.LocalWaveHeight, 0.0f, 1e-5f);
    EXPECT_TRUE(outside.Valid);
    EXPECT_NEAR(outside.Height, 6.0f, 1e-4f);
    EXPECT_NEAR(outside.WaveWeight, 1.0f, 1e-5f);
    EXPECT_NEAR(outside.WaveChop, 1.0f, 1e-5f);
    EXPECT_NEAR(outside.LocalWaveHeight, 0.0f, 1e-5f);
}

TEST(OceanQueryTests, TextureWaveMaskFootprintIsReportedBySurfaceQueries)
{
    OceanRenderFeature feature;

    OceanParamsGPU params{};
    params.SeaLevel = 0.0f;
    feature.SetParams(params);

    OceanRenderFeature::WaterBodyWaveTextureBox box{};
    box.CenterX = 0.0f;
    box.CenterZ = 0.0f;
    box.HalfX = 5.0f;
    box.HalfZ = 4.0f;
    box.WeightScale = 2.0f;
    box.ChopScale = 0.5f;
    box.WeightBias = 0.1f;
    box.ChopBias = 0.2f;
    box.Feather = 1.0f;
    box.Texture.Width = 1u;
    box.Texture.Height = 1u;
    box.Texture.RG = {0.25f, 0.5f};
    feature.SetWaterBodyWaveOverrides({}, {}, {box});

    const OceanSurfaceSample inside = feature.SampleSurface(0.0f, 0.0f);
    const OceanSurfaceSample outside = feature.SampleSurface(8.0f, 0.0f);

    EXPECT_TRUE(inside.Valid);
    EXPECT_TRUE(inside.TextureWaveMaskContributes);
    EXPECT_EQ(inside.TextureWaveMaskCount, 1u);
    EXPECT_NEAR(inside.WaveWeight, 0.6f, 1e-5f);
    EXPECT_NEAR(inside.WaveChop, 0.45f, 1e-5f);
    EXPECT_TRUE(outside.Valid);
    EXPECT_FALSE(outside.TextureWaveMaskContributes);
    EXPECT_EQ(outside.TextureWaveMaskCount, 0u);
    EXPECT_NEAR(outside.WaveWeight, 1.0f, 1e-5f);
    EXPECT_NEAR(outside.WaveChop, 1.0f, 1e-5f);

    const OceanSurfaceQueryPoint points[2] = {{0.0f, 0.0f}, {8.0f, 0.0f}};
    OceanSurfaceSample samples[2]{};
    feature.SampleSurfaces(points, 2u, samples);
    EXPECT_TRUE(samples[0].TextureWaveMaskContributes);
    EXPECT_NEAR(samples[0].WaveWeight, 0.6f, 1e-5f);
    EXPECT_NEAR(samples[0].WaveChop, 0.45f, 1e-5f);
    EXPECT_FALSE(samples[1].TextureWaveMaskContributes);
}

TEST(OceanQueryTests, WaterBodyWaveOverrideScalesQueryHorizontalDisplacement)
{
    OceanRenderFeature feature;

    OceanParamsGPU params{};
    params.SeaLevel = 2.0f;
    params.ChoppyScale = 1.0f;
    params.GerstnerWaveCount = 1u;
    params.Waves[0].DirectionX = 1.0f;
    params.Waves[0].DirectionZ = 0.0f;
    params.Waves[0].Amplitude = 3.0f;
    params.Waves[0].Wavelength = 4.0f;
    params.Waves[0].Steepness = 0.5f;
    params.Waves[0].Speed = 1.0f;
    feature.SetParams(params);

    OceanRenderFeature::WaterBodyWaveBox box{};
    box.CenterX = 0.0f;
    box.CenterZ = 0.0f;
    box.HalfX = 2.0f;
    box.HalfZ = 2.0f;
    box.Weight = 0.5f;
    box.Chop = 0.25f;
    feature.SetWaterBodyWaveOverrides({box}, {});

    const OceanSurfaceSample inside = feature.SampleSurface(0.0f, 0.0f);
    const OceanSurfaceSample outside = feature.SampleSurface(5.0f, 0.0f);

    EXPECT_TRUE(inside.Valid);
    EXPECT_NEAR(inside.Height, 2.0f, 1e-4f);
    EXPECT_NEAR(inside.DisplacementWS[0], 0.1875f, 1e-4f);
    EXPECT_NEAR(inside.PositionWS[0], 0.1875f, 1e-4f);
    EXPECT_NEAR(inside.WaveWeight, 0.5f, 1e-5f);
    EXPECT_NEAR(inside.WaveChop, 0.25f, 1e-5f);
    EXPECT_TRUE(outside.Valid);
    EXPECT_NEAR(outside.WaveWeight, 1.0f, 1e-5f);
    EXPECT_NEAR(outside.WaveChop, 1.0f, 1e-5f);
}

TEST(OceanQueryTests, WaterBodyLocalWaveAddsQueryHeight)
{
    OceanRenderFeature feature;

    OceanParamsGPU params{};
    params.SeaLevel = 2.0f;
    params.Time = 0.0f;
    feature.SetParams(params);

    OceanRenderFeature::WaterBodyWaveBox box{};
    box.CenterX = 1.0f;
    box.CenterZ = 0.0f;
    box.HalfX = 2.0f;
    box.HalfZ = 2.0f;
    box.Weight = 1.0f;
    box.LocalAmplitude = 1.5f;
    box.LocalWavelength = 4.0f;
    box.LocalDirX = 1.0f;
    box.LocalDirZ = 0.0f;
    feature.SetWaterBodyWaveOverrides({box}, {});

    const OceanSurfaceSample inside = feature.SampleSurface(1.0f, 0.0f);
    const OceanSurfaceSample outside = feature.SampleSurface(5.0f, 0.0f);

    EXPECT_TRUE(inside.Valid);
    EXPECT_NEAR(inside.Height, 3.5f, 1e-4f);
    EXPECT_NEAR(inside.WaveWeight, 1.0f, 1e-5f);
    EXPECT_NEAR(inside.WaveChop, 1.0f, 1e-5f);
    EXPECT_NEAR(inside.LocalWaveHeight, 1.5f, 1e-4f);
    EXPECT_TRUE(outside.Valid);
    EXPECT_NEAR(outside.Height, 2.0f, 1e-4f);
    EXPECT_NEAR(outside.LocalWaveHeight, 0.0f, 1e-5f);
}

TEST(OceanQueryTests, WaterBodyLocalFFTBlendIsReportedByQueries)
{
    OceanRenderFeature feature;

    OceanParamsGPU params{};
    params.SeaLevel = 2.0f;
    feature.SetParams(params);

    OceanRenderFeature::WaterBodyWaveBox box{};
    box.CenterX = 1.0f;
    box.CenterZ = 0.0f;
    box.HalfX = 2.0f;
    box.HalfZ = 2.0f;
    box.Weight = 1.0f;
    box.LocalFFTBlend = 1.0f;
    box.LocalFFTStream = 6u;
    feature.SetWaterBodyWaveOverrides({box}, {});

    const OceanSurfaceSample inside = feature.SampleSurface(1.0f, 0.0f);
    const OceanSurfaceSample outside = feature.SampleSurface(5.0f, 0.0f);

    EXPECT_TRUE(inside.Valid);
    EXPECT_NEAR(inside.LocalFFTBlend, 1.0f, 1e-5f);
    EXPECT_EQ(inside.LocalFFTStream, 6u);
    EXPECT_TRUE(outside.Valid);
    EXPECT_NEAR(outside.LocalFFTBlend, 0.0f, 1e-5f);
    EXPECT_EQ(outside.LocalFFTStream, 0u);
}

TEST(OceanQueryTests, PolygonWaterBodyLocalFFTBlendIsReported)
{
    OceanRenderFeature feature;

    OceanRenderFeature::WaterBodyWavePolygon polygon{};
    polygon.PointCount = 3u;
    polygon.X[0] = 0.0f;
    polygon.Z[0] = 0.0f;
    polygon.X[1] = 8.0f;
    polygon.Z[1] = 0.0f;
    polygon.X[2] = 0.0f;
    polygon.Z[2] = 8.0f;
    polygon.Weight = 1.0f;
    polygon.LocalFFTBlend = 0.75f;
    polygon.LocalFFTStream = 7u;
    feature.SetWaterBodyWaveOverrides({}, {polygon});

    const auto inside = feature.SampleWaterBodyWaveOverride(1.0f, 1.0f, 0.0f);
    const auto outside = feature.SampleWaterBodyWaveOverride(7.0f, 7.0f, 0.0f);

    EXPECT_NEAR(inside.LocalFFTBlend, 0.75f, 1e-5f);
    EXPECT_EQ(inside.LocalFFTStream, 7u);
    EXPECT_NEAR(outside.LocalFFTBlend, 0.0f, 1e-5f);
    EXPECT_EQ(outside.LocalFFTStream, 0u);
}

TEST(OceanQueryTests, WaterBodyLocalWavePacketAddsQueryHeight)
{
    OceanRenderFeature feature;

    OceanParamsGPU params{};
    params.SeaLevel = 2.0f;
    params.Time = 0.0f;
    feature.SetParams(params);

    OceanRenderFeature::WaterBodyWaveBox box{};
    box.CenterX = 1.0f;
    box.CenterZ = 0.0f;
    box.HalfX = 2.0f;
    box.HalfZ = 2.0f;
    box.Weight = 1.0f;
    box.LocalWaveCount = 2u;
    box.LocalAmplitude = 1.0f;
    box.LocalWavelength = 4.0f;
    box.LocalDirX = 1.0f;
    box.LocalDirZ = 0.0f;
    box.LocalWaveExtra[0][0] = 0.5f;
    box.LocalWaveExtra[0][1] = 4.0f;
    box.LocalWaveExtra[0][2] = 1.0f;
    box.LocalWaveExtra[0][3] = 0.0f;
    feature.SetWaterBodyWaveOverrides({box}, {});

    const OceanSurfaceSample inside = feature.SampleSurface(1.0f, 0.0f);
    const OceanSurfaceSample outside = feature.SampleSurface(5.0f, 0.0f);

    EXPECT_TRUE(inside.Valid);
    EXPECT_NEAR(inside.Height, 3.5f, 1e-4f);
    EXPECT_TRUE(outside.Valid);
    EXPECT_NEAR(outside.Height, 2.0f, 1e-4f);
}

TEST(OceanQueryTests, WaterBodyWaveFeatherBlendsQueryHeightAtEdge)
{
    OceanRenderFeature feature;

    OceanParamsGPU params{};
    params.SeaLevel = 2.0f;
    params.Time = 0.0f;
    feature.SetParams(params);

    OceanRenderFeature::WaterBodyWaveBox box{};
    box.CenterX = 0.0f;
    box.CenterZ = 0.0f;
    box.HalfX = 10.0f;
    box.HalfZ = 10.0f;
    box.Weight = 1.0f;
    box.Chop = 0.2f;
    box.Feather = 4.0f;
    box.LocalAmplitude = 1.0f;
    box.LocalWavelength = 4.0f;
    box.LocalDirX = 1.0f;
    box.LocalDirZ = 0.0f;
    feature.SetWaterBodyWaveOverrides({box}, {});

    const OceanSurfaceSample edge = feature.SampleSurface(9.0f, 0.0f);
    const OceanSurfaceSample outside = feature.SampleSurface(11.0f, 0.0f);

    EXPECT_TRUE(edge.Valid);
    EXPECT_NEAR(edge.Height, 2.15625f, 1e-4f);
    EXPECT_NEAR(edge.WaveChop, 0.875f, 1e-5f);
    EXPECT_NEAR(edge.LocalWaveHeight, 0.15625f, 1e-5f);
    EXPECT_TRUE(outside.Valid);
    EXPECT_NEAR(outside.Height, 2.0f, 1e-4f);
    EXPECT_NEAR(outside.WaveChop, 1.0f, 1e-5f);
}

TEST(OceanQueryTests, WaterBodyLocalWaveTiltsQueryNormal)
{
    OceanRenderFeature feature;

    OceanParamsGPU params{};
    params.SeaLevel = 0.0f;
    params.Time = 0.0f;
    feature.SetParams(params);

    OceanRenderFeature::WaterBodyWaveBox box{};
    box.CenterX = 0.0f;
    box.CenterZ = 0.0f;
    box.HalfX = 2.0f;
    box.HalfZ = 2.0f;
    box.Weight = 1.0f;
    box.LocalAmplitude = 1.0f;
    box.LocalWavelength = 4.0f;
    box.LocalDirX = 1.0f;
    box.LocalDirZ = 0.0f;
    feature.SetWaterBodyWaveOverrides({box}, {});

    const OceanSurfaceSample sample = feature.SampleSurface(0.0f, 0.0f);

    constexpr float32 k = 1.57079632679f;
    const float32 expectedNy = 1.0f / std::sqrt(k * k + 1.0f);
    const float32 expectedNx = -k * expectedNy;

    EXPECT_TRUE(sample.Valid);
    EXPECT_NEAR(sample.Height, 0.0f, 1e-5f);
    EXPECT_NEAR(sample.LocalWaveHeight, 0.0f, 1e-5f);
    EXPECT_NEAR(sample.LocalWaveSlopeX, k, 1e-5f);
    EXPECT_NEAR(sample.LocalWaveSlopeZ, 0.0f, 1e-5f);
    EXPECT_NEAR(sample.NormalWS[0], expectedNx, 1e-5f);
    EXPECT_NEAR(sample.NormalWS[1], expectedNy, 1e-5f);
    EXPECT_NEAR(sample.NormalWS[2], 0.0f, 1e-5f);
}

TEST(OceanQueryTests, SurfaceSampleReportsDepthDerivedShorelineFactors)
{
    OceanRenderFeature feature;

    OceanParamsGPU params{};
    params.SeaLevel = 0.0f;
    params.SubSurfaceDepthMax = 10.0f;
    params.SubSurfaceDepthPower = 2.0f;
    params.ShorelineFoamMaxDepth = 4.0f;
    params.ShorelineFoamStrength = 2.0f;
    params.FoamAmount = 0.5f;
    feature.SetParams(params);

    OceanSeabedGPU seabed{};
    seabed.OriginExtent[2] = 10.0f;
    seabed.OriginExtent[3] = 10.0f;
    seabed.HeightSlope[0] = -2.0f;
    feature.GetSeabedDepth().SetSeabeds(&seabed, 1u, nullptr, 0u, params.SeaLevel, params.SubSurfaceDepthMax);

    const OceanSurfaceSample sample = feature.SampleSurface(0.0f, 0.0f);

    EXPECT_TRUE(sample.Valid);
    EXPECT_TRUE(sample.HasWaterDepth);
    EXPECT_NEAR(sample.WaterDepth, 2.0f, 1e-5f);
    EXPECT_NEAR(sample.ShallowWaterFactor, 0.64f, 1e-5f);
    EXPECT_NEAR(sample.ShorelineContactFactor, 0.5f, 1e-5f);
    EXPECT_NEAR(sample.ShorelineFoamFactor, 0.5f, 1e-5f);
}

TEST(OceanQueryTests, DeferredSurfaceQueryBatchCompletesWhenProcessed)
{
    OceanRenderFeature feature;

    OceanParamsGPU params{};
    params.SeaLevel = 3.0f;
    feature.SetParams(params);

    const OceanSurfaceQueryPoint points[2] = {{1.0f, 2.0f}, {-4.0f, 5.0f}};
    const OceanSurfaceQueryHandle handle = feature.EnqueueSurfaceQueries(points, 2u);

    EXPECT_NE(handle, OceanSurfaceQueryHandle{0});
    EXPECT_FALSE(feature.IsSurfaceQueryReady(handle));

    feature.ProcessQueuedSurfaceQueries();

    OceanSurfaceSample samples[2]{};
    uint32 count = 0u;
    EXPECT_TRUE(feature.IsSurfaceQueryReady(handle));
    EXPECT_TRUE(feature.CopySurfaceQueryResults(handle, samples, 2u, &count));
    EXPECT_EQ(count, 2u);
    EXPECT_TRUE(samples[0].Valid);
    EXPECT_TRUE(samples[1].Valid);
    EXPECT_FLOAT_EQ(samples[0].Height, 3.0f);
    EXPECT_FLOAT_EQ(samples[1].Height, 3.0f);
    EXPECT_FALSE(feature.IsSurfaceQueryReady(handle));
}

TEST(OceanQueryTests, SeabedDepthSamplesAnalyticPlane)
{
    OceanSeabedDepth depthCache;
    OceanSeabedGPU seabed{};
    seabed.OriginExtent[0] = 0.0f;
    seabed.OriginExtent[1] = 0.0f;
    seabed.OriginExtent[2] = 10.0f;
    seabed.OriginExtent[3] = 20.0f;
    seabed.HeightSlope[0] = -4.0f;
    seabed.HeightSlope[1] = -0.25f;
    seabed.HeightSlope[2] = 0.5f;

    depthCache.SetSeabeds(&seabed, 1, nullptr, 0, 2.0f, kDepthBandSaturationMeters);

    float32 depth = 0.0f;
    EXPECT_TRUE(depthCache.SampleDepth(4.0f, 2.0f, depth));
    EXPECT_NEAR(depth, 6.0f, 1e-5f);
    EXPECT_FALSE(depthCache.SampleDepth(11.0f, 0.0f, depth));
}

TEST(OceanQueryTests, DynamicDepthContributorOverridesToShallowerDepth)
{
    OceanSeabedDepth depthCache;

    OceanSeabedGPU seabed{};
    seabed.OriginExtent[2] = 10.0f;
    seabed.OriginExtent[3] = 10.0f;
    seabed.HeightSlope[0] = -8.0f;

    OceanDepthContributorGPU contributor{};
    contributor.OriginExtent[2] = 2.0f;
    contributor.OriginExtent[3] = 2.0f;
    contributor.DepthShape[0] = 1.5f;
    contributor.DepthShape[1] = 0.0f;
    contributor.DepthShape[2] = 1.0f;

    depthCache.SetSeabeds(&seabed, 1, &contributor, 1, 0.0f, kDepthBandSaturationMeters);

    float32 depth = 0.0f;
    EXPECT_TRUE(depthCache.SampleDepth(0.0f, 0.0f, depth));
    EXPECT_NEAR(depth, 1.5f, 1e-5f);
    EXPECT_TRUE(depthCache.SampleDepth(5.0f, 5.0f, depth));
    EXPECT_NEAR(depth, 8.0f, 1e-5f);
}

// A feathered depth band ramps through the depth where the shallow terms
// saturate and continues its bank slope one feather past the edge, never
// stepping to the deep-water sentinel there, so the cascade can reconstruct
// its edge.
TEST(OceanQueryTests, DepthContributorFeatherRampsThroughTheSaturationDepth)
{
    OceanSeabedDepth depthCache;
    OceanDepthContributorGPU contributor{};
    contributor.OriginExtent[2] = 4.0f;
    contributor.OriginExtent[3] = 4.0f;
    contributor.DepthShape[0] = 2.0f;
    contributor.DepthShape[1] = 2.0f;
    contributor.DepthShape[2] = 0.0f;
    depthCache.SetSeabeds(nullptr, 0, &contributor, 1, 0.0f, kDepthBandSaturationMeters);

    float32 depth = 0.0f;
    ASSERT_TRUE(depthCache.SampleDepth(0.0f, 0.0f, depth));
    EXPECT_FLOAT_EQ(depth, 2.0f) << "full coverage inside the feather";
    ASSERT_TRUE(depthCache.SampleDepth(3.0f, 0.0f, depth));
    EXPECT_FLOAT_EQ(depth, (kDepthBandSaturationMeters + 2.0f) * 0.5f) << "half way through the feather";
    ASSERT_TRUE(depthCache.SampleDepth(5.0f, 0.0f, depth));
    EXPECT_FLOAT_EQ(depth, kDepthBandSaturationMeters + (kDepthBandSaturationMeters - 2.0f) * 0.5f)
        << "the bank slope continues half a feather outside the edge";
    EXPECT_FALSE(depthCache.SampleDepth(6.5f, 0.0f, depth)) << "and stops one feather outside it";
    EXPECT_FLOAT_EQ(OceanDepthBandDepth(20.0f, kDepthBandSaturationMeters, 0.5f, 2.0f), 20.0f)
        << "a band deeper than saturation has no edge ramp";
    EXPECT_GE(OceanDepthBandDepth(20.0f, kDepthBandSaturationMeters, -0.5f, 2.0f), kOceanDepthBandOutside)
        << "and no bank outside its edge";
}

TEST(OceanQueryTests, DepthCacheAssetSamplesAndRoundTrips)
{
    OceanDepthCacheAssetDesc desc{};
    desc.Width = 2;
    desc.Height = 2;
    desc.OriginX = 0.0f;
    desc.OriginZ = 0.0f;
    desc.SizeX = 2.0f;
    desc.SizeZ = 2.0f;
    desc.DeepWaterDepth = 60000.0f;

    OceanDepthCacheAsset cache;
    ASSERT_TRUE(cache.Reset(desc, {2.0f, 4.0f, 6.0f, 8.0f}));

    float32 depth = 0.0f;
    EXPECT_TRUE(cache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 2.0f, 1e-5f);
    EXPECT_TRUE(cache.SampleDepth(1.0f, 1.0f, depth));
    EXPECT_NEAR(depth, 5.0f, 1e-5f);
    EXPECT_FALSE(cache.SampleDepth(2.1f, 1.0f, depth));

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "GameEngine_OceanDepthCacheAsset_Test.oceandepth";
    std::filesystem::remove(path);
    ASSERT_TRUE(cache.SaveBinary(path));

    OceanDepthCacheAsset loaded;
    ASSERT_TRUE(loaded.LoadBinary(path));
    EXPECT_TRUE(loaded.SampleDepth(1.0f, 1.0f, depth));
    EXPECT_NEAR(depth, 5.0f, 1e-5f);
    std::filesystem::remove(path);
}

TEST(OceanQueryTests, DepthCacheCompositionMergesUnionAndOverlap)
{
    OceanDepthCacheAssetDesc wideDesc{};
    wideDesc.Width = 2;
    wideDesc.Height = 1;
    wideDesc.OriginX = 0.0f;
    wideDesc.OriginZ = 0.0f;
    wideDesc.SizeX = 2.0f;
    wideDesc.SizeZ = 1.0f;

    OceanDepthCacheAsset wide;
    ASSERT_TRUE(wide.Reset(wideDesc, {10.0f, 10.0f}));

    OceanDepthCacheAssetDesc shallowDesc{};
    shallowDesc.Width = 1;
    shallowDesc.Height = 1;
    shallowDesc.OriginX = 1.0f;
    shallowDesc.OriginZ = 0.0f;
    shallowDesc.SizeX = 1.0f;
    shallowDesc.SizeZ = 1.0f;

    OceanDepthCacheAsset shallow;
    ASSERT_TRUE(shallow.Reset(shallowDesc, {3.0f}));

    const OceanDepthCacheAsset caches[] = {wide, shallow};
    OceanDepthCacheAsset composed;
    ASSERT_TRUE(ComposeOceanDepthCaches(caches, 2, composed));

    const OceanDepthCacheAssetDesc& desc = composed.GetDesc();
    EXPECT_EQ(desc.Width, 2u);
    EXPECT_EQ(desc.Height, 1u);
    EXPECT_FLOAT_EQ(desc.OriginX, 0.0f);
    EXPECT_FLOAT_EQ(desc.SizeX, 2.0f);

    float32 depth = 0.0f;
    ASSERT_TRUE(composed.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 10.0f, 1e-5f);
    ASSERT_TRUE(composed.SampleDepth(1.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 3.0f, 1e-5f);
}

TEST(OceanQueryTests, SeabedDepthUsesAtlasPagesForFarApartSavedCaches)
{
    OceanDepthCacheAssetDesc leftDesc{};
    leftDesc.Width = 1;
    leftDesc.Height = 1;
    leftDesc.OriginX = 0.0f;
    leftDesc.OriginZ = 0.0f;
    leftDesc.SizeX = 1.0f;
    leftDesc.SizeZ = 1.0f;

    OceanDepthCacheAsset left;
    ASSERT_TRUE(left.Reset(leftDesc, {4.0f}));

    OceanDepthCacheAssetDesc rightDesc = leftDesc;
    rightDesc.OriginX = 20000.0f;
    rightDesc.OriginZ = 0.0f;

    OceanDepthCacheAsset right;
    ASSERT_TRUE(right.Reset(rightDesc, {7.0f}));

    const OceanDepthCacheAsset caches[] = {left, right};
    OceanDepthCacheAsset composed;
    EXPECT_FALSE(ComposeOceanDepthCaches(caches, 2u, composed));

    OceanSeabedDepth depthCache;
    ASSERT_TRUE(depthCache.SetSavedDepthCaches(caches, 2u));

    float32 depth = 0.0f;
    EXPECT_TRUE(depthCache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 4.0f, 1e-5f);
    EXPECT_TRUE(depthCache.SampleDepth(20000.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 7.0f, 1e-5f);
}

TEST(OceanQueryTests, DepthCacheExtensionIsRegisteredAsBinaryAsset)
{
    EXPECT_EQ(GetAssetTypeFromExtension(".oceandepth"), AssetType::OceanDepthCache);
    EXPECT_EQ(AssetTypeToString(AssetType::OceanDepthCache), "OceanDepthCache");
    EXPECT_TRUE(IsBinaryAssetType(AssetType::OceanDepthCache));

    const Vector<String> extensions = GetDefaultExtensionsForAssetType(AssetType::OceanDepthCache);
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), ".oceandepth"), extensions.end());

    EXPECT_EQ(GetAssetTypeFromExtension(".oceanspectrum"), AssetType::OceanWaveSpectrum);
    EXPECT_EQ(AssetTypeToString(AssetType::OceanWaveSpectrum), "OceanWaveSpectrum");
    EXPECT_TRUE(IsTextBasedAssetType(AssetType::OceanWaveSpectrum));

    const Vector<String> spectrumExtensions =
        GetDefaultExtensionsForAssetType(AssetType::OceanWaveSpectrum);
    EXPECT_NE(std::find(spectrumExtensions.begin(), spectrumExtensions.end(), ".oceanspectrum"),
              spectrumExtensions.end());
}

TEST(OceanQueryTests, DepthCacheBakerSamplesHeightCallbackAtTexelCenters)
{
    OceanDepthCacheBakeDesc desc{};
    desc.Width = 2;
    desc.Height = 2;
    desc.OriginX = 0.0f;
    desc.OriginZ = 0.0f;
    desc.SizeX = 2.0f;
    desc.SizeZ = 2.0f;
    desc.SeaLevel = 0.0f;

    SlopedFloor floor{};
    floor.Base = -1.0f;
    floor.SlopeX = -1.0f;
    floor.SlopeZ = -2.0f;

    OceanDepthCacheAsset cache;
    ASSERT_TRUE(BakeOceanDepthCache(desc, &SampleSlopedFloor, &floor, cache));

    const std::vector<float32>& depths = cache.GetDepths();
    ASSERT_EQ(depths.size(), 4u);
    EXPECT_NEAR(depths[0], 2.5f, 1e-5f); // sample at (0.5, 0.5): floor = -2.5
    EXPECT_NEAR(depths[1], 3.5f, 1e-5f); // sample at (1.5, 0.5): floor = -3.5
    EXPECT_NEAR(depths[2], 4.5f, 1e-5f); // sample at (0.5, 1.5): floor = -4.5
    EXPECT_NEAR(depths[3], 5.5f, 1e-5f); // sample at (1.5, 1.5): floor = -5.5
}

TEST(OceanQueryTests, DepthCacheBakerSamplesHeightfieldSources)
{
    OceanDepthCacheBakeDesc desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.OriginX = 0.0f;
    desc.OriginZ = 0.0f;
    desc.SizeX = 1.0f;
    desc.SizeZ = 1.0f;
    desc.SeaLevel = 5.0f;

    const float32 samples[] = {0.2f, 0.2f, 0.2f, 0.2f};
    OceanDepthCacheHeightfieldSource source{};
    source.Samples = samples;
    source.Width = 2;
    source.Height = 2;
    source.OriginX = 0.0f;
    source.OriginZ = 0.0f;
    source.SizeX = 1.0f;
    source.SizeZ = 1.0f;
    source.HeightScale = 10.0f;
    source.WorldOriginY = -5.0f;
    source.LayerMask = 0x1u;

    OceanDepthCacheAsset cache;
    ASSERT_TRUE(BakeOceanDepthCacheFromHeightfields(desc, &source, 1, 0x1u, cache));

    float32 depth = 0.0f;
    ASSERT_TRUE(cache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 8.0f, 1e-5f);
}

TEST(OceanQueryTests, DepthCacheBakerFiltersAndOverlapsHeightfieldLayers)
{
    OceanDepthCacheBakeDesc desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.OriginX = 0.0f;
    desc.OriginZ = 0.0f;
    desc.SizeX = 1.0f;
    desc.SizeZ = 1.0f;
    desc.SeaLevel = 0.0f;

    const float32 deepFloor[] = {-10.0f};
    const float32 shallowFloor[] = {-2.0f};
    OceanDepthCacheHeightfieldSource sources[2]{};
    sources[0].Samples = deepFloor;
    sources[0].Width = 1;
    sources[0].Height = 1;
    sources[0].SizeX = 1.0f;
    sources[0].SizeZ = 1.0f;
    sources[0].LayerMask = 0x1u;
    sources[1] = sources[0];
    sources[1].Samples = shallowFloor;
    sources[1].LayerMask = 0x2u;

    OceanDepthCacheAsset cache;
    float32 depth = 0.0f;

    ASSERT_TRUE(BakeOceanDepthCacheFromHeightfields(desc, sources, 2, 0x1u, cache));
    ASSERT_TRUE(cache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 10.0f, 1e-5f);

    ASSERT_TRUE(BakeOceanDepthCacheFromHeightfields(desc, sources, 2, 0x2u, cache));
    ASSERT_TRUE(cache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 2.0f, 1e-5f);

    ASSERT_TRUE(BakeOceanDepthCacheFromHeightfields(desc, sources, 2, 0x3u, cache));
    ASSERT_TRUE(cache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 2.0f, 1e-5f);
}

TEST(OceanQueryTests, DepthCacheBakerCapturesMeshGeometry)
{
    OceanDepthCacheBakeDesc desc{};
    desc.Width = 2;
    desc.Height = 2;
    desc.OriginX = 0.0f;
    desc.OriginZ = 0.0f;
    desc.SizeX = 2.0f;
    desc.SizeZ = 2.0f;
    desc.SeaLevel = 1.0f;

    const Mathematics::Vector3 positions[] = {
        {0.0f, -3.0f, 0.0f},
        {2.0f, -3.0f, 0.0f},
        {2.0f, -3.0f, 2.0f},
        {0.0f, -3.0f, 2.0f},
    };
    const uint32 indices[] = {0, 1, 2, 0, 2, 3};

    OceanDepthCacheMeshSource source{};
    source.Positions = positions;
    source.VertexCount = 4;
    source.Indices = indices;
    source.IndexCount = 6;
    source.LayerMask = 0x1u;

    OceanDepthCacheAsset cache;
    ASSERT_TRUE(BakeOceanDepthCacheFromMeshes(desc, &source, 1, 0x1u, cache));

    const std::vector<float32>& depths = cache.GetDepths();
    ASSERT_EQ(depths.size(), 4u);
    for (float32 depth : depths)
        EXPECT_NEAR(depth, 4.0f, 1e-5f);
}

TEST(OceanQueryTests, DepthCacheBakerTransformsMeshSources)
{
    OceanDepthCacheBakeDesc desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.OriginX = 10.0f;
    desc.OriginZ = 20.0f;
    desc.SizeX = 1.0f;
    desc.SizeZ = 1.0f;
    desc.SeaLevel = 5.0f;

    const Mathematics::Vector3 positions[] = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f},
    };
    const uint32 indices[] = {0, 1, 2, 0, 2, 3};

    OceanDepthCacheMeshSource source{};
    source.Positions = positions;
    source.VertexCount = 4;
    source.Indices = indices;
    source.IndexCount = 6;
    source.UseLocalToWorld = true;
    source.LocalToWorld[12] = 10.0f;
    source.LocalToWorld[13] = -3.0f;
    source.LocalToWorld[14] = 20.0f;

    OceanDepthCacheAsset cache;
    ASSERT_TRUE(BakeOceanDepthCacheFromMeshes(desc, &source, 1, 0xFFFFFFFFu, cache));

    float32 depth = 0.0f;
    ASSERT_TRUE(cache.SampleDepth(10.5f, 20.5f, depth));
    EXPECT_NEAR(depth, 8.0f, 1e-5f);
}

TEST(OceanQueryTests, DepthCacheBakerFiltersAndOverlapsMeshLayers)
{
    OceanDepthCacheBakeDesc desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.OriginX = 0.0f;
    desc.OriginZ = 0.0f;
    desc.SizeX = 1.0f;
    desc.SizeZ = 1.0f;
    desc.SeaLevel = 0.0f;

    const Mathematics::Vector3 deepPositions[] = {
        {0.0f, -10.0f, 0.0f},
        {1.0f, -10.0f, 0.0f},
        {1.0f, -10.0f, 1.0f},
        {0.0f, -10.0f, 1.0f},
    };
    const Mathematics::Vector3 shallowPositions[] = {
        {0.0f, -2.0f, 0.0f},
        {1.0f, -2.0f, 0.0f},
        {1.0f, -2.0f, 1.0f},
        {0.0f, -2.0f, 1.0f},
    };
    const uint32 indices[] = {0, 1, 2, 0, 2, 3};

    OceanDepthCacheMeshSource sources[2]{};
    sources[0].Positions = deepPositions;
    sources[0].VertexCount = 4;
    sources[0].Indices = indices;
    sources[0].IndexCount = 6;
    sources[0].LayerMask = 0x1u;
    sources[1] = sources[0];
    sources[1].Positions = shallowPositions;
    sources[1].LayerMask = 0x2u;

    OceanDepthCacheAsset cache;
    float32 depth = 0.0f;

    ASSERT_TRUE(BakeOceanDepthCacheFromMeshes(desc, sources, 2, 0x1u, cache));
    ASSERT_TRUE(cache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 10.0f, 1e-5f);

    ASSERT_TRUE(BakeOceanDepthCacheFromMeshes(desc, sources, 2, 0x2u, cache));
    ASSERT_TRUE(cache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 2.0f, 1e-5f);

    ASSERT_TRUE(BakeOceanDepthCacheFromMeshes(desc, sources, 2, 0x3u, cache));
    ASSERT_TRUE(cache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 2.0f, 1e-5f);
}

TEST(OceanQueryTests, SavedDepthCacheParticipatesInSeabedDepthSamples)
{
    OceanDepthCacheAssetDesc desc{};
    desc.Width = 2;
    desc.Height = 2;
    desc.OriginX = 0.0f;
    desc.OriginZ = 0.0f;
    desc.SizeX = 2.0f;
    desc.SizeZ = 2.0f;

    OceanDepthCacheAsset cache;
    ASSERT_TRUE(cache.Reset(desc, {4.0f, 4.0f, 4.0f, 4.0f}));

    OceanSeabedDepth depthCache;
    ASSERT_TRUE(depthCache.SetSavedDepthCache(cache));
    EXPECT_TRUE(depthCache.HasSeabeds());

    float32 depth = 0.0f;
    EXPECT_TRUE(depthCache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 4.0f, 1e-5f);
    EXPECT_FALSE(depthCache.SampleDepth(3.0f, 3.0f, depth));

    OceanSeabedGPU seabed{};
    seabed.OriginExtent[0] = 0.5f;
    seabed.OriginExtent[1] = 0.5f;
    seabed.OriginExtent[2] = 10.0f;
    seabed.OriginExtent[3] = 10.0f;
    seabed.HeightSlope[0] = -2.0f;
    depthCache.SetSeabeds(&seabed, 1, nullptr, 0, 0.0f, kDepthBandSaturationMeters);

    EXPECT_TRUE(depthCache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 2.0f, 1e-5f);
}

TEST(OceanQueryTests, RasterDepthCaptureMarksSeabedDepthAvailable)
{
    OceanSeabedDepth depthCache;
    EXPECT_FALSE(depthCache.HasSeabeds());

    const ::GameEngine::Rendering::TextureHandle texture{42u};
    const ::GameEngine::Rendering::SamplerHandle sampler{7u};
    depthCache.SetRasterDepthCapture(texture, sampler, -10.0f, -20.0f,
                                     100.0f, 200.0f, 500.0f);

    EXPECT_TRUE(depthCache.HasSeabeds());
    float32 depth = 0.0f;
    EXPECT_FALSE(depthCache.SampleDepth(0.0f, 0.0f, depth));

    depthCache.ClearRasterDepthCapture();
    EXPECT_FALSE(depthCache.HasSeabeds());

    depthCache.SetRasterDepthCapture(texture, sampler, 0.0f, 0.0f,
                                     0.0f, 100.0f, 500.0f);
    EXPECT_FALSE(depthCache.HasSeabeds());
}

TEST(OceanQueryTests, SavedDepthCacheReloadsWhenSourceFileChanges)
{
    OceanDepthCacheAssetDesc desc{};
    desc.Width = 2;
    desc.Height = 2;
    desc.OriginX = 0.0f;
    desc.OriginZ = 0.0f;
    desc.SizeX = 2.0f;
    desc.SizeZ = 2.0f;

    OceanDepthCacheAsset first;
    ASSERT_TRUE(first.Reset(desc, {3.0f, 3.0f, 3.0f, 3.0f}));
    OceanDepthCacheAsset second;
    ASSERT_TRUE(second.Reset(desc, {7.0f, 7.0f, 7.0f, 7.0f}));

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "GameEngine_OceanDepthCacheReload_Test.oceandepth";
    std::filesystem::remove(path);
    ASSERT_TRUE(first.SaveBinary(path));

    std::error_code ec;
    const std::filesystem::file_time_type firstWriteTime = std::filesystem::last_write_time(path, ec);
    ASSERT_FALSE(ec);

    OceanSeabedDepth depthCache;
    ASSERT_TRUE(depthCache.LoadSavedDepthCacheFromFile(path));

    float32 depth = 0.0f;
    EXPECT_TRUE(depthCache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 3.0f, 1e-5f);

    ASSERT_TRUE(second.SaveBinary(path));
    std::filesystem::last_write_time(path, firstWriteTime + std::chrono::seconds(2), ec);
    ASSERT_FALSE(ec);

    ASSERT_TRUE(depthCache.LoadSavedDepthCacheFromFile(path));
    EXPECT_TRUE(depthCache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 7.0f, 1e-5f);

    std::filesystem::remove(path);
}

TEST(OceanQueryTests, SavedDepthCacheReloadsWhenRevisionChangesWithoutTimestampChange)
{
    OceanDepthCacheAssetDesc desc{};
    desc.Width = 2;
    desc.Height = 2;
    desc.OriginX = 0.0f;
    desc.OriginZ = 0.0f;
    desc.SizeX = 2.0f;
    desc.SizeZ = 2.0f;

    OceanDepthCacheAsset first;
    ASSERT_TRUE(first.Reset(desc, {3.0f, 3.0f, 3.0f, 3.0f}));
    OceanDepthCacheAsset second;
    ASSERT_TRUE(second.Reset(desc, {7.0f, 7.0f, 7.0f, 7.0f}));

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "GameEngine_OceanDepthCacheRevision_Test.oceandepth";
    std::filesystem::remove(path);
    ASSERT_TRUE(first.SaveBinary(path));

    std::error_code ec;
    const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, ec);
    ASSERT_FALSE(ec);

    OceanSavedDepthCacheSource source{};
    source.Path = path;

    OceanSeabedDepth depthCache;
    ASSERT_TRUE(depthCache.LoadSavedDepthCaches(&source, 1));

    float32 depth = 0.0f;
    EXPECT_TRUE(depthCache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 3.0f, 1e-5f);

    ASSERT_TRUE(second.SaveBinary(path));
    std::filesystem::last_write_time(path, writeTime, ec);
    ASSERT_FALSE(ec);

    ASSERT_TRUE(depthCache.LoadSavedDepthCaches(&source, 1));
    EXPECT_TRUE(depthCache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 3.0f, 1e-5f);

    ++source.CacheRevision;
    ASSERT_TRUE(depthCache.LoadSavedDepthCaches(&source, 1));
    EXPECT_TRUE(depthCache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 7.0f, 1e-5f);

    std::filesystem::remove(path);
}

TEST(OceanQueryTests, SavedDepthCacheLoadsAndComposesMultipleFiles)
{
    OceanDepthCacheAssetDesc leftDesc{};
    leftDesc.Width = 1;
    leftDesc.Height = 1;
    leftDesc.OriginX = 0.0f;
    leftDesc.OriginZ = 0.0f;
    leftDesc.SizeX = 1.0f;
    leftDesc.SizeZ = 1.0f;

    OceanDepthCacheAssetDesc rightDesc = leftDesc;
    rightDesc.OriginX = 1.0f;

    OceanDepthCacheAsset left;
    ASSERT_TRUE(left.Reset(leftDesc, {9.0f}));
    OceanDepthCacheAsset right;
    ASSERT_TRUE(right.Reset(rightDesc, {4.0f}));

    const std::filesystem::path leftPath =
        std::filesystem::temp_directory_path() / "GameEngine_OceanDepthCache_Left.oceandepth";
    const std::filesystem::path rightPath =
        std::filesystem::temp_directory_path() / "GameEngine_OceanDepthCache_Right.oceandepth";
    std::filesystem::remove(leftPath);
    std::filesystem::remove(rightPath);
    ASSERT_TRUE(left.SaveBinary(leftPath));
    ASSERT_TRUE(right.SaveBinary(rightPath));

    const std::filesystem::path paths[] = {leftPath, rightPath};
    OceanSeabedDepth depthCache;
    ASSERT_TRUE(depthCache.LoadSavedDepthCachesFromFiles(paths, 2));

    float32 depth = 0.0f;
    ASSERT_TRUE(depthCache.SampleDepth(0.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 9.0f, 1e-5f);
    ASSERT_TRUE(depthCache.SampleDepth(1.5f, 0.5f, depth));
    EXPECT_NEAR(depth, 4.0f, 1e-5f);

    std::filesystem::remove(leftPath);
    std::filesystem::remove(rightPath);
}

TEST(OceanQueryTests, FFTCollisionAssetBakesSamplesAndRoundTrips)
{
    OceanFFTCollisionAssetDesc desc{};
    desc.Resolution = 8u;
    desc.FrameCount = 4u;
    desc.TimeResolution = 0.25f;
    desc.LoopPeriod = 1.0f;
    desc.LoopLength = 8.0f;
    desc.SmallestWavelength = 1.0f;
    desc.SpatialResolution = 1.0f;
    desc.SeaLevel = 3.0f;
    desc.SpectrumHash = 0x12345678u;

    float32 period = desc.LoopLength;
    OceanFFTCollisionAsset asset;
    ASSERT_TRUE(BakeOceanFFTCollision(desc, &SamplePeriodicCollision, &period, asset));
    EXPECT_TRUE(asset.IsCompatible(desc.SpectrumHash));
    EXPECT_FALSE(asset.IsCompatible(desc.SpectrumHash + 1u));

    OceanSurfaceSample sample{};
    ASSERT_TRUE(asset.SampleSurface(2.0f, 0.0f, 0.0f, sample));
    EXPECT_TRUE(sample.Valid);
    EXPECT_EQ(sample.Source, OceanQuerySource::BakedFFTCPU);
    EXPECT_NEAR(sample.Height, 4.0f, 1e-4f);
    EXPECT_NEAR(sample.VelocityWS[1], 0.0f, 0.1f);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "GameEngine_OceanFFTCollision_Test.oceanfft";
    std::filesystem::remove(path);
    ASSERT_TRUE(asset.SaveBinary(path));
    OceanFFTCollisionAsset loaded;
    ASSERT_TRUE(loaded.LoadBinary(path));
    OceanSurfaceSample roundTrip{};
    ASSERT_TRUE(loaded.SampleSurface(2.0f, 0.0f, 0.0f, roundTrip));
    EXPECT_NEAR(roundTrip.Height, sample.Height, 1e-5f);
    std::filesystem::remove(path);
}

TEST(OceanQueryTests, CollisionProviderReusesOwnerAndKeepsLastCompletedResult)
{
    OceanRenderFeature feature;
    OceanParamsGPU params{};
    params.GerstnerWaveCount = 1u;
    params.Waves[0].DirectionX = 1.0f;
    params.Waves[0].Amplitude = 1.0f;
    params.Waves[0].Wavelength = 4.0f;
    params.Waves[0].Speed = 1.0f;
    feature.SetParams(params);

    auto& provider = static_cast<OceanFeatureCollisionProvider&>(feature.GetCollisionProvider());
    provider.SetMode(OceanCollisionProviderMode::AnalyticGerstner);
    OceanSurfaceQueryPoint points[2] = {{1.0f, 0.0f}, {2.0f, 0.0f}};
    OceanCollisionQueryDesc query{};
    query.Owner = 77u;
    query.Points = points;
    query.Count = 2u;
    query.Fields = OceanQueryField::All;
    const OceanCollisionQueryHandle first = provider.Submit(query);
    ASSERT_NE(first, 0u);
    EXPECT_EQ(provider.GetStatus(first), OceanQueryStatus::NotReady);
    provider.Update();
    EXPECT_EQ(provider.GetStatus(first), OceanQueryStatus::Success);

    OceanSurfaceSample completed[2]{};
    uint32 count = 0u;
    ASSERT_TRUE(provider.CopyResults(first, completed, nullptr, 2u, &count));
    EXPECT_EQ(count, 2u);
    const float32 priorHeight = completed[0].Height;

    params.Time = 0.5f;
    feature.SetParams(params);
    const OceanCollisionQueryHandle reused = provider.Submit(query);
    EXPECT_EQ(reused, first);
    EXPECT_EQ(provider.GetStatus(first), OceanQueryStatus::NotReady);
    // Pending work does not hide the last accurate buffer.
    ASSERT_TRUE(provider.CopyResults(first, completed, nullptr, 2u));
    EXPECT_NEAR(completed[0].Height, priorHeight, 1e-5f);
    provider.Update();
    ASSERT_TRUE(provider.CopyResults(first, completed, nullptr, 2u));
    EXPECT_NE(completed[0].Height, priorHeight);
    EXPECT_NE(completed[0].VelocityWS[1], 0.0f);
}

TEST(OceanQueryTests, CollisionProviderRejectsCapacityAndMismatchedBake)
{
    OceanRenderFeature feature;
    feature.SetParams(OceanParamsGPU{});
    auto& provider = static_cast<OceanFeatureCollisionProvider&>(feature.GetCollisionProvider());
    provider.SetMaxQueryPoints(1u);
    OceanSurfaceQueryPoint points[2]{};
    OceanCollisionQueryDesc query{};
    query.Owner = 1u;
    query.Points = points;
    query.Count = 2u;
    EXPECT_EQ(provider.Submit(query), 0u);

    OceanFFTCollisionAssetDesc desc{};
    desc.Resolution = 2u;
    desc.FrameCount = 2u;
    desc.TimeResolution = 0.5f;
    desc.LoopPeriod = 1.0f;
    desc.LoopLength = 2.0f;
    desc.SmallestWavelength = 1.0f;
    desc.SpatialResolution = 1.0f;
    desc.SpectrumHash = 123u;
    auto asset = std::make_shared<OceanFFTCollisionAsset>();
    ASSERT_TRUE(asset->Reset(desc, std::vector<OceanFFTDisplacementSample>(8u)));
    provider.SetMaxQueryPoints(4u);
    provider.SetBakedFFTAsset(asset);
    provider.SetExpectedSpectrumHash(456u);
    provider.SetMode(OceanCollisionProviderMode::BakedFFTCPU);
    query.Count = 1u;
    const auto handle = provider.Submit(query);
    provider.Update();
    EXPECT_EQ(provider.GetStatus(handle), OceanQueryStatus::AssetMismatch);
}

TEST(OceanQueryTests, ArbitraryGPUQueryQueuesWorldPointsAndSpatialLengths)
{
    OceanGPUQuery query;
    OceanSurfaceQueryPoint points[2]{};
    points[0] = {12.0f, -8.0f};
    points[1] = {-4000.0f, 9200.0f};
    const OceanGPUQueryToken token = query.Enqueue(points, 2u, 0.5f);
    ASSERT_NE(token, 0u);
    EXPECT_TRUE(query.HasPending());
    OceanParamsGPU ocean{};
    ocean.SeaLevel = 7.0f;
    ocean.FFTCascadeCount = 8u;
    ocean.Time = 3.0f;
    ASSERT_TRUE(query.PrepareNext(ocean, OceanCascadeLayoutGPU{}, false));
    OceanGPUQueryParamsGPU params{};
    query.FillParams(params);
    EXPECT_EQ(params.PointCount, 2u);
    EXPECT_EQ(params.FFTCascadeCount, 8u);
    EXPECT_FLOAT_EQ(params.SeaLevel, 7.0f);
    EXPECT_FLOAT_EQ(params.Points[0][0], 12.0f);
    EXPECT_FLOAT_EQ(params.Points[0][1], -8.0f);
    EXPECT_FLOAT_EQ(params.Points[0][2], 0.5f);
    EXPECT_FLOAT_EQ(params.Points[1][0], -4000.0f);
    EXPECT_FLOAT_EQ(params.Points[1][1], 9200.0f);
    EXPECT_FALSE(query.HasPending());
}

TEST(OceanQueryTests, OceanRayTracerFindsFirstSurfaceCrossing)
{
    OceanRenderFeature feature;
    OceanParamsGPU params{};
    params.SeaLevel = 2.0f;
    feature.SetParams(params);
    auto& provider = static_cast<OceanFeatureCollisionProvider&>(feature.GetCollisionProvider());
    provider.SetMode(OceanCollisionProviderMode::AnalyticGerstner);
    OceanRayTracer tracer(provider);
    OceanRayTraceDesc ray{};
    ray.Owner = 99u;
    ray.Origin[1] = 7.0f;
    ray.Direction[1] = -1.0f;
    ray.MaximumDistance = 10.0f;
    ray.SampleCount = 16u;
    const auto handle = tracer.Submit(ray);
    ASSERT_NE(handle, 0u);
    provider.Update();
    OceanRayTraceResult result{};
    ASSERT_TRUE(tracer.TryResolve(handle, result));
    EXPECT_TRUE(result.Hit);
    EXPECT_NEAR(result.Distance, 5.0f, 1e-4f);
    EXPECT_NEAR(result.PositionWS[1], 2.0f, 1e-4f);
}

TEST(OceanQueryTests, TypedInputSortAndBlendAreDeterministic)
{
    std::vector<OceanInputDrawPacket> packets(4);
    packets[0].Priority = 2; packets[0].EntityId = 8u;
    packets[1].Priority = -1; packets[1].EntityId = 4u;
    packets[2].Priority = 2; packets[2].EntityId = 2u;
    packets[3].Priority = -1; packets[3].EntityId = 1u;
    OceanInputDrawRegistry::Sort(packets);
    EXPECT_EQ(packets[0].EntityId, 1u);
    EXPECT_EQ(packets[1].EntityId, 4u);
    EXPECT_EQ(packets[2].EntityId, 2u);
    EXPECT_EQ(packets[3].EntityId, 8u);
    EXPECT_FLOAT_EQ(OceanInputDrawRegistry::ApplyBlend(
                        2.0f, 3.0f, 1.0f, OceanInputBlendMode::Additive), 5.0f);
    EXPECT_FLOAT_EQ(OceanInputDrawRegistry::ApplyBlend(
                        2.0f, 3.0f, 1.0f, OceanInputBlendMode::Multiply), 6.0f);
    EXPECT_FLOAT_EQ(OceanInputDrawRegistry::ApplyBlend(
                        2.0f, 3.0f, 0.5f, OceanInputBlendMode::Replace), 2.5f);
}

TEST(OceanQueryTests, OriginShiftPreservesAnalyticWavePhase)
{
    OceanRenderFeature feature;
    OceanParamsGPU params{};
    params.GerstnerWaveCount = 1u;
    params.Waves[0].DirectionX = 1.0f;
    params.Waves[0].Amplitude = 2.0f;
    params.Waves[0].Wavelength = 11.0f;
    params.Waves[0].Speed = 1.0f;
    feature.SetParams(params);
    const OceanSurfaceSample before = feature.SampleSurface(123.0f, 0.0f);
    feature.NotifyOriginShift(100.0f, 0.0f, 0.0f);
    const OceanSurfaceSample after = feature.SampleSurface(23.0f, 0.0f);
    EXPECT_NEAR(after.Height, before.Height, 1e-4f);
    EXPECT_NEAR(after.DisplacementWS[0], before.DisplacementWS[0], 1e-4f);
}

TEST(OceanQueryTests, TimeProvidersCoverPauseNetworkAndTimeline)
{
    OceanCustomTimeProvider custom;
    custom.SetScale(0.5f);
    custom.SetOffset(3.0f);
    OceanTimeSample sample = custom.Sample(10.0f, 2.0f);
    EXPECT_FLOAT_EQ(sample.Time, 8.0f);
    EXPECT_FLOAT_EQ(sample.DeltaTime, 1.0f);
    custom.SetFixedTime(true, 12.0f);
    sample = custom.Sample(20.0f, 1.0f);
    EXPECT_TRUE(sample.Paused);
    EXPECT_FLOAT_EQ(sample.Time, 12.0f);
    EXPECT_FLOAT_EQ(sample.DeltaTime, 0.0f);

    OceanNetworkOffsetTimeProvider network;
    network.SetNetworkOffset(5.0f);
    network.SetRate(2.0f);
    sample = network.Sample(4.0f, 0.5f);
    EXPECT_FLOAT_EQ(sample.Time, 13.0f);
    EXPECT_FLOAT_EQ(sample.DeltaTime, 1.0f);

    OceanTimelineTimeProvider timeline;
    timeline.SetTimelineTime(7.0f, true, 0.5f);
    sample = timeline.Sample(100.0f, 2.0f);
    EXPECT_FLOAT_EQ(sample.Time, 7.0f);
    EXPECT_FLOAT_EQ(sample.DeltaTime, 1.0f);
    sample = timeline.Sample(100.0f, 2.0f);
    EXPECT_FLOAT_EQ(sample.Time, 8.0f);
}

TEST(OceanQueryTests, OceanSettingsAssetRoundTripsAndAssetTypesAreRegistered)
{
    OceanSettingsAsset settings;
    settings.Kind = OceanSettingsKind::DynamicWaves;
    settings.DynamicWaves.SimulationFrequency = 90.0f;
    settings.DynamicWaves.MaximumSubsteps = 6u;
    settings.DynamicWaves.CourantNumber = 0.6f;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "GameEngine_OceanSettings_Test.oceansettings";
    std::filesystem::remove(path);
    ASSERT_TRUE(settings.SaveJson(path));
    OceanSettingsAsset loaded;
    ASSERT_TRUE(loaded.LoadJson(path));
    EXPECT_EQ(loaded.Kind, OceanSettingsKind::DynamicWaves);
    EXPECT_FLOAT_EQ(loaded.DynamicWaves.SimulationFrequency, 90.0f);
    EXPECT_EQ(loaded.DynamicWaves.MaximumSubsteps, 6u);
    std::filesystem::remove(path);

    EXPECT_EQ(GetAssetTypeFromExtension(".oceanfft"), AssetType::OceanFFTCollision);
    EXPECT_EQ(GetAssetTypeFromExtension(".oceansettings"), AssetType::OceanSettings);
    EXPECT_EQ(GetAssetTypeFromExtension(".oceanpreset"), AssetType::OceanPreset);
    EXPECT_TRUE(IsBinaryAssetType(AssetType::OceanFFTCollision));
    EXPECT_TRUE(IsTextBasedAssetType(AssetType::OceanSettings));
    EXPECT_TRUE(IsTextBasedAssetType(AssetType::OceanPreset));
}

TEST(OceanQueryTests, OceanPresetAppliesLiveBaseAndSparseOverrides)
{
    auto preset = std::make_shared<OceanPresetAsset>();
    preset->Set("Renderer.LodCount", uint64{3u});
    preset->Set("Renderer.GravityMultiplier", 0.75);

    Components::OceanRenderer renderer{};
    renderer.LodCount = 9u;
    const Components::OceanPresetOverride overrides[] = {{HashStringId("Renderer.LodCount")}};
    const auto typeId = ECS::GetComponentTypeId<Components::OceanRenderer>();
    EXPECT_EQ(preset->Apply("Renderer", typeId, &renderer, sizeof(renderer), overrides), 1u);
    EXPECT_EQ(renderer.LodCount, 9u);
    EXPECT_FLOAT_EQ(renderer.GravityMultiplier, 0.75f);

    OceanPresetInstance instance(preset);
    ASSERT_NE(instance.Resolve("Renderer.LodCount"), nullptr);
    instance.SetOverride("Renderer.LodCount", uint64{12u});
    EXPECT_TRUE(instance.IsOverridden("Renderer.LodCount"));
    ASSERT_NE(std::get_if<uint64>(instance.Resolve("Renderer.LodCount")), nullptr);
    EXPECT_EQ(*std::get_if<uint64>(instance.Resolve("Renderer.LodCount")), 12u);
    EXPECT_TRUE(instance.ResetField("Renderer.LodCount"));
    EXPECT_EQ(*std::get_if<uint64>(instance.Resolve("Renderer.LodCount")), 3u);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "GameEngine_OceanPreset_Test.oceanpreset";
    std::filesystem::remove(path);
    ASSERT_TRUE(preset->SaveJson(path));
    OceanPresetAsset loaded;
    ASSERT_TRUE(loaded.LoadJson(path));
    EXPECT_NE(loaded.Find("Renderer.GravityMultiplier"), nullptr);
    Components::OceanRenderer loadedRenderer{};
    loadedRenderer.LodCount = 9u;
    EXPECT_EQ(loaded.Apply("Renderer", typeId, &loadedRenderer, sizeof(loadedRenderer), overrides), 1u);
    EXPECT_EQ(loadedRenderer.LodCount, 9u);
    std::filesystem::remove(path);
}

TEST(OceanQueryTests, ContinuousRibbonCoversLongSegmentsWithoutStampBudgets)
{
    OceanRibbonStyle style{};
    style.Width = 2;
    style.FlowSpeed = 3;
    style.Depth = 4;
    style.Flags = 1u | 2u | 4u | 16u | 32u;
    style.UnderwaterDepth = 10;
    const std::vector<OceanRibbonPoint> points = {{0, 1, 0, 1, 0}, {1000, 1, 0, 1, 0}};
    OceanSplineRaster ribbon;
    ribbon.SetTriangles(BuildOceanRibbon(points, style));
    ASSERT_EQ(ribbon.GetTriangleCount(), 2u);
    EXPECT_TRUE(ribbon.Contains(500, 0));
    EXPECT_FALSE(ribbon.Contains(500, 2.1f));
    EXPECT_FALSE(ribbon.Contains(-1, 0));
    OceanCurrentSample flow{};
    ribbon.ApplyFlow(500, 0, flow); // shared diagonal must contribute only once
    EXPECT_TRUE(flow.Valid);
    EXPECT_FLOAT_EQ(flow.FlowX, 3);
    EXPECT_FLOAT_EQ(flow.FlowZ, 0);
    float depth = 0;
    EXPECT_TRUE(ribbon.SampleDepth(500, 0, depth));
    EXPECT_FLOAT_EQ(depth, 4);
    EXPECT_TRUE(ribbon.UnderwaterDepth(500, -2, 0, depth));
    EXPECT_FLOAT_EQ(depth, 3);
    EXPECT_FALSE(ribbon.UnderwaterDepth(500, -20, 0, depth));
    ribbon.RebaseOrigin(450, 20);
    EXPECT_TRUE(ribbon.Contains(50, -20));
    EXPECT_FALSE(ribbon.Contains(500, 0));
    ribbon.SetTriangles({});
    EXPECT_FALSE(ribbon.Contains(50, -20));
    EXPECT_FALSE(ribbon.HasField(1));
}

// The cascade bakes a ribbon draws into rerun only on a reported change, so an
// unchanged spline must not report one frame after frame.
TEST(OceanQueryTests, RibbonReportsATriangleChangeOnlyWhenTheTrianglesChange)
{
    OceanRibbonStyle style{};
    style.Width = 2;
    style.Flags = 2u | 16u;
    const auto triangles = BuildOceanRibbon({{0, 0, 0, 1, 0}, {10, 0, 0, 1, 0}}, style);
    OceanSplineRaster ribbon;
    EXPECT_TRUE(ribbon.SetTriangles(triangles));
    EXPECT_FALSE(ribbon.SetTriangles(triangles));
    style.Width = 3;
    EXPECT_TRUE(ribbon.SetTriangles(BuildOceanRibbon({{0, 0, 0, 1, 0}, {10, 0, 0, 1, 0}}, style)));
    EXPECT_TRUE(ribbon.SetTriangles({}));
    EXPECT_FALSE(ribbon.SetTriangles({}));
}

TEST(OceanQueryTests, RibbonJoinsStayContinuousAndDifferentCurrentsAccumulate)
{
    OceanRibbonStyle style{};
    style.Width = 2;
    style.FlowSpeed = 3;
    style.Flags = 2u | 16u;
    const std::vector<OceanRibbonPoint> points = {
        {0, 0, 0, 1, 0}, {10, 0, 0, 0.7071f, 0.7071f}, {10, 0, 10, 0, 1}};
    auto triangles = BuildOceanRibbon(points, style);
    OceanSplineRaster ribbon;
    ribbon.SetTriangles(triangles);
    EXPECT_TRUE(ribbon.Contains(9.5f, 0.5f));
    EXPECT_TRUE(ribbon.Contains(10, 5));
    style.Id = 1;
    style.FlowSpeed = 2;
    auto second = BuildOceanRibbon({{0, 0, 0, 1, 0}, {10, 0, 0, 1, 0}}, style);
    triangles.insert(triangles.end(), second.begin(), second.end());
    ribbon.SetTriangles(triangles);
    OceanCurrentSample flow{};
    ribbon.ApplyFlow(1, 0, flow);
    EXPECT_GT(flow.FlowX, 4.8f);
    EXPECT_LT(flow.FlowX, 5.1f);
}

// The triangles reach past the true edge so the clip and paint ramps can
// straddle it; every CPU query still ends at the true edge.
TEST(OceanQueryTests, RibbonApronCarriesTheEdgeRampButQueriesEndAtTheTrueEdge)
{
    OceanRibbonStyle style{};
    style.Width = 2;
    style.FlowSpeed = 3;
    style.Depth = 4;
    style.UnderwaterDepth = 10;
    style.Flags = 1u | 2u | 4u | 16u | 32u;
    const auto triangles = BuildOceanRibbon({{0, 0, 0, 1, 0}, {10, 0, 0, 1, 0}}, style);
    ASSERT_EQ(triangles.size(), 2u);
    float reach = 0;
    for (const auto &t : triangles)
        for (const float *v : {t.A, t.B, t.C})
            reach = std::max(reach, std::abs(v[1]));
    EXPECT_FLOAT_EQ(reach, 4.0f) << "the apron is one half-width by default";
    float bary[3];
    ASSERT_TRUE(SampleOceanRibbonTriangle(triangles[0], 5, -3, bary) ||
                SampleOceanRibbonTriangle(triangles[1], 5, -3, bary));
    OceanSplineRaster ribbon;
    ribbon.SetTriangles(triangles);
    EXPECT_TRUE(ribbon.Contains(5, 1.9f));
    EXPECT_FALSE(ribbon.Contains(5, 2.1f));
    EXPECT_FALSE(ribbon.Contains(5, -3.5f));
    float depth = 0;
    EXPECT_FALSE(ribbon.SampleDepth(5, 2.5f, depth));
    EXPECT_FALSE(ribbon.UnderwaterDepth(5, -1, -2.5f, depth));
    OceanCurrentSample flow{};
    ribbon.ApplyFlow(5, 3, flow);
    EXPECT_FALSE(flow.Valid);

    style.Feather = 10;
    const auto feathered = BuildOceanRibbon({{0, 0, 0, 1, 0}, {10, 0, 0, 1, 0}}, style);
    reach = 0;
    for (const auto &t : feathered)
        for (const float *v : {t.A, t.B, t.C})
            reach = std::max(reach, std::abs(v[1]));
    EXPECT_FLOAT_EQ(reach, 7.0f) << "a wide feather widens the apron to half the feather";
}

TEST(OceanQueryTests, RibbonDepthFeatherAndUnderwaterHierarchySurviveRebase)
{
    OceanRibbonStyle style{};
    style.Width = 2;
    style.Depth = 4;
    style.DepthFeather = 1;
    style.DepthSaturation = kDepthBandSaturationMeters;
    style.UnderwaterDepth = 10;
    style.Flags = 1u | 32u;
    std::vector<OceanRibbonPoint> points;
    for (uint32 i = 0; i < 100; ++i)
        points.push_back({static_cast<float>(i * 10), 2, 0, 1, 0});
    OceanSplineRaster ribbon;
    ribbon.SetTriangles(BuildOceanRibbon(points, style));
    float depth = 0;
    ASSERT_TRUE(ribbon.SampleDepth(5, 1.5f, depth));
    EXPECT_FLOAT_EQ(depth, (kDepthBandSaturationMeters + 4.0f) * 0.5f);
    std::vector<OceanRibbonTriangleGPU> tree;
    ribbon.CopyUnderwaterTree(tree);
    ASSERT_FALSE(tree.empty());
    EXPECT_EQ(tree[0].Meta[1], tree.size());
    EXPECT_FLOAT_EQ(tree[0].A[1], -8);
    EXPECT_FLOAT_EQ(tree[0].B[1], 2);
    size_t leaves = 0;
    for (size_t i = 0; i < tree.size(); ++i)
    {
        if (tree[i].Meta[0] & 0x80000000u)
        {
            EXPECT_GT(tree[i].Meta[1], i);
            EXPECT_LE(tree[i].Meta[1], tree.size());
        }
        else
            ++leaves;
    }
    EXPECT_EQ(leaves, 198u);
    ribbon.RebaseOrigin(5, 10, 2);
    ribbon.CopyUnderwaterTree(tree);
    EXPECT_FLOAT_EQ(tree[0].A[1], -10);
    EXPECT_FLOAT_EQ(tree[0].B[1], 0);
    EXPECT_TRUE(ribbon.UnderwaterDepth(0, -2, -10, depth));
    EXPECT_FLOAT_EQ(depth, 2);
}

// A water-body preset overrides surface values for its material only, and its
// texture references validate: a bad reference keeps the last good one, an empty
// one clears it.
TEST(OceanQueryTests, WaterMaterialPresetsApplyValuesAndValidateTextureReferences)
{
    Components::OceanSurface surface{};
    surface.Roughness = 0.12f;
    OceanPresetAsset preset;
    preset.Set("Surface.Roughness", 0.7);
    preset.Set("Surface.FoamBubbleCoverage", 0.0);
    const auto type = ECS::GetComponentTypeId<Components::OceanSurface>();
    EXPECT_EQ(preset.Apply("Surface", type, &surface, sizeof(surface), {}), 2u);
    auto material = BuildOceanWaterMaterial(surface);
    EXPECT_FLOAT_EQ(material.Surface[2], 0.7f);
    EXPECT_FLOAT_EQ(material.Bubbles[0], 0);
    EXPECT_FLOAT_EQ(material.FoamDetail[1], surface.FoamScale);
    EXPECT_EQ(material.FoamTexture, ~0u);

    const GUID texture("2a119632-5328-4bfb-b78c-a27b287dbf68");
    OceanPresetAsset textured;
    textured.Set("Surface.FoamTexture", texture.ToString());
    EXPECT_EQ(textured.Apply("Surface", type, &surface, sizeof(surface), {}), 1u);
    EXPECT_EQ(surface.FoamTexture.ToGuid(), texture);
    textured.Set("Surface.FoamTexture", std::string("invalid"));
    EXPECT_EQ(textured.Apply("Surface", type, &surface, sizeof(surface), {}), 0u);
    EXPECT_EQ(surface.FoamTexture.ToGuid(), texture);
    textured.Set("Surface.FoamTexture", std::string());
    EXPECT_EQ(textured.Apply("Surface", type, &surface, sizeof(surface), {}), 1u);
    EXPECT_TRUE(surface.FoamTexture.IsNull());
}

// The shallow clarity window was literals in ocean_surface.glsl (8 m of path,
// floor 0.22). Its defaults on the component and on the GPU block must stay
// those values so every existing scene renders as before.
TEST(OceanQueryTests, ShallowClarityWindowDefaultsKeepTheFormerShaderConstants)
{
    const Components::OceanSurface surface{};
    EXPECT_FLOAT_EQ(surface.ShallowClarityDistance, 8.0f);
    EXPECT_FLOAT_EQ(surface.ShallowClarityFloor, 0.22f);
    const OceanParamsGPU blockDefaults{};
    EXPECT_FLOAT_EQ(blockDefaults.ShallowClarityDistance, 8.0f);
    EXPECT_FLOAT_EQ(blockDefaults.ShallowClarityFloor, 0.22f);

    OceanParamsGPU params{};
    params.ShallowClarityDistance = -1.0f;
    params.ShallowClarityFloor = -1.0f;
    PackShallowClarityWindow(surface, params);
    EXPECT_EQ(params.ShallowClarityDistance, 8.0f);
    EXPECT_EQ(params.ShallowClarityFloor, 0.22f);
}

// The extraction passes authored values through and clamps the ones the shader
// cannot evaluate: a zero distance (an undefined smoothstep) and a floor
// outside [0, 1].
TEST(OceanQueryTests, ShallowClarityWindowPacksAuthoredValuesAndClampsUnusableOnes)
{
    Components::OceanSurface surface{};
    OceanParamsGPU params{};
    surface.ShallowClarityDistance = 2.0f;
    surface.ShallowClarityFloor = 0.05f;
    PackShallowClarityWindow(surface, params);
    EXPECT_FLOAT_EQ(params.ShallowClarityDistance, 2.0f);
    EXPECT_FLOAT_EQ(params.ShallowClarityFloor, 0.05f);

    surface.ShallowClarityDistance = 0.0f;
    surface.ShallowClarityFloor = 1.5f;
    PackShallowClarityWindow(surface, params);
    EXPECT_GT(params.ShallowClarityDistance, 0.0f);
    EXPECT_FLOAT_EQ(params.ShallowClarityFloor, 1.0f);

    surface.ShallowClarityDistance = -3.0f;
    surface.ShallowClarityFloor = -0.5f;
    PackShallowClarityWindow(surface, params);
    EXPECT_GT(params.ShallowClarityDistance, 0.0f);
    EXPECT_FLOAT_EQ(params.ShallowClarityFloor, 0.0f);
}

// Both fields are reflected (so scenes and presets carry them) with inspector
// slider bounds that hold their defaults.
TEST(OceanQueryTests, ShallowClarityWindowFieldsAreReflectedWithSliderRanges)
{
    RegisterOceanFieldRanges();
    const auto type = ECS::GetComponentTypeId<Components::OceanSurface>();
    struct Expected
    {
        const char* Field;
        float Min;
        float Max;
        float Default;
    };
    for (const Expected e : {Expected{"ShallowClarityDistance", 0.01f, 50.0f, 8.0f},
                             Expected{"ShallowClarityFloor", 0.0f, 1.0f, 0.22f}})
    {
        SCOPED_TRACE(e.Field);
        const ECS::FieldInfo* field = ECS::ComponentFieldRegistry::FindField(type, e.Field);
        ASSERT_NE(field, nullptr);
        EXPECT_EQ(field->Size, sizeof(float32));
        EXPECT_TRUE(field->HasRange);
        EXPECT_FLOAT_EQ(field->MinValue, e.Min);
        EXPECT_FLOAT_EQ(field->MaxValue, e.Max);
        EXPECT_GE(e.Default, field->MinValue);
        EXPECT_LE(e.Default, field->MaxValue);
    }

    Components::OceanSurface surface{};
    OceanPresetAsset preset;
    preset.Set("Surface.ShallowClarityDistance", 2.5);
    preset.Set("Surface.ShallowClarityFloor", 0.1);
    EXPECT_EQ(preset.Apply("Surface", type, &surface, sizeof(surface), {}), 2u);
    EXPECT_FLOAT_EQ(surface.ShallowClarityDistance, 2.5f);
    EXPECT_FLOAT_EQ(surface.ShallowClarityFloor, 0.1f);
}

namespace
{
// CPU mirror of the surface's depth fog over a seabed `path` meters of view ray below the surface,
// seen from above, at the default exponential falloff (start 0, power 1): the fraction of the
// seabed's colour left in `channel`, 1 - alpha * OceanShallowFogScale.
float32 SeabedVisibleFraction(const OceanParamsGPU& params, uint32 channel, float32 path)
{
    const float32 t = std::clamp(path / params.ShallowClarityDistance, 0.0f, 1.0f);
    const float32 ramp = t * t * (3.0f - 2.0f * t);
    const float32 scale = params.ShallowClarityFloor + (1.0f - params.ShallowClarityFloor) * ramp;
    const float32 alpha = 1.0f - std::exp(-params.DepthFogDensity[channel] * path);
    return 1.0f - alpha * scale;
}
} // namespace

// The CBT terrain classifier refines seabed deeper than OceanSeabedVisibleDepthM 8 times coarser,
// as hidden ground. With the shallow clarity window authorable, a seabed at that depth must still
// read as deep water: at most kOceanSeabedVisibleTransmittance of its colour in every channel, on a
// vertical view ray (the shortest path to it) and on longer ones.
TEST(OceanQueryTests, SeabedVisibleDepthHonoursTheShallowClarityWindow)
{
    struct Case
    {
        const char* Name;
        float32 Density[3];
        float32 Distance;
        float32 Floor;
    };
    const Case cases[] = {
        {"tuned density, default window", {1.6f, 0.9f, 0.6f}, 8.0f, 0.22f},
        {"tuned density, 20 m / 0", {1.6f, 0.9f, 0.6f}, 20.0f, 0.0f},
        {"tuned density, 50 m / 0", {1.6f, 0.9f, 0.6f}, 50.0f, 0.0f},
        {"tuned density, 50 m / 0.22", {1.6f, 0.9f, 0.6f}, 50.0f, 0.22f},
        {"tuned density, 2 m / 0", {1.6f, 0.9f, 0.6f}, 2.0f, 0.0f},
        {"component density, default window", {0.9f, 0.3f, 0.35f}, 8.0f, 0.22f},
        {"component density, 50 m / 0", {0.9f, 0.3f, 0.35f}, 50.0f, 0.0f},
    };
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.Name);
        OceanParamsGPU params{};
        for (uint32 channel = 0; channel < 3; ++channel)
            params.DepthFogDensity[channel] = c.Density[channel];
        params.ShallowClarityDistance = c.Distance;
        params.ShallowClarityFloor = c.Floor;
        const float32 depth = OceanSeabedVisibleDepthM(params);
        ASSERT_LT(depth, kOceanSeabedVisibleDepthMaxM);
        for (const float32 pathOverDepth : {1.0f, 1.3f, 2.0f, 4.0f})
            for (uint32 channel = 0; channel < 3; ++channel)
                EXPECT_LE(SeabedVisibleFraction(params, channel, depth * pathOverDepth),
                          kOceanSeabedVisibleTransmittance + 1e-5f)
                    << "depth " << depth << " m, path " << depth * pathOverDepth << " m, channel " << channel;
    }
}

// The window adds no margin when it is off (floor 1) or ends before the density alone has hidden
// the seabed, so the classifier keeps coarsening exactly as much hidden seabed as before; an
// authored fog end distance still bounds it.
TEST(OceanQueryTests, SeabedVisibleDepthGainsNothingFromAWindowThatIsOffOrShort)
{
    OceanParamsGPU params{};
    params.DepthFogDensity[0] = 1.6f;
    params.DepthFogDensity[1] = 0.9f;
    params.DepthFogDensity[2] = 0.6f;
    const float32 byDensity = -std::log(kOceanSeabedVisibleTransmittance) / 0.6f;

    params.ShallowClarityDistance = 50.0f;
    params.ShallowClarityFloor = 1.0f;
    EXPECT_NEAR(OceanSeabedVisibleDepthM(params), byDensity, 1e-4f);

    params.ShallowClarityDistance = 2.0f;
    params.ShallowClarityFloor = 0.0f;
    EXPECT_NEAR(OceanSeabedVisibleDepthM(params), byDensity, 1e-4f);

    params.DepthFogEndDistance = 30.0f;
    EXPECT_NEAR(OceanSeabedVisibleDepthM(params), 30.0f, 1e-4f);
}

// A non-finite authored value (a scene or preset edited by hand) would reach the shader's
// smoothstep as NaN; the extraction falls back to that field's default instead, and leaves the
// other field's authored value alone.
TEST(OceanQueryTests, ShallowClarityWindowFallsBackToTheDefaultsForNonFiniteValues)
{
    const Components::OceanSurface defaults{};
    for (const float32 bad : {std::numeric_limits<float32>::quiet_NaN(),
                              std::numeric_limits<float32>::infinity(),
                              -std::numeric_limits<float32>::infinity()})
    {
        SCOPED_TRACE(bad);
        Components::OceanSurface surface{};
        surface.ShallowClarityDistance = bad;
        surface.ShallowClarityFloor = bad;
        OceanParamsGPU params{};
        params.ShallowClarityDistance = -1.0f;
        params.ShallowClarityFloor = -1.0f;
        PackShallowClarityWindow(surface, params);
        EXPECT_EQ(params.ShallowClarityDistance, defaults.ShallowClarityDistance);
        EXPECT_EQ(params.ShallowClarityFloor, defaults.ShallowClarityFloor);

        surface.ShallowClarityDistance = 3.0f;
        surface.ShallowClarityFloor = bad;
        PackShallowClarityWindow(surface, params);
        EXPECT_EQ(params.ShallowClarityDistance, 3.0f);
        EXPECT_EQ(params.ShallowClarityFloor, defaults.ShallowClarityFloor);

        surface.ShallowClarityDistance = bad;
        surface.ShallowClarityFloor = 0.05f;
        PackShallowClarityWindow(surface, params);
        EXPECT_EQ(params.ShallowClarityDistance, defaults.ShallowClarityDistance);
        EXPECT_EQ(params.ShallowClarityFloor, 0.05f);
    }
}

// The extraction is what carries an authored window to the GPU block: after an Update the render
// feature holds the surface's window, clamped as PackShallowClarityWindow clamps it.
TEST(OceanQueryTests, ExtractionCarriesTheAuthoredShallowClarityWindowToTheRenderFeature)
{
    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::OceanSurface surface{};
    surface.ShallowClarityDistance = 20.0f;
    surface.ShallowClarityFloor = 0.05f;
    world.AddComponentImmediate(entity, surface);

    Engine::Renderer::RenderServices rs;
    OceanExtractionSystem system(&rs);
    system.Update(world, 1.0f / 60.0f);
    const auto* feature = rs.GetFeature<OceanRenderFeature>();
    ASSERT_NE(feature, nullptr);
    ASSERT_TRUE(feature->HasOcean());
    EXPECT_EQ(feature->GetParams().ShallowClarityDistance, 20.0f);
    EXPECT_EQ(feature->GetParams().ShallowClarityFloor, 0.05f);

    auto* authored = world.GetComponentForWrite<Components::OceanSurface>(entity);
    ASSERT_NE(authored, nullptr);
    authored->ShallowClarityDistance = 0.0f;
    authored->ShallowClarityFloor = 1.5f;
    system.Update(world, 1.0f / 60.0f);
    EXPECT_GT(feature->GetParams().ShallowClarityDistance, 0.0f);
    EXPECT_EQ(feature->GetParams().ShallowClarityFloor, 1.0f);
}

TEST(OceanQueryTests, OceanShadowSettingsRejectInvalidHistoryAndJitter)
{
    OceanSettingsAsset settings;
    settings.Kind = OceanSettingsKind::Shadows;
    settings.Shadows.Enabled = true;
    EXPECT_TRUE(settings.Validate());
    settings.Shadows.TemporalWeight = 1;
    EXPECT_FALSE(settings.Validate());
    settings.Shadows.TemporalWeight = 0.92f;
    settings.Shadows.HardJitterDiameter = -1;
    EXPECT_FALSE(settings.Validate());
    settings.Shadows.HardJitterDiameter = 0.6f;
    settings.Shadows.SoftChannelScale = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(settings.Validate());
}

TEST(OceanQueryTests, OceanSetupValidationReportsAndFixesUnsafeRanges)
{
    Components::OceanRenderer renderer{};
    Components::OceanSurface surface{};
    renderer.QualityOverride = 99u;
    renderer.LodCount = 0u;
    renderer.GeometryDownSampleFactor = 0u;
    renderer.MaxCollisionQueryCount = 0u;
    surface.IorWater = 0.0f;
    surface.DepthFogStartDistance = 5.0f;
    surface.DepthFogEndDistance = 2.0f;
    OceanInputFrameStats stats{};
    stats.DroppedFoamInputs = 2u;

    const auto before = OceanSetupValidator::Validate(renderer, surface, nullptr, nullptr,
                                                       nullptr, nullptr, &stats);
    EXPECT_GE(before.size(), 6u);
    EXPECT_GT(OceanSetupValidator::ApplyAutomaticFixes(renderer, surface), 0u);
    EXPECT_EQ(renderer.QualityOverride, 2u);
    EXPECT_EQ(renderer.LodCount, 1u);
    EXPECT_EQ(renderer.GeometryDownSampleFactor, 1u);
    EXPECT_EQ(renderer.MaxCollisionQueryCount, 1u);
    EXPECT_FLOAT_EQ(surface.IorWater, 1.333f);
    EXPECT_FLOAT_EQ(surface.DepthFogEndDistance, 5.0f);

    stats.DroppedFoamInputs = 0u;
    const auto after = OceanSetupValidator::Validate(renderer, surface, nullptr, nullptr,
                                                      nullptr, nullptr, &stats);
    EXPECT_TRUE(after.empty());
}

} // namespace
} // namespace GameEngine::Ocean
