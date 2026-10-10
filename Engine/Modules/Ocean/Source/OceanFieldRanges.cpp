#include "Ocean/OceanFieldRanges.h"

#include "Components/ComponentRegistration.h"
#include "Components/Rendering/Ocean.h"

// Inspector slider bounds for the ocean components' float fields. Centralised here
// (rather than the headers) so the component structs stay plain data and the
// ranges are applied once at init via SetReflectedFieldRange. uint/bool fields use
// other inspector widgets and are intentionally omitted.
#define GE_OCEAN_RANGE(Type, Field, Lo, Hi)                                                        \
    ::GameEngine::Components::SetReflectedFieldRange<::GameEngine::Components::Type>(#Field, (Lo),  \
                                                                                    (Hi))

namespace GameEngine::Ocean
{

void RegisterOceanFieldRanges()
{
    using namespace ::GameEngine::Components;

    // --- OceanSurface ---
    SetReflectedFieldFlags<OceanSurface>("SeaLevel", ::GameEngine::ECS::FieldFlags::Hidden);
    SetReflectedFieldFlags<OceanSurface>("WaveMode", ::GameEngine::ECS::FieldFlags::Hidden);
    GE_OCEAN_RANGE(OceanSurface, ChoppyScale, 0.0f, 2.0f);
    GE_OCEAN_RANGE(OceanSurface, FresnelPower, 1.0f, 20.0f);
    GE_OCEAN_RANGE(OceanSurface, ReflectionStrength, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanSurface, SubsurfaceStrength, 0.0f, 4.0f);
    GE_OCEAN_RANGE(OceanSurface, FoamAmount, 0.0f, 5.0f);
    GE_OCEAN_RANGE(OceanSurface, FoamFadeRate, 0.0f, 4.0f);
    GE_OCEAN_RANGE(OceanSurface, WaveFoamStrength, 0.0f, 5.0f);
    GE_OCEAN_RANGE(OceanSurface, WaveFoamCoverage, 0.0f, 2.0f);
    GE_OCEAN_RANGE(OceanSurface, FoamScale, 0.01f, 50.0f);
    GE_OCEAN_RANGE(OceanSurface, FoamFeather, 0.001f, 1.0f);
    GE_OCEAN_RANGE(OceanSurface, FoamNormalStrength, 0.0f, 2.0f);
    GE_OCEAN_RANGE(OceanSurface, FoamBubbleCoverage, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanSurface, FoamBubbleParallax, 0.0f, 0.5f);
    GE_OCEAN_RANGE(OceanSurface, FoamRoughness, 0.04f, 1.0f);
    GE_OCEAN_RANGE(OceanSurface, SprayWindThreshold, 0.0f, 40.0f);
    GE_OCEAN_RANGE(OceanSurface, SprayMaxParticles, 256u, 262144u);
    GE_OCEAN_RANGE(OceanSurface, SpraySpawnRate, 0.0f, 1000.0f);
    GE_OCEAN_RANGE(OceanSurface, SpraySpawnRadius, 1.0f, 500.0f);
    GE_OCEAN_RANGE(OceanSurface, SprayEmissionThreshold, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanSplineInput, MaxSegmentLength, 0.1f, 100.0f);
    GE_OCEAN_RANGE(OceanSurface, SprayLifetime, 0.05f, 5.0f);
    GE_OCEAN_RANGE(OceanSurface, SprayStartSize, 0.01f, 5.0f);
    GE_OCEAN_RANGE(OceanSurface, SprayEndSize, 0.01f, 8.0f);
    GE_OCEAN_RANGE(OceanSurface, SprayUpVelocity, 0.0f, 10.0f);
    GE_OCEAN_RANGE(OceanSurface, SprayWindVelocityScale, 0.0f, 2.0f);
    GE_OCEAN_RANGE(OceanSurface, SprayOpacity, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanSurface, SprayIntersectionSpawnRate, 0.0f, 100.0f);
    GE_OCEAN_RANGE(OceanSurface, SprayIntersectionBand, 0.05f, 10.0f);
    GE_OCEAN_RANGE(OceanSurface, ShorelineFoamMaxDepth, 0.01f, 5.0f);
    GE_OCEAN_RANGE(OceanSurface, ShorelineFoamStrength, 0.0f, 5.0f);
    GE_OCEAN_RANGE(OceanSurface, IntersectionFoamDepth, 0.01f, 10.0f);
    GE_OCEAN_RANGE(OceanSurface, IntersectionFoamStrength, 0.0f, 5.0f);
    GE_OCEAN_RANGE(OceanSurface, NormalsStrength, 0.0f, 2.0f);
    GE_OCEAN_RANGE(OceanSurface, NormalsScale, 0.01f, 200.0f);
    GE_OCEAN_RANGE(OceanSurface, SubSurfaceDepthMax, 0.01f, 50.0f);
    GE_OCEAN_RANGE(OceanSurface, SubSurfaceDepthPower, 0.01f, 10.0f);
    GE_OCEAN_RANGE(OceanSurface, SubSurfaceBase, 0.0f, 4.0f);
    GE_OCEAN_RANGE(OceanSurface, SubSurfaceSun, 0.0f, 10.0f);
    GE_OCEAN_RANGE(OceanSurface, SubSurfaceSunFallOff, 1.0f, 16.0f);
    GE_OCEAN_RANGE(OceanSurface, Specular, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanSurface, Roughness, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanSurface, IorAir, 1.0f, 2.0f);
    GE_OCEAN_RANGE(OceanSurface, IorWater, 1.0f, 2.0f);
    GE_OCEAN_RANGE(OceanSurface, PlanarReflectionStrength, 0.0f, 3.0f);
    GE_OCEAN_RANGE(OceanSurface, SkyDirectionality, 0.0f, 0.99f);
    GE_OCEAN_RANGE(OceanSurface, DirectionalLightBoost, 0.0f, 512.0f);
    GE_OCEAN_RANGE(OceanSurface, DirectionalLightFallOff, 1.0f, 4096.0f);
    GE_OCEAN_RANGE(OceanSurface, DepthFogStartDistance, 0.0f, 250.0f);
    GE_OCEAN_RANGE(OceanSurface, DepthFogEndDistance, 0.0f, 1000.0f);
    GE_OCEAN_RANGE(OceanSurface, DepthFogFalloffPower, 0.1f, 8.0f);
    GE_OCEAN_RANGE(OceanSurface, RefractionStrength, 0.0f, 2.0f);
    GE_OCEAN_RANGE(OceanSurface, ShallowRefractionReflectionSuppression, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanSurface, ShallowClarityDistance, 0.01f, 50.0f);
    GE_OCEAN_RANGE(OceanSurface, ShallowClarityFloor, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanSurface, CausticsScale, 0.0f, 25.0f);
    GE_OCEAN_RANGE(OceanSurface, CausticsAverage, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanSurface, CausticsStrength, 0.0f, 10.0f);
    GE_OCEAN_RANGE(OceanSurface, CausticsFocalDepth, 0.0f, 250.0f);
    GE_OCEAN_RANGE(OceanSurface, CausticsDepthOfField, 0.01f, 1000.0f);
    GE_OCEAN_RANGE(OceanSurface, CausticsDistortionStrength, 0.0f, 0.25f);
    GE_OCEAN_RANGE(OceanSurface, CausticsDistortionScale, 0.01f, 50.0f);
    GE_OCEAN_RANGE(OceanSurface, ReflectedCausticsStrength, 0.0f, 4.0f);
    GE_OCEAN_RANGE(OceanSurface, ReflectedCausticsHeight, 0.0f, 25.0f);
    GE_OCEAN_RANGE(OceanSurface, ReflectedCausticsFalloff, 0.01f, 8.0f);
    GE_OCEAN_RANGE(OceanSurface, MeniscusWidth, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanSurface, WaterlineFadeDistance, 0.005f, 0.5f);
    GE_OCEAN_RANGE(OceanSurface, DefaultClippingState, 0.0f, 1.0f);

    // --- OceanWaveSpectrum ---
    GE_OCEAN_RANGE(OceanWaveSpectrum, WindSpeed, 0.0f, 40.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, WindDirectionDegrees, -180.0f, 180.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, Turbulence, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, Multiplier, 0.0f, 10.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, Chop, 0.0f, 2.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, GravityScale, 0.0f, 25.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, LoopPeriod, 0.0f, 128.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, DirectionalSpread, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, AmplitudeScale, 0.0f, 4.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, MaxWavelength, 1.0f, 1000.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, Weight, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, MaxHorizontalDisplacement, 0.0f, 50.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, MaxVerticalDisplacement, 0.0f, 30.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, RespectShallowWaterAttenuation, 0.0f, 1.0f);
    // Per-octave FFT spectrum arrays render as sliders (one per wavelength band),
    // matching the reference's spectrum editor. The range applies to every element.
    GE_OCEAN_RANGE(OceanWaveSpectrum, SpectrumPower, 0.0f, 2.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, ChopScales, 0.0f, 4.0f);
    GE_OCEAN_RANGE(OceanWaveSpectrum, GravityScales, 0.0f, 4.0f);

    // --- OceanRenderer (float knobs only) ---
    GE_OCEAN_RANGE(OceanRenderer, MinScale, 1.0f, 256.0f);
    GE_OCEAN_RANGE(OceanRenderer, MaxScale, 1.0f, 4096.0f);
    GE_OCEAN_RANGE(OceanRenderer, GravityMultiplier, 0.0f, 10.0f);
    GE_OCEAN_RANGE(OceanRenderer, RasterDepthCaptureSizeX, 1.0f, 10000.0f);
    GE_OCEAN_RANGE(OceanRenderer, RasterDepthCaptureSizeZ, 1.0f, 10000.0f);
    GE_OCEAN_RANGE(OceanRenderer, RasterDepthCaptureTopPadding, 0.01f, 1000.0f);
    GE_OCEAN_RANGE(OceanRenderer, RasterDepthCaptureDeepWaterDepth, 1.0f, 100000.0f);
    GE_OCEAN_RANGE(OceanRenderer, TimeScale, 0.0f, 4.0f);
    GE_OCEAN_RANGE(OceanRenderer, TimeOffset, -100000.0f, 100000.0f);
    GE_OCEAN_RANGE(OceanRenderer, FixedTime, -100000.0f, 100000.0f);
    GE_OCEAN_RANGE(OceanRenderer, GlobalWindSpeed, 0.0f, 40.0f);
    GE_OCEAN_RANGE(OceanRenderer, GlobalWindDirection, -180.0f, 180.0f);
    GE_OCEAN_RANGE(OceanRenderer, GlobalWindTurbulence, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanWaterBody, ExtentX, 0.0f, 2000.0f);
    GE_OCEAN_RANGE(OceanWaterBody, ExtentZ, 0.0f, 2000.0f);
    GE_OCEAN_RANGE(OceanWaterBody, ClipFeather, 0.0f, 200.0f);
    GE_OCEAN_RANGE(OceanWaterBody, UnderwaterDepth, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanWaterBody, FlowX, -20.0f, 20.0f);
    GE_OCEAN_RANGE(OceanWaterBody, FlowZ, -20.0f, 20.0f);
    GE_OCEAN_RANGE(OceanWaterBody, WaveWeight, 0.0f, 2.0f);
    GE_OCEAN_RANGE(OceanWaterBody, WaveChop, 0.0f, 2.0f);
    GE_OCEAN_RANGE(OceanWaterBody, WaveFeather, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanWaterBody, LocalWaveAmplitude, 0.0f, 10.0f);
    GE_OCEAN_RANGE(OceanWaterBody, LocalWaveWavelength, 0.1f, 200.0f);
    GE_OCEAN_RANGE(OceanWaterBody, LocalWaveDirectionDegrees, -360.0f, 360.0f);
    GE_OCEAN_RANGE(OceanWaterBody, LocalWaveExtraAmplitude, 0.0f, 10.0f);
    GE_OCEAN_RANGE(OceanWaterBody, LocalWaveExtraWavelength, 0.1f, 200.0f);
    GE_OCEAN_RANGE(OceanWaterBody, LocalWaveExtraDirectionDegrees, -360.0f, 360.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, PointX, -2000.0f, 2000.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, PointZ, -2000.0f, 2000.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, ClipFeather, 0.0f, 200.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, UnderwaterDepth, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, FlowX, -20.0f, 20.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, FlowZ, -20.0f, 20.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, WaveWeight, 0.0f, 2.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, WaveChop, 0.0f, 2.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, WaveFeather, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, LocalWaveAmplitude, 0.0f, 10.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, LocalWaveWavelength, 0.1f, 200.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, LocalWaveDirectionDegrees, -360.0f, 360.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, LocalWaveExtraAmplitude, 0.0f, 10.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, LocalWaveExtraWavelength, 0.1f, 200.0f);
    GE_OCEAN_RANGE(OceanPolygonWaterBody, LocalWaveExtraDirectionDegrees, -360.0f, 360.0f);

    // --- Source markers ---
    GE_OCEAN_RANGE(OceanClipSource, ExtentX, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanClipSource, ExtentZ, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanClipSource, ClipState, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanClipSource, Feather, 0.0f, 200.0f);
    GE_OCEAN_RANGE(OceanAlbedoSource, ExtentX, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanAlbedoSource, ExtentZ, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanAlbedoSource, Coverage, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanFlowSource, ExtentX, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanFlowSource, ExtentZ, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanFlowSource, FlowX, -20.0f, 20.0f);
    GE_OCEAN_RANGE(OceanFlowSource, FlowZ, -20.0f, 20.0f);
    GE_OCEAN_RANGE(OceanFlowMapSource, ExtentX, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanFlowMapSource, ExtentZ, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanFlowMapSource, Strength, 0.0f, 50.0f);
    GE_OCEAN_RANGE(OceanFlowMapSource, Feather, 0.0f, 200.0f);
    GE_OCEAN_RANGE(OceanFlowMapSource, BiasX, -20.0f, 20.0f);
    GE_OCEAN_RANGE(OceanFlowMapSource, BiasZ, -20.0f, 20.0f);
    GE_OCEAN_RANGE(OceanWaveMaskTextureSource, ExtentX, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanWaveMaskTextureSource, ExtentZ, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanWaveMaskTextureSource, WeightScale, 0.0f, 4.0f);
    GE_OCEAN_RANGE(OceanWaveMaskTextureSource, ChopScale, 0.0f, 4.0f);
    GE_OCEAN_RANGE(OceanWaveMaskTextureSource, WeightBias, -2.0f, 2.0f);
    GE_OCEAN_RANGE(OceanWaveMaskTextureSource, ChopBias, -2.0f, 2.0f);
    GE_OCEAN_RANGE(OceanWaveMaskTextureSource, Coverage, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanWaveMaskTextureSource, Feather, 0.0f, 200.0f);
    GE_OCEAN_RANGE(OceanUnderwaterVolume, ExtentX, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanUnderwaterVolume, ExtentY, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanUnderwaterVolume, ExtentZ, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanUnderwaterExclusionVolume, ExtentX, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanUnderwaterExclusionVolume, ExtentY, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanUnderwaterExclusionVolume, ExtentZ, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanUnderwaterPortalOccluder, ExtentX, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanUnderwaterPortalOccluder, ExtentY, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanUnderwaterPortalOccluder, ExtentZ, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanSplineInput, Width, 0.1f, 500.0f);
    GE_OCEAN_RANGE(OceanSplineInput, UnderwaterDepth, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanSplineInput, ClipFeather, 0.0f, 200.0f);
    GE_OCEAN_RANGE(OceanSplineInput, FlowSpeed, -50.0f, 50.0f);
    GE_OCEAN_RANGE(OceanSplineInput, AlbedoCoverage, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanSplineInput, DepthMeters, 0.0f, 100.0f);
    GE_OCEAN_RANGE(OceanSplineInput, DepthFeather, 0.0f, 50.0f);
    GE_OCEAN_RANGE(OceanWaveImpulse, Radius, 0.1f, 50.0f);
    GE_OCEAN_RANGE(OceanWaveImpulse, Amplitude, -5.0f, 5.0f);
    GE_OCEAN_RANGE(OceanWaterInteraction, Radius, 0.1f, 50.0f);
    GE_OCEAN_RANGE(OceanWaterInteraction, Strength, 0.0f, 5.0f);
    GE_OCEAN_RANGE(OceanWaterInteraction, MinSpeed, 0.0f, 50.0f);
    GE_OCEAN_RANGE(OceanWaterInteraction, VerticalStrength, 0.0f, 5.0f);
    GE_OCEAN_RANGE(OceanWaterInteraction, MinVerticalSpeed, 0.0f, 50.0f);
    GE_OCEAN_RANGE(OceanWaterInteraction, MaxAmplitude, 0.0f, 10.0f);
    GE_OCEAN_RANGE(OceanSeabed, ExtentX, 0.0f, 2000.0f);
    GE_OCEAN_RANGE(OceanSeabed, ExtentZ, 0.0f, 2000.0f);
    GE_OCEAN_RANGE(OceanSeabed, BaseHeight, -100.0f, 10.0f);
    GE_OCEAN_RANGE(OceanSeabed, SlopeX, -1.0f, 1.0f);
    GE_OCEAN_RANGE(OceanSeabed, SlopeZ, -1.0f, 1.0f);
    GE_OCEAN_RANGE(OceanDepthCacheSource, BakeOriginX, -100000.0f, 100000.0f);
    GE_OCEAN_RANGE(OceanDepthCacheSource, BakeOriginZ, -100000.0f, 100000.0f);
    GE_OCEAN_RANGE(OceanDepthCacheSource, BakeSizeX, 1.0f, 100000.0f);
    GE_OCEAN_RANGE(OceanDepthCacheSource, BakeSizeZ, 1.0f, 100000.0f);
    GE_OCEAN_RANGE(OceanDepthCacheSource, BakeSeaLevel, -10000.0f, 10000.0f);
    GE_OCEAN_RANGE(OceanDepthCacheSource, BakeDeepWaterDepth, 1.0f, 100000.0f);
    GE_OCEAN_RANGE(OceanDepthCacheSource, StreamRadius, 0.0f, 100000.0f);
    GE_OCEAN_RANGE(OceanDepthContributor, ExtentX, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanDepthContributor, ExtentZ, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanDepthContributor, Depth, 0.0f, 100.0f);
    GE_OCEAN_RANGE(OceanDepthContributor, Feather, 0.0f, 50.0f);
    GE_OCEAN_RANGE(OceanDepthContributor, Roundness, 0.0f, 1.0f);
    GE_OCEAN_RANGE(OceanMeshDepthContributor, ExtentPadding, 0.0f, 50.0f);
    GE_OCEAN_RANGE(OceanMeshDepthContributor, DepthBias, -10.0f, 10.0f);
    GE_OCEAN_RANGE(OceanMeshDepthContributor, MinDepth, 0.0f, 100.0f);
    GE_OCEAN_RANGE(OceanMeshDepthContributor, MaxDepth, 0.0f, 500.0f);
    GE_OCEAN_RANGE(OceanMeshDepthContributor, Feather, 0.0f, 50.0f);
    GE_OCEAN_RANGE(OceanMeshDepthContributor, Roundness, 0.0f, 1.0f);

    // --- OceanBuoyancy ---
    GE_OCEAN_RANGE(OceanBuoyancy, BuoyancyStrength, 0.0f, 100.0f);
    GE_OCEAN_RANGE(OceanBuoyancy, ProbeRadius, 0.05f, 10.0f);
    GE_OCEAN_RANGE(OceanBuoyancy, Draft, 0.0f, 5.0f);
    GE_OCEAN_RANGE(OceanBuoyancy, WaterLineOffset, -5.0f, 5.0f);
    GE_OCEAN_RANGE(OceanBuoyancy, LinearDrag, 0.0f, 10.0f);
    GE_OCEAN_RANGE(OceanBuoyancy, AngularDrag, 0.0f, 10.0f);
}

} // namespace GameEngine::Ocean

#undef GE_OCEAN_RANGE
