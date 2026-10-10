#pragma once

#include "Types/Types.h"

namespace GameEngine::Ocean
{

// Quality tier drives which optional simulations run. The FFT working
// resolution and cascade-array dimensions are compile-time (a shader #define and
// fixed-size persistent textures), so the tier does NOT resize them at runtime;
// it gates the expensive optional sims. Authored on the OceanRenderer component
// (QualityOverride) or left to the device default. See OceanTierAllows below for
// the exact per-tier gates.
//   Low    — FFT waves only; no foam, flow, or dynamic waves.
//   Medium — FFT spectrum + persistent foam; no flow or dynamic waves.
//   High   — FFT spectrum + foam + flow + dynamic waves (every sim).
// Clip / albedo / seabed-depth bakes are content-gated (only run when a tagged
// source exists), so they are allowed on every tier — the tier never forces them.
enum class OceanQuality : uint32
{
    Low = 0,
    Medium = 1,
    High = 2,
};

// Per-tier gates applied at extraction (ANDed onto the authored OceanSurface
// toggles). Keeping this as free functions next to the enum means the extraction
// system and any future device-default selector agree on what each tier permits.
inline constexpr bool OceanTierAllowsFoam(OceanQuality q) { return q != OceanQuality::Low; }
inline constexpr bool OceanTierAllowsFlow(OceanQuality q) { return q == OceanQuality::High; }
inline constexpr bool OceanTierAllowsDynWaves(OceanQuality q) { return q == OceanQuality::High; }

// Wave generation path. FFT is the authored/runtime path. OceanWaveMode
// (Gerstner = 0, FFT = 1) is declared in the OceanSurface component header
// (Components/Rendering/Ocean.h) for old scene compatibility.
// DepthFogFalloff follows the same pattern. The GPU params below carry both as
// raw scalars.

// Engine-wide cascade ceiling. Cascades are LOD slices of the displacement
// (and later foam/flow/depth/shadow) Texture2DArrays. Each cascade covers twice
// the world area of the previous one and snaps to the camera.
inline constexpr uint32 kMaxOceanCascades = 6;

// Maximum LOD layers in a camera-snapped sim cascade array (foam, and later
// flow/depth/clip/albedo). Each layer covers twice the world area of the
// previous one, centered on (snapped to) the camera. Must match the array
// dimension of OceanCascadeLayoutGPU below and the GLSL mirror.
inline constexpr uint32 kMaxOceanLodCascades = 7;

// Maximum legacy analytic waves kept in the SSBO tail for old data/local packet
// helpers. Runtime surface displacement is FFT-only.
inline constexpr uint32 kMaxGerstnerWaves = 16;

// FFT working resolution (per axis) and cascade count. Must match GE_FFT_SIZE /
// GE_FFT_CASCADES in the FFT shaders. 16 cascades of 0.5*2^c m patches cover
// ~6 cm ripples to ~16 km swell (the reference parity). v2 tiers these per platform.
inline constexpr uint32 kOceanFFTResolution = 256;
inline constexpr uint32 kOceanFFTCascades = 16;
inline constexpr uint32 kOceanFFTPasses = 8; // log2(kOceanFFTResolution)
inline constexpr uint32 kMaxOceanLocalFFTStreams = 8;
inline constexpr uint32 kOceanLocalFFTMaskChannels = 4;
inline constexpr uint32 kOceanLocalFFTMaskPages =
    (kMaxOceanLocalFFTStreams + kOceanLocalFFTMaskChannels - 1u) /
    kOceanLocalFFTMaskChannels;

// the reference OceanWaveSpectrum octave count (per-octave power curve length).
inline constexpr uint32 kOceanSpectrumOctaves = 14;

// Surface geometry budget retained for scene compatibility. The renderer maps
// this value to the per-tile density of its camera-following concentric LOD rings.
// Runtime authoring can raise it for more near-field geometry or lower it for
// performance; keep a hard cap because vertex and triangle counts grow with it.
inline constexpr uint32 kDefaultOceanGridSize = 256;
inline constexpr uint32 kMinOceanGridSize = 8;
inline constexpr uint32 kMaxOceanGridSize = 1024;
inline constexpr uint32 kDefaultOceanGeometryUpSampleFactor = 2;

inline constexpr uint32 ClampOceanGeometryGridSize(uint32 gridSize)
{
    return gridSize < kMinOceanGridSize
        ? kMinOceanGridSize
        : (gridSize > kMaxOceanGridSize ? kMaxOceanGridSize : gridSize);
}

inline constexpr uint32 ResolveOceanGeometryGridSize(uint32 upSample, uint32 downSample)
{
    const uint64 up = upSample == 0u
        ? static_cast<uint64>(kDefaultOceanGeometryUpSampleFactor)
        : static_cast<uint64>(upSample);
    const uint64 down = downSample == 0u ? 1u : static_cast<uint64>(downSample);
    const uint64 desired =
        (static_cast<uint64>(kDefaultOceanGridSize) * up + down - 1u) / down;
    return ClampOceanGeometryGridSize(static_cast<uint32>(
        desired > static_cast<uint64>(kMaxOceanGridSize)
            ? static_cast<uint64>(kMaxOceanGridSize)
            : desired));
}
inline constexpr uint32 kDefaultOceanGeometryGridSize =
    ResolveOceanGeometryGridSize(0u, 1u);

// Base world-space extent used to size the concentric ring mesh. The outer
// horizon skirt extends beyond this reach and the whole layout scales with
// viewer altitude.
inline constexpr float32 kDefaultPatchExtent = 4096.0f;

// One legacy analytic wave. DirectionX/Z is a unit vector in the XZ plane.
// Layout matches GerstnerWaveGPU in ocean_common.glsl (std430).
struct GerstnerWave
{
    float32 DirectionX = 1.0f;
    float32 DirectionZ = 0.0f;
    float32 Amplitude = 0.0f;
    float32 Wavelength = 10.0f;
    float32 Steepness = 0.5f;
    float32 Speed = 1.0f;
    float32 _Pad0 = 0.0f;
    float32 _Pad1 = 0.0f;
};
static_assert(sizeof(GerstnerWave) == 32, "GerstnerWave must be std430-packed (8 floats)");

// CPU-side underwater and reflected-caustic effect settings, authored on OceanSurface
// and routed to the OceanUnderwater / reflected-caustics passes via the feature. Kept OFF
// the byte-exact OceanParamsGPU SSBO so adding effect knobs can't corrupt the
// surface layout. Defaults reproduce the built-in look.
struct OceanUnderwaterSettings
{
    bool Inscatter = true;
    float Inscatter_Strength = 1.0f;
    float Inscatter_PhaseG = 0.25f;
    bool Distortion = true;
    float Distortion_Strength = 1.0f;
    bool GodRays = true;
    float GodRay_Strength = 0.6f;
    float GodRay_Density = 32.0f;
    bool CausticsOnGeometry = true;
    float WaterlineFadeDistance = 0.05f;
    float ReflectedCaustics_Strength = 0.0f;
    float ReflectedCaustics_Height = 2.5f;
    float ReflectedCaustics_Falloff = 0.45f;
};

// Per-frame ocean parameters uploaded to the surface vertex/fragment shaders via
// a set-2 SSBO. Header fields first, then the legacy wave array. Kept std430.
struct OceanParamsGPU
{
    float32 Time = 0.0f;
    float32 SeaLevel = 0.0f;
    float32 PatchExtent = kDefaultPatchExtent;
    uint32 GerstnerWaveCount = 0;

    float32 _DeadShallowColor[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // dead: superseded by SubSurfaceShallowCol; kept for std430 layout
    float32 DeepColor[4] = {0.05f, 0.07f, 0.20f, 1.0f};    // deep-water diffuse base

    float32 ChoppyScale = 1.0f;
    float32 FresnelPower = 5.0f;
    uint32 WaveMode = 1; // OceanWaveMode::FFT; the extraction overwrites this
    uint32 FFTCascadeCount = 0; // active FFT cascades sampled by the surface (0 = none yet)

    float32 _DeadSkyColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // dead: superseded by SkyBase/SkyTowardsSun/SkyAwayFromSun; kept for std430 layout
    float32 FoamColor[4] = {1.0f, 0.996f, 0.972f, 1.0f};   // whitecap color (the reference _FoamWhiteColor)

    float32 ReflectionStrength = 1.0f;      // sky/planar reflection amount (scales the Fresnel blend)
    float32 SubsurfaceStrength = 1.0f;
    float32 FoamAmount = 1.0f;
    float32 _DeadSmoothness = 0.0f;         // dead: superseded by Roughness; kept for std430 layout

    // Main directional light, fed from the scene so the surface can do
    // sun-direction subsurface scattering (forward scatter through wave crests)
    // and sun glitter — the reference ocean's signature look. xyz of SunDirection
    // points TOWARD the sun; SunColor is light colour premultiplied by intensity.
    float32 SunDirection[4] = {0.32f, 0.7f, 0.22f, 0.0f};
    float32 SunColor[4] = {1.0f, 0.96f, 0.88f, 1.0f};

    // Foam controls (persistent foam simulation). FoamFadeRate is the per-second
    // exponential decay of accumulated foam; WaveFoamStrength scales how much
    // foam a breaking/pinching crest deposits; WaveFoamCoverage is the Jacobian
    // threshold below which a texel is "breaking" (lower = more foam); FoamScale
    // is the world tiling of the bubbly surface texture; FoamFeather softens the
    // coverage/threshold dissolve so whitecap edges break into bubbles.
    // Grouped as 8 floats (two vec4 lanes) so the std430 layout stays clean
    // ahead of the runtime-sized Waves[] tail. Mirror in ocean_common.glsl.
    float32 FoamFadeRate = 0.8f;
    float32 WaveFoamStrength = 1.0f;
    float32 WaveFoamCoverage = 0.55f;
    float32 FoamScale = 0.25f;
    float32 FoamFeather = 0.4f;
    float32 IntersectionFoamDepth = 1.0f;
    float32 IntersectionFoamStrength = 0.5f;
    float32 _FoamPad2 = 0.0f;

    // --- Material / shading parity (scattering + subsurface + IOR-Fresnel) ---
    // Mirror byte-for-byte in OceanParamsBuffer (ocean_common.glsl). All inserted
    // BEFORE the runtime-sized Waves[] tail. std430: every vec4 lane is 16-byte
    // aligned; trailing scalars are grouped four-to-a-lane (padded where odd).

    // Diffuse scattering: view-weighted blend of two tints (grazing vs. up-look)
    // plus an in-shadow tint. ColorLinear -> vec4. Each on its own 16-byte lane.
    float32 Diffuse[4] = {0.0f, 0.0028f, 0.0033f, 1.0f};
    float32 DiffuseGrazing[4] = {0.0014f, 0.0021f, 0.0022f, 1.0f};
    float32 DiffuseShadow[4] = {0.0f, 0.0006f, 0.0009f, 1.0f};

    // Subsurface: the shallow-water tint (toward shore) and the scatter colour the
    // sun lobe drives through wave crests. ColorLinear -> vec4.
    float32 SubSurfaceShallowCol[4] = {0.42f, 0.75f, 0.69f, 1.0f};
    float32 SubSurfaceColour[4] = {0.08f, 0.46f, 0.40f, 1.0f};

    // Procedural sky dome: zenith/base, the warmer tint toward the sun, and the
    // tint away from it. Steered by SkyDirectionality below. ColorLinear -> vec4.
    float32 SkyBase[4] = {0.09f, 0.19f, 0.34f, 1.0f};
    float32 SkyTowardsSun[4] = {0.27f, 0.39f, 0.49f, 1.0f};
    float32 SkyAwayFromSun[4] = {0.06f, 0.13f, 0.26f, 1.0f};
    float32 DirectionalLightColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // Scalar lane 1: surface normal map strength + world tiling scale, and the
    // shallow-tint depth shaping (depth = 0 until the Phase 5 depth cascade).
    float32 NormalsStrength = 1.0f;
    float32 NormalsScale = 1.0f;
    float32 SubSurfaceDepthMax = 10.0f;
    float32 SubSurfaceDepthPower = 2.5f;

    // Scalar lane 2: subsurface lobe terms (ambient base, sun lobe gain, lobe
    // falloff exponent) + specular reflection intensity.
    float32 SubSurfaceBase = 0.0f;
    float32 SubSurfaceSun = 1.7f;
    float32 SubSurfaceSunFallOff = 5.0f;
    float32 Specular = 0.7f;

    // Scalar lane 3: microfacet roughness, the two IORs for the Schlick R0, the
    // planar-reflection toggle (0 = off), and planar capture blend strength.
    float32 Roughness = 0.0f;
    float32 IorAir = 1.0f;
    float32 IorWater = 1.333f;
    uint32 PlanarReflections = 0;

    // Scalar lane 4: sky directionality exponent, sun glitter boost/falloff, and
    // how strongly the planar capture replaces the procedural reflected sky.
    float32 SkyDirectionality = 1.0f;
    float32 DirectionalLightBoost = 7.0f;
    float32 DirectionalLightFallOff = 275.0f;
    float32 PlanarReflectionStrength = 1.0f;

    // --- Refraction / depth-fog transparency (Phase 4) ---
    // Lane A: DepthFogDensity (rgb) is the per-channel extinction of the grabbed
    // scene colour as the view ray travels through water to the seabed (higher =
    // colour absorbed sooner, so deep water reads as DeepColor); the w lane
    // carries RefractionStrength — how far the surface normal bends the
    // screen-space grab UV (0 = no distortion).
    float32 DepthFogDensity[3] = {0.9f, 0.3f, 0.35f};
    float32 RefractionStrength = 0.5f;

    // Lane B: RefractionAvailable is 1 when OceanRenderNode scheduled the scene-
    // colour grab AND bound it on this draw (0 = the grab declined; the surface
    // reads opaque). RefractionDepthMatched is 1 when the grab resolved MSAA
    // colour from the samples on the resolved depth surface; refraction then
    // filters within that surface. Both stamped by the contributor at emit, not
    // the extraction. The lane's other half is the shallow clarity window
    // (OceanSurface.ShallowClarityDistance / ShallowClarityFloor): the depth fog
    // is scaled from the floor up to full over the first ShallowClarityDistance
    // meters of view-ray path through the water. Mirror in ocean_common.glsl.
    uint32 RefractionAvailable = 0;
    uint32 RefractionDepthMatched = 0;
    float32 ShallowClarityDistance = 8.0f;
    float32 ShallowClarityFloor = 0.22f;

    // Lane C: depth-fog distance shaping. x = start distance (m), y = end
    // distance (m; 0 = auto), z = falloff power, w = OceanDepthFogFalloff as a
    // float (0 exponential, 1 linear, 2 smooth). Mirror in ocean_common.glsl.
    float32 DepthFogStartDistance = 0.0f;
    float32 DepthFogEndDistance = 0.0f;
    float32 DepthFogFalloffPower = 1.0f;
    float32 DepthFogFalloffMode = 0.0f;

    // --- Sea-floor depth cascade (Phase 5) ---
    // The seabed-depth cascade (rendered top-down per LOD into the shared
    // camera-snapped layout) feeds two effects: the shallow-water colour term
    // (Phase 2's `shallow` factor, previously gated to 0) and the shoreline foam
    // term (Phase 1's whitecaps near the waterline). One std430 lane:
    //   x = ShorelineFoamMaxDepth — depth (m) below which shoreline foam appears;
    //       at 0 m (the waterline) it is full, fading to none at this depth.
    //   y = ShorelineFoamStrength — how strongly the shoreline term adds foam.
    //   z = SeabedDepthAvailable — 1 when the depth cascade was written + bound
    //       this frame (0 = no tagged seabed / sim unavailable; both terms inert).
    //   w = pad.
    // The surface samples the depth cascade with its own layout in the
    // OceanCascadeLayout block (set 2 binding 4). Mirror in ocean_common.glsl.
    float32 ShorelineFoamMaxDepth = 0.65f;
    float32 ShorelineFoamStrength = 2.0f;
    uint32 SeabedDepthAvailable = 0;
    float32 _SeabedPad0 = 0.0f;

    // --- Underwater caustics (Phase 6) ---
    // The surface multiplies a procedural caustics web into the refracted scene
    // colour (Phase 4) to fake refracted-sunlight focusing on the seabed. Two
    // tiled scrolling samples of the caustics texture are taken, their UVs
    // distorted by a normal sample and offset along the sun direction; the mip
    // level is selected from the seabed-distance focal blur, and the result is
    // (A*B - Average)*Strength added to 1.0 as a colour multiplier. Two std430
    // 16-byte lanes (8 scalars) inserted BEFORE the runtime-sized Waves[] tail;
    // mirror byte-for-byte in ocean_common.glsl.
    //
    // Lane 1 (bytes 416-431):
    //   CausticsScale          — world tiling (m) of one caustics texture period.
    //   CausticsAverage        — the texture's mean intensity, subtracted so the
    //                            web brightens and darkens around neutral.
    //   CausticsStrength       — overall caustic contrast added to the scene.
    //   CausticsFocalDepth     — seabed distance (m) at which caustics are sharpest.
    float32 CausticsScale = 5.0f;
    float32 CausticsAverage = 0.07f;
    float32 CausticsStrength = 3.2f;
    float32 CausticsFocalDepth = 2.0f;

    // Lane 2 (bytes 432-447):
    //   CausticsDepthOfField   — how quickly caustics blur away from the focal
    //                            depth (smaller = tighter focus band).
    //   CausticsDistortionStrength — amount the normal sample bends the lookup UVs.
    //   CausticsDistortionScale    — world tiling (m) of the distortion sample.
    //   CausticsAvailable      — 1 when the caustics texture was generated AND
    //                            bound this frame (stamped by the contributor at
    //                            emit, like RefractionAvailable). 0 = surface skips
    //                            caustics. Only bound when refraction is active.
    float32 CausticsDepthOfField = 0.33f;
    float32 CausticsDistortionStrength = 0.16f;
    float32 CausticsDistortionScale = 25.0f;
    uint32 CausticsAvailable = 0;

    // --- Underwater rendering (Phase 7) ---
    // The submerged-camera look. The fullscreen ocean_underwater pass and the
    // surface back-face branch read these. One std430 16-byte lane (4 scalars)
    // inserted BEFORE the runtime-sized Waves[] tail; mirror byte-for-byte in
    // ocean_common.glsl.
    //   Underwater     — 1 when the underwater look is enabled AND the camera is
    //                    below the displaced surface this frame (CPU-gated by the
    //                    render node; the surface uses it to flip back-face
    //                    normals + add subsurface from below). 0 = above water.
    //   MeniscusWidth  — screen-height fraction of the bright waterline band the
    //                    fullscreen pass paints where the surface crosses the
    //                    screen. 0 = no band.
    //   two trailing pads fill the 16-byte lane.
    uint32 Underwater = 0;
    float32 MeniscusWidth = 0.05f;
    float32 _UnderwaterPad0 = 0.0f;
    float32 _UnderwaterPad1 = 0.0f;

    // --- Flow + dynamic waves (Phase 8 + 9) ---
    // One std430 16-byte lane (4 scalars) inserted BEFORE the runtime-sized
    // Waves[] tail; mirror byte-for-byte in ocean_common.glsl. The surface
    // samples the flow and dynamic-wave cascades each with its own layout in the
    // OceanCascadeLayout block (set 2 binding 4).
    //   FlowAvailable     — 1 when the flow cascade was baked + bound this frame
    //                       (the authored Flow toggle AND a flow cascade present).
    //                       Gates the detail-UV scroll (foam advection reads the
    //                       cascade in the sim, not here). 0 = static ripples.
    //   DynamicWavesAvailable — 1 when the dynamic-wave sim ran + its cascade was
    //                       bound this frame. Gates the vertex-stage dynamic-height
    //                       add + the surface-normal fold. 0 = spectrum waves only.
    //   DynWavesAmplitude — vertical scale applied to the sampled dynamic-wave
    //                       height before it is added to the surface displacement.
    //   FlowDetailScale   — multiplier on flow*time when scrolling the detail
    //                       normal UVs (visual strength of the current's drift).
    uint32 FlowAvailable = 0;
    uint32 DynamicWavesAvailable = 0;
    float32 DynWavesAmplitude = 1.0f;
    float32 FlowDetailScale = 1.0f;

    // Advanced dynamic-wave shape controls supplied by the optional typed
    // settings asset. Horizontal displacement pushes vertices opposite the
    // simulated height gradient; DisplacementClamp bounds both the simulation
    // state and the visible contribution. Zero horizontal displacement preserves
    // the legacy vertical-only behavior.
    float32 DynWavesHorizontalDisplacement = 0.0f;
    float32 DynWavesDisplacementClamp = 4.0f;
    float32 _DynWavesPad0 = 0.0f;
    float32 _DynWavesPad1 = 0.0f;

    // --- Clip surface + albedo cascades (Phase 10 + 11) ---
    // One std430 16-byte lane (4 scalars) inserted BEFORE the runtime-sized
    // Waves[] tail; mirror byte-for-byte in ocean_common.glsl. The surface samples
    // each cascade with its own layout in the OceanCascadeLayout block (set 2
    // binding 4).
    //   ClipAvailable   — 1 when the clip cascade was baked + bound this frame (the
    //                     authored ClipSurface toggle AND a clip source present).
    //                     The surface discards fragments where the sampled clip
    //                     value exceeds 0.5, cutting holes (harbors, hulls). 0 =
    //                     no clipping (the surface is solid everywhere).
    //   DefaultClippingState — the clip value the cascade is cleared to before the
    //                     sources composite (0 = surface solid by default, sources
    //                     punch holes; 1 = surface clipped by default, sources
    //                     carve the water back in). Read by the clip bake, mirrored
    //                     here so the surface gate matches.
    //   AlbedoAvailable — 1 when the albedo cascade was baked + bound this frame
    //                     (the authored Albedo toggle AND an albedo source present).
    //                     The surface blends baseColor toward the cascade's rgb by
    //                     its alpha BEFORE lighting + under foam (decals/paint). 0 =
    //                     no albedo override.
    //   NormalTextureAvailable — 1 when the linear detail normal texture is bound.
    uint32 ClipAvailable = 0;
    float32 DefaultClippingState = 0.0f;
    uint32 AlbedoAvailable = 0;
    uint32 NormalTextureAvailable = 0;

    // --- Texture overrides + planar reflection availability gates ---
    // One std430 16-byte lane (4 scalars) inserted BEFORE the runtime-sized Waves[]
    // tail; mirror byte-for-byte in ocean_common.glsl.
    //   FoamTextureAvailable      — 1 when a user foam-bubble texture is bound; the
    //                               surface samples it instead of the analytic Worley.
    //   CausticsTextureAvailable  — 1 when a user caustics texture is bound; the
    //                               caustics skip the encoded-normal UV distortion (a
    //                               plain image has no R,G distortion channel).
    //   PlanarReflectionAvailable — 1 when the planar-reflection RT was rendered +
    //                               bound this frame; the surface samples it instead
    //                               of the procedural sky dome. (Reserved here so
    //                               planar reflections need no second layout edit.)
    //   ShallowRefractionReflectionSuppression — 0 keeps reflections unchanged in
    //                               shallow refractive water; 1 applies the authored
    //                               suppression so caustics stay visible.
    uint32 FoamTextureAvailable = 0;
    uint32 CausticsTextureAvailable = 0;
    uint32 PlanarReflectionAvailable = 0;
    float32 ShallowRefractionReflectionSuppression = 1.0f;

    // --- Displacement combine cascade (OceanRenderer.CombineDisplacementCascade) ---
    // CombineWavesAvailable = 1 when the combine cascade was baked + bound this
    // frame (the toggle is on AND the cascade is ready), so OceanSampleDisplacement
    // samples ~2 combined slices instead of summing all 16 FFT cascades. 0 = the
    // direct 16-cascade path. One std430 16-byte lane; mirror byte-for-byte in
    // ocean_common.glsl.
    uint32 CombineWavesAvailable = 0;
    // Foam/depth debug overlay: 0 normal, 1 combined foam, 2 wave,
    // 3 shoreline/contact, 4 freshness/age, 5 latest deposit,
    // 6 raw seabed depth ramp, 7 normalized shallow mask, 8 invalid/deep tiles.
    uint32 FoamDebugMode = 0;
    // WaveMaskAvailable = 1 when the local wave-override cascade is baked + bound.
    uint32 WaveMaskAvailable = 0;
    // LocalFFTAvailable is a bit mask of bound local spectrum displacement streams.
    uint32 LocalFFTAvailable = 0;

    // --- Shape FFT wave-shape controls (one std430 16-byte lane, mirror in
    // ocean_common.glsl). Applied to the sampled FFT displacement by
    // OceanShapeDisplacement so geometry + macro normal stay consistent.
    //   Weight                    — overall displacement scale (0 = flat, 1 = full).
    //   MaxHorizontalDisplacement — clamp on |displacement.xz| (meters).
    //   MaxVerticalDisplacement   — clamp on |displacement.y| (meters).
    //   RespectShallowAttenuation — 0..1 flatten toward the shoreline by seabed depth.
    float32 Weight = 1.0f;
    float32 MaxHorizontalDisplacement = 15.0f;
    float32 MaxVerticalDisplacement = 10.0f;
    float32 RespectShallowAttenuation = 1.0f;

    // --- Surface grid geometry controls ---
    // One std430 16-byte lane. GeometryGridSize is the compatible surface budget
    // that determines each LOD tile's cell count. The vertex shader uses the
    // resulting cell width to estimate per-vertex footprint, so raising geometry
    // density also lets smaller displacement survive the anti-aliasing filter.
    float32 GeometryGridSize = static_cast<float32>(kDefaultOceanGeometryGridSize);
    // Accumulated floating-origin shift added only for wave phase evaluation.
    float32 WaveOriginOffsetX = 0.0f;
    float32 WaveOriginOffsetZ = 0.0f;
    float32 _GeometryPad0 = 0.0f;

    // Foam material detail, one std430 vec4 lane.
    float32 FoamNormalStrength = 0.35f;
    float32 FoamBubbleCoverage = 0.35f;
    float32 FoamBubbleParallax = 0.10f;
    float32 FoamRoughness = 0.65f;

    GerstnerWave Waves[kMaxGerstnerWaves] = {};
};

// Per-frame parameters for the FFT compute chain (std140 UBO). Layout MUST match
// OceanFFTParams in ocean_fft_common.glsl. the reference OceanWaveSpectrum parity: the
// per-octave power LUT is 10^powerLog * multiplier^2 (14 octaves, packed as four
// vec4s = 16 contiguous floats).
struct OceanFFTParamsGPU
{
    uint32 Resolution = kOceanFFTResolution;
    uint32 CascadeCount = kOceanFFTCascades;
    float32 Gravity = 9.81f;
    float32 Time = 0.0f;

    float32 WindSpeed = 8.0f;
    float32 WindDirX = 1.0f;
    float32 WindDirZ = 0.0f;
    float32 Turbulence = 0.145f; // the reference WindTurbulence

    float32 Chop = 1.6f;     // the reference _chop (global fallback / scale)
    float32 Multiplier = 1.0f;
    float32 Period = 0.0f;   // loop period (0 = none)
    float32 _FFTPad0 = 0.0f;

    // Per-octave spectrum LUTs (14 octaves used, the rest zero/identity-padded).
    // Each is packed as std140 vec4[4] = 16 contiguous floats so it stays compact
    // (a bare float[14] would pad each element to a 16-byte stride). The GLSL
    // mirror indexes them the same way: arr[octave>>2][octave&3].
    //   SpectrumPower  — linear power per octave (10^powerLog * multiplier^2),
    //                    multiplied by the per-octave SpectrumPower control.
    //   ChopScales     — per-octave horizontal-displacement (chop) multiplier;
    //                    the global Chop scales these. Sharpens/rounds crests by
    //                    wavelength band.
    //   GravityScales  — per-octave dispersion gravity multiplier; speeds up or
    //                    slows the waves in that wavelength band.
    float32 SpectrumPower[16] = {};
    float32 ChopScales[16] = {};
    float32 GravityScales[16] = {};

    // Per-octave disable bitmask: bit o set => octave o is muted (its power is
    // forced to 0 in the spectrum init). 14 bits used; the high bits are ignored.
    // One std140 16-byte lane (the uint + three pads).
    uint32 OctaveDisableMask = 0;
    uint32 _FFTPad1 = 0;
    uint32 _FFTPad2 = 0;
    uint32 _FFTPad3 = 0;
};
static_assert(sizeof(OceanFFTParamsGPU) == 256, "OceanFFTParamsGPU must be std140 (256 bytes)");

// The world layout of one camera-snapped cascade. One vec4 per LOD layer:
//   xy = layer origin (world XZ of texel (0,0), snapped to the texel grid)
//   z  = texel world size (meters per texel for this layer)
//   w  = lod scale (the layer's world extent / base extent; 2^lod)
// std140 — the vec4 array has a 16-byte stride, and the trailing scalar is
// padded to a full 16-byte lane. MUST match OceanCascadeLayoutData in
// ocean_common.glsl and the embedded layouts in ocean_foam_sim.comp and
// ocean_combine.comp byte-for-byte.
struct OceanCascadeLayoutGPU
{
    float32 CascadeOriginScale[kMaxOceanLodCascades][4] = {};
    uint32 LodCount = 0;
    // 1 when the coarsest layer is fixed in the world (OceanCascadeArray::
    // AnchorCoarsestLevel) rather than snapped to the camera: a sampler does not
    // blend a camera layer into it, since its texels follow the spread of all the
    // authored content, not the camera layers' doubling.
    uint32 CoarsestAnchored = 0;
    uint32 _CascadePad1 = 0;
    uint32 _CascadePad2 = 0;
};
static_assert(sizeof(OceanCascadeLayoutGPU) == 16 * (kMaxOceanLodCascades + 1),
              "OceanCascadeLayoutGPU must be std140 (vec4[N] + padded uint lane)");

// The camera-snapped cascades the surface and its helpers sample, by index into
// OceanSampledCascadeLayoutsGPU. Each sim snaps its cascade on its own schedule
// (a bake when its sources or snap change, a simulation only on the frames it
// steps), so each texture is sampled with the layout it was written with. Matches
// the GE_OCEAN_CASCADE_* constants in ocean_common.glsl.
enum class OceanSampledCascade : uint32
{
    Foam = 0,
    SeabedDepth = 1,
    Flow = 2,
    DynWaves = 3,
    WaveMask = 4,
    Clip = 5,
    Albedo = 6,
    Count = 7,
};

// The layout of every sampled cascade (std140 UBO "OceanCascadeLayout", set 2
// binding 4). A cascade whose sim is not running has LodCount 0, which every
// sample helper reads as "unavailable". MUST match the OceanCascadeLayout block in
// ocean_common.glsl byte-for-byte.
struct OceanSampledCascadeLayoutsGPU
{
    OceanCascadeLayoutGPU Layouts[static_cast<uint32>(OceanSampledCascade::Count)] = {};

    OceanCascadeLayoutGPU& operator[](OceanSampledCascade cascade)
    {
        return Layouts[static_cast<uint32>(cascade)];
    }
    const OceanCascadeLayoutGPU& operator[](OceanSampledCascade cascade) const
    {
        return Layouts[static_cast<uint32>(cascade)];
    }
};
static_assert(sizeof(OceanSampledCascadeLayoutsGPU) ==
                  sizeof(OceanCascadeLayoutGPU) * static_cast<uint32>(OceanSampledCascade::Count),
              "OceanSampledCascadeLayoutsGPU must be a tightly packed std140 array");

inline constexpr uint32 kMaxOceanFoamInputs = 8u;

struct OceanFoamInputGPU
{
    float32 OriginExtent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // x = amount, y = inward feather, z = OceanInputBlendMode, w = pad.
    float32 AmountFeatherBlend[4] = {0.0f, 0.0f, 1.0f, 0.0f};
};
static_assert(sizeof(OceanFoamInputGPU) == 32,
              "OceanFoamInputGPU must be std140 (two vec4 lanes)");

// Per-frame parameters for the foam simulation compute (std140 UBO). Carries the
// sim scalars, the FFT displacement cascade count (so the Jacobian sampling can
// skip inactive layers), the shoreline-foam controls (Phase 5), wave-shape
// controls, and the snapped foam cascade layouts. Layout MUST match
// OceanFoamParams in ocean_foam_sim.comp byte-for-byte.
struct OceanFoamParamsGPU
{
    uint32 Resolution = 0;
    uint32 LodCount = 0;
    uint32 FFTCascadeCount = 0;
    float32 DeltaTime = 0.0f;

    float32 FadeRate = 0.8f;        // per-second exponential decay
    float32 Coverage = 0.55f;       // Jacobian breaking threshold
    float32 Strength = 1.0f;        // foam deposited per breaking texel
    float32 ShorelineMaxDepth = 0.65f; // seabed depth below which shoreline foam appears

    // Shoreline foam (Phase 5). ShorelineStrength scales the deposit near the
    // waterline; SeabedDepthAvailable gates the seabed-depth sample (1 = the depth
    // cascade was written + bound this frame). FlowAvailable gates the flow-driven
    // advection (Phase 8); FlowScale converts the cascade velocity (m/s) to the
    // per-step advection magnitude.
    float32 ShorelineStrength = 2.0f;
    uint32 SeabedDepthAvailable = 0;
    uint32 FlowAvailable = 0;
    float32 FlowScale = 1.0f;

    // Match the visible surface's OceanShapeDisplacement controls so persistent
    // foam is deposited by the same crests that are actually rendered.
    float32 ShapeWeight = 1.0f;
    float32 ShapeMaxHorizontalDisplacement = 15.0f;
    float32 ShapeMaxVerticalDisplacement = 10.0f;
    float32 ShapeRespectShallowAttenuation = 1.0f;

    float32 ShapeSubSurfaceDepthMax = 10.0f;
    float32 WaveOriginOffsetX = 0.0f;
    float32 WaveOriginOffsetZ = 0.0f;
    uint32 ResetHistory = 0u;

    // Additional advection/source controls. WindVelocity is world XZ meters/sec
    // and provides a weak fallback drift even when no explicit flow cascade is
    // authored. CombinedDisplacementAvailable is reserved for a future stable
    // combined-displacement source; the render node currently keeps it off.
    float32 WindVelocityX = 0.0f;
    float32 WindVelocityZ = 0.0f;
    uint32 CombinedDisplacementAvailable = 0;
    uint32 AuthoredFoamSourceCount = 0;

    // Shoreline foam decays at this fraction of FadeRate. The shader reads it from
    // here, so the CPU's closed-form catch-up (OceanFoamSim::FillParams) and the
    // shader's per-step decay share one value.
    float32 ShorelineFadeRateScale = 0.65f;
    float32 ShorelinePad[3] = {0.0f, 0.0f, 0.0f};

    OceanCascadeLayoutGPU Cascade;
    OceanCascadeLayoutGPU PrevCascade;
    OceanCascadeLayoutGPU CombineCascade;
    OceanFoamInputGPU AuthoredFoamSources[kMaxOceanFoamInputs];
};
static_assert(sizeof(OceanFoamParamsGPU) ==
                  112 + sizeof(OceanCascadeLayoutGPU) * 3 +
                      kMaxOceanFoamInputs * sizeof(OceanFoamInputGPU),
              "OceanFoamParamsGPU must be std140 (seven scalar lanes + three embedded cascade layouts)");

// Per-frame parameters for the displacement-combine compute (std140 UBO). The
// combine pass sums the tileable FFT displacement cascades into the snapped
// combine cascade (one slice per LOD), per LOD only summing the cascades
// resolvable at that layer's texel (an anti-aliased LOD pyramid). Carries the
// resolution + LOD count, the active FFT cascade count (so inactive layers are
// skipped), and the snapped combine cascade layout. Layout MUST match
// OceanCombineParams in ocean_combine.comp byte-for-byte.
struct OceanCombineParamsGPU
{
    uint32 Resolution = 0;
    uint32 LodCount = 0;
    uint32 FFTCascadeCount = 0;
    uint32 _Pad0 = 0;

    float32 WaveOriginOffsetX = 0.0f;
    float32 WaveOriginOffsetZ = 0.0f;
    float32 _OriginPad0 = 0.0f;
    float32 _OriginPad1 = 0.0f;

    OceanCascadeLayoutGPU Cascade;
};
static_assert(sizeof(OceanCombineParamsGPU) == 32 + sizeof(OceanCascadeLayoutGPU),
              "OceanCombineParamsGPU must be std140 (one scalar lane + embedded cascade layout)");

// Maximum analytic seabeds baked into the sea-floor depth cascade in one frame.
// Each is a sloped rectangular plane; the bake takes the shallowest (largest
// depth) contributor per texel. Most scenes have one seabed; the cap keeps the
// std140 UBO bounded.
inline constexpr uint32 kMaxOceanSeabeds = 8;
inline constexpr uint32 kMaxOceanDepthContributors = 16;
inline constexpr uint32 kMaxOceanSavedDepthCachePages = 8;

// One analytic seabed plane for the depth bake (std140 vec4[2]):
//   OriginExtent.xy = footprint center (world XZ), .zw = half-extent (X, Z)
//   HeightSlope.x   = seabed height at the center (world Y, the calm floor)
//   HeightSlope.yz  = linear slope (height per meter) on X and Z
//   HeightSlope.w   = pad
struct OceanSeabedGPU
{
    float32 OriginExtent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float32 HeightSlope[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(OceanSeabedGPU) == 32, "OceanSeabedGPU must be std140 (two vec4 lanes)");

// One dynamic analytic depth contributor for the sea-floor depth bake (std140 vec4[2]):
//   OriginExtent.xy = footprint center (world XZ), .zw = half-extent (X, Z)
//   DepthShape.x    = positive depth below sea level written inside the footprint
//   DepthShape.y    = feather distance in meters
//   DepthShape.z    = roundness: 0 rectangle, 1 ellipse
//   DepthShape.w    = pad
// Returned where a depth band does not reach; above any depth the cascade holds.
inline constexpr float32 kOceanDepthBandOutside = 1.0e6f;

// Depth written by a feathered depth band at signed distance `edge` inside its
// edge: across the feather it ramps from the depth at which every shallow term
// has saturated down to the band depth, and for one more feather width outside
// the edge the same bank slope continues downward, so the field is linear across
// the edge and a bilinear read of the cascade puts the shallow boundary on the
// true edge rather than on its texel grid. A band no shallower than the
// saturation depth ends at its edge. Mirror of OceanDepthBandDepth in
// ocean_cascade_common.glsl.
inline float32 OceanDepthBandDepth(float32 bandDepth, float32 saturationDepth, float32 edge, float32 feather)
{
    const float32 outer = bandDepth > saturationDepth ? bandDepth : saturationDepth;
    const float32 width = feather > 1e-3f ? feather : 1e-3f;
    if (edge < -width || (edge < 0.0f && outer <= bandDepth))
        return kOceanDepthBandOutside;
    const float32 ramp = edge / width;
    return outer + (bandDepth - outer) * (ramp < 1.0f ? ramp : 1.0f);
}

struct OceanDepthContributorGPU
{
    float32 OriginExtent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float32 DepthShape[4] = {0.0f, 0.0f, 1.0f, 0.0f};
};
static_assert(sizeof(OceanDepthContributorGPU) == 32,
              "OceanDepthContributorGPU must be std140 (two vec4 lanes)");

// One page in the saved-depth-cache atlas. std140 vec4[3]:
//   OriginSize.xy = page world origin, zw = world size
//   InvSizeDeep.xy = inverse world size, z = deep-water sentinel
//   AtlasRect.xy = normalized atlas offset, zw = normalized atlas size
struct OceanSavedDepthCachePageGPU
{
    float32 OriginSize[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float32 InvSizeDeep[4] = {1.0f, 1.0f, 60000.0f, 0.0f};
    float32 AtlasRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
};
static_assert(sizeof(OceanSavedDepthCachePageGPU) == 48,
              "OceanSavedDepthCachePageGPU must be std140 (three vec4 lanes)");

// Per-frame parameters for the sea-floor depth bake compute (std140 UBO). Carries
// the sim scalars, the active seabed count + the calm sea level (so the bake can
// write depth = SeaLevel - seabedHeight), the seabed list, and the snapped depth
// cascade layout (the foam cascade's resolution, LOD count and base scale; the
// surface samples it with this layout). Layout MUST match
// OceanSeabedDepthParams in ocean_seafloor_depth.comp byte-for-byte.
struct OceanSeabedDepthParamsGPU
{
    uint32 Resolution = 0;
    uint32 LodCount = 0;
    uint32 SeabedCount = 0;
    float32 SeaLevel = 0.0f;

    uint32 DepthContributorCount = 0;
    // Depth (m) at which every shallow term has saturated; depth bands ramp
    // through it at their feathered edge (see OceanDepthBandDepth).
    float32 DepthBandSaturation = 0.0f;
    uint32 _DepthPad1 = 0;
    uint32 _DepthPad2 = 0;

    uint32 SavedDepthCacheAvailable = 0;
    uint32 SavedDepthCachePageCount = 0;
    uint32 _SavedDepthPad1 = 0;
    uint32 _SavedDepthPad2 = 0;

    float32 SavedDepthCacheOriginSize[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float32 SavedDepthCacheInvSizeDeep[4] = {1.0f, 1.0f, 60000.0f, 0.0f};

    uint32 RasterDepthCaptureAvailable = 0;
    uint32 _RasterDepthPad0 = 0;
    uint32 _RasterDepthPad1 = 0;
    uint32 _RasterDepthPad2 = 0;

    float32 RasterDepthCaptureOriginSize[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float32 RasterDepthCaptureInvSizeDeep[4] = {1.0f, 1.0f, 60000.0f, 0.0f};

    OceanCascadeLayoutGPU Cascade;
    OceanSavedDepthCachePageGPU SavedDepthCachePages[kMaxOceanSavedDepthCachePages];
    OceanSeabedGPU Seabeds[kMaxOceanSeabeds];
    OceanDepthContributorGPU DepthContributors[kMaxOceanDepthContributors];
};
static_assert(sizeof(OceanSeabedDepthParamsGPU) ==
                  128 + sizeof(OceanCascadeLayoutGPU) +
                      kMaxOceanSavedDepthCachePages * sizeof(OceanSavedDepthCachePageGPU) +
                      kMaxOceanSeabeds * sizeof(OceanSeabedGPU) +
                      kMaxOceanDepthContributors * sizeof(OceanDepthContributorGPU),
              "OceanSeabedDepthParamsGPU must be std140 (depth-cache lanes + cascade + pages + depth lists)");

// Maximum flow sources baked into the flow cascade in one frame. Analytic sources
// are rectangular or polygon footprints of constant horizontal current; flow-map
// sources sample a vector texture inside a rectangular footprint. Overlapping
// sources accumulate. Most scenes have a few (a river, a couple of eddies); the
// caps keep the std140 UBO bounded.
inline constexpr uint32 kMaxOceanFlowSources = 8;
inline constexpr uint32 kMaxOceanFlowPolygons = 4;
inline constexpr uint32 kMaxOceanFlowPolygonPoints = 8;
inline constexpr uint32 kMaxOceanFlowMapSources = 4;

// One analytic flow source for the flow bake (std140 vec4[2]):
//   OriginExtent.xy = footprint center (world XZ), .zw = half-extent (X, Z)
//   FlowVelocity.xy = horizontal current (meters/second) on world X and Z
//   FlowVelocity.zw = pad
struct OceanFlowSourceGPU
{
    float32 OriginExtent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float32 FlowVelocity[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(OceanFlowSourceGPU) == 32, "OceanFlowSourceGPU must be std140 (two vec4 lanes)");

// One bounded polygon flow source. Points are world-space XZ and use the first
// PointCount lanes; Bounds is min/max XZ for fast reject.
//   Meta.x          = point count
//   Meta.y          = optional inward feather distance in meters
//   FlowVelocity.xy = horizontal current (meters/second) on world X and Z
struct OceanFlowPolygonGPU
{
    float32 Meta[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float32 Bounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float32 FlowVelocity[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float32 Points[kMaxOceanFlowPolygonPoints][4] = {};
};
static_assert(sizeof(OceanFlowPolygonGPU) ==
                  (3 + kMaxOceanFlowPolygonPoints) * 16,
              "OceanFlowPolygonGPU must be std140 (vec4 lanes)");

// One texture-driven flow source. The texture is supplied in a parallel descriptor
// slot by OceanFlowSim; the GPU lanes define how to map world XZ into source UVs.
//   OriginExtent.xy = footprint center (world XZ), .zw = half-extent (X, Z)
//   StrengthFeather.xy = velocity scale (m/s) and inward feather distance (meters)
//   StrengthFeather.zw = additional flow bias on world X/Z (meters/second)
struct OceanFlowMapSourceGPU
{
    float32 OriginExtent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float32 StrengthFeather[4] = {1.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(OceanFlowMapSourceGPU) == 32,
              "OceanFlowMapSourceGPU must be std140 (two vec4 lanes)");

// Per-frame parameters for the flow bake compute (std140 UBO). Writes a
// camera-snapped RG16F flow cascade (world XZ current per texel) from the tagged
// analytic and texture flow sources, with the foam cascade's resolution, LOD
// count and base scale. Layout MUST match OceanFlowParams in ocean_flow_sim.comp
// byte-for-byte.
struct OceanFlowParamsGPU
{
    uint32 Resolution = 0;
    uint32 LodCount = 0;
    uint32 SourceCount = 0;
    uint32 _FlowPad0 = 0;

    OceanCascadeLayoutGPU Cascade;
    OceanFlowSourceGPU Sources[kMaxOceanFlowSources];
    uint32 PolygonCount = 0;
    uint32 _FlowPolygonPad0 = 0;
    uint32 _FlowPolygonPad1 = 0;
    uint32 _FlowPolygonPad2 = 0;
    OceanFlowPolygonGPU Polygons[kMaxOceanFlowPolygons];
    uint32 FlowMapCount = 0;
    uint32 _FlowMapPad0 = 0;
    uint32 _FlowMapPad1 = 0;
    uint32 _FlowMapPad2 = 0;
    OceanFlowMapSourceGPU FlowMaps[kMaxOceanFlowMapSources];
};
static_assert(sizeof(OceanFlowParamsGPU) ==
                  48 + sizeof(OceanCascadeLayoutGPU) +
                      kMaxOceanFlowSources * sizeof(OceanFlowSourceGPU) +
                      kMaxOceanFlowPolygons * sizeof(OceanFlowPolygonGPU) +
                      kMaxOceanFlowMapSources * sizeof(OceanFlowMapSourceGPU),
              "OceanFlowParamsGPU must be std140 (scalar lanes + cascade + flow lists)");

// Maximum local wave-override sources baked into the wave-mask cascade. The mask
// stores local values: R = wave weight/amplitude, G = horizontal chop scale,
// B = local directional wave height, A = spare.
inline constexpr uint32 kMaxOceanWaveMaskSources = 8;
inline constexpr uint32 kMaxOceanWaveMaskPolygons = 4;
inline constexpr uint32 kMaxOceanWaveMaskPolygonPoints = 8;
inline constexpr uint32 kMaxOceanWaveMaskLocalWaves = 4;
inline constexpr uint32 kMaxOceanWaveMaskTextureSources = 4;

struct OceanWaveMaskSourceGPU
{
    float32 OriginExtent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // x = wave weight, y = chop scale, z = inward feather meters,
    // w = local wave count
    float32 WaveChopFeather[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    // Each lane: x = amplitude, y = wavelength, zw = normalized direction XZ.
    float32 LocalWaves[kMaxOceanWaveMaskLocalWaves][4] = {};
    // x = local FFT blend weight, y = local FFT stream index,
    // z = authored constant height offset, w = OceanInputBlendMode.
    float32 LocalFFTBlend[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(OceanWaveMaskSourceGPU) == (3 + kMaxOceanWaveMaskLocalWaves) * 16,
              "OceanWaveMaskSourceGPU must be std140 (vec4 lanes)");

struct OceanWaveMaskPolygonGPU
{
    // x = point count, y = inward feather meters, z = local wave count
    float32 Meta[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float32 Bounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // x = wave weight, y = chop scale, z = local FFT blend weight, w = local FFT stream index
    float32 WaveChop[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    // Each lane: x = amplitude, y = wavelength, zw = normalized direction XZ.
    float32 LocalWaves[kMaxOceanWaveMaskLocalWaves][4] = {};
    float32 Points[kMaxOceanWaveMaskPolygonPoints][4] = {};
};
static_assert(sizeof(OceanWaveMaskPolygonGPU) ==
                  (3 + kMaxOceanWaveMaskLocalWaves +
                   kMaxOceanWaveMaskPolygonPoints) * 16,
              "OceanWaveMaskPolygonGPU must be std140 (vec4 lanes)");

// One texture-authored wave mask source. The texture is supplied in a parallel
// descriptor slot by OceanWaveMaskSim; red = target wave weight, green = target
// chop scale. Texture values are shaped by scale/bias, blended by coverage, and
// feathered inside the rectangular footprint.
struct OceanWaveMaskTextureSourceGPU
{
    float32 OriginExtent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // x = weight scale, y = chop scale, z = weight bias, w = chop bias
    float32 ScaleBias[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    // x = inward feather meters, y = coverage, zw = pad
    float32 FeatherCoverage[4] = {0.0f, 1.0f, 0.0f, 0.0f};
};
static_assert(sizeof(OceanWaveMaskTextureSourceGPU) == 48,
              "OceanWaveMaskTextureSourceGPU must be std140 (three vec4 lanes)");

struct OceanWaveMaskParamsGPU
{
    uint32 Resolution = 0;
    uint32 LodCount = 0;
    uint32 SourceCount = 0;
    uint32 PolygonCount = 0;

    float32 Time = 0.0f;
    float32 WaveOriginOffsetX = 0.0f;
    float32 WaveOriginOffsetZ = 0.0f;
    float32 _WaveMaskPad2 = 0.0f;

    OceanCascadeLayoutGPU Cascade;
    OceanWaveMaskSourceGPU Sources[kMaxOceanWaveMaskSources];
    OceanWaveMaskPolygonGPU Polygons[kMaxOceanWaveMaskPolygons];
    uint32 TextureSourceCount = 0;
    uint32 _WaveMaskTexturePad0 = 0;
    uint32 _WaveMaskTexturePad1 = 0;
    uint32 _WaveMaskTexturePad2 = 0;
    OceanWaveMaskTextureSourceGPU TextureSources[kMaxOceanWaveMaskTextureSources];
};
static_assert(sizeof(OceanWaveMaskParamsGPU) ==
                  48 + sizeof(OceanCascadeLayoutGPU) +
                      kMaxOceanWaveMaskSources * sizeof(OceanWaveMaskSourceGPU) +
                      kMaxOceanWaveMaskPolygons * sizeof(OceanWaveMaskPolygonGPU) +
                      kMaxOceanWaveMaskTextureSources *
                          sizeof(OceanWaveMaskTextureSourceGPU),
              "OceanWaveMaskParamsGPU must be std140 (scalar lanes + cascade + wave-mask lists)");

// Maximum dynamic-wave impulses injected into the sim in one frame. Each is a
// circular footprint depositing a vertical displacement. The cap keeps the std140
// UBO bounded; extra impulses are ignored.
inline constexpr uint32 kMaxOceanWaveImpulses = 16;

// One dynamic-wave impulse (std140 vec4):
//   x,y = impact center (world XZ)
//   z   = radius (meters)
//   w   = amplitude (meters, vertical displacement at center)
struct OceanWaveImpulseGPU
{
    float32 CenterRadiusAmp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(OceanWaveImpulseGPU) == 16, "OceanWaveImpulseGPU must be std140 (one vec4)");

// Per-frame parameters for the dynamic-wave simulation compute (std140 UBO).
// Solves the 2D wave equation on a ping-pong RG16F (height, velocity) cascade and
// injects this frame's impulses, with the foam cascade's resolution, LOD count
// and base scale. Layout MUST match
// OceanDynWavesParams in ocean_dynwaves_sim.comp byte-for-byte.
struct OceanDynWavesParamsGPU
{
    uint32 Resolution = 0;
    uint32 LodCount = 0;
    uint32 ImpulseCount = 0;
    float32 DeltaTime = 0.0f;

    float32 WaveSpeed = 4.0f;     // base propagation speed c (clamped to CFL per-LOD)
    float32 Damping = 0.2f;       // per-second velocity damping
    float32 CourantNumber = 0.7f; // CFL safety factor (< 1 for stability)
    float32 Gravity = 9.81f;      // dispersion gravity for the per-LOD speed

    float32 ShallowAttenuation = 1.0f;
    float32 HorizontalDisplacement = 0.0f;
    float32 DisplacementClamp = 4.0f;
    uint32 SeabedDepthAvailable = 0u;

    uint32 MinimumCascade = 0u;
    uint32 MaximumCascade = kMaxOceanLodCascades - 1u;
    float32 ShallowDepthScale = 4.0f;
    uint32 ResetHistory = 0u;

    OceanCascadeLayoutGPU Cascade;
    OceanCascadeLayoutGPU PrevCascade;
    OceanWaveImpulseGPU Impulses[kMaxOceanWaveImpulses];
};
static_assert(sizeof(OceanDynWavesParamsGPU) ==
                  64 + sizeof(OceanCascadeLayoutGPU) * 2 +
                      kMaxOceanWaveImpulses * sizeof(OceanWaveImpulseGPU),
              "OceanDynWavesParamsGPU must be std140 (four scalar lanes + two cascades + impulse list)");

// Maximum clip sources composited into the clip cascade in one frame. Each is a
// rectangular footprint that sets the surface clip state (cut a hole / restore
// the water) inside it. The cap keeps the std140 UBO bounded; extra sources are
// ignored.
inline constexpr uint32 kMaxOceanClipSources = 8;
inline constexpr uint32 kMaxOceanClipPolygons = 4;
inline constexpr uint32 kMaxOceanClipPolygonPoints = 8;

// One analytic clip source for the clip bake (std140 vec4[2]):
//   OriginExtent.xy = footprint center (world XZ), .zw = half-extent (X, Z)
//   ClipState.x     = clip value written inside the footprint (1 = clipped/hole,
//                     0 = solid water)
//   ClipState.y     = optional inward feather distance in meters (0 = legacy
//                     normalized edge smoothing for rectangles)
//   ClipState.zw    = pad
struct OceanClipSourceGPU
{
    float32 OriginExtent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // z = encoded typed blend mode + 1 (0 keeps legacy clip composition).
    float32 ClipState[4] = {1.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(OceanClipSourceGPU) == 32, "OceanClipSourceGPU must be std140 (two vec4 lanes)");

// One bounded polygon clip source. Points are world-space XZ and use the first
// PointCount lanes; Bounds is min/max XZ for fast reject. Meta.x stores
// PointCount as a float, Meta.y stores clip state (0 restore water, 1 cut hole),
// and Meta.z stores optional inward feather distance in meters.
struct OceanClipPolygonGPU
{
    float32 Meta[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float32 Bounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float32 Points[kMaxOceanClipPolygonPoints][4] = {};
};
static_assert(sizeof(OceanClipPolygonGPU) ==
                  (2 + kMaxOceanClipPolygonPoints) * 16,
              "OceanClipPolygonGPU must be std140 (vec4 lanes)");

// Per-frame parameters for the clip bake compute (std140 UBO). Writes a
// camera-snapped R8_UNORM clip cascade (surface clip state per texel) from the
// tagged clip sources over a default clip state, with the foam cascade's
// resolution and base scale and one more (anchored) layer. Layout MUST match OceanClipParams in
// ocean_clip_sim.comp byte-for-byte.
struct OceanClipParamsGPU
{
    uint32 Resolution = 0;
    uint32 LodCount = 0;
    uint32 SourceCount = 0;
    float32 DefaultClippingState = 0.0f; // clip value outside every source

    OceanCascadeLayoutGPU Cascade;
    OceanClipSourceGPU Sources[kMaxOceanClipSources];
    uint32 PolygonCount = 0;
    uint32 _PolygonPad0 = 0;
    uint32 _PolygonPad1 = 0;
    uint32 _PolygonPad2 = 0;
    OceanClipPolygonGPU Polygons[kMaxOceanClipPolygons];
};
static_assert(sizeof(OceanClipParamsGPU) ==
                  32 + sizeof(OceanCascadeLayoutGPU) +
                      kMaxOceanClipSources * sizeof(OceanClipSourceGPU) +
                      kMaxOceanClipPolygons * sizeof(OceanClipPolygonGPU),
              "OceanClipParamsGPU must be std140 (scalar lanes + cascade + clip lists)");

// Maximum albedo sources composited into the albedo cascade in one frame. Each is
// a rectangular footprint painting a colour (rgb) + coverage (a) onto the surface
// albedo. The cap keeps the std140 UBO bounded; extra sources are ignored.
inline constexpr uint32 kMaxOceanAlbedoSources = 8;

// One analytic albedo source for the albedo bake (std140 vec4[2]):
//   OriginExtent.xy = footprint center (world XZ), .zw = half-extent (X, Z)
//   Color.rgba      = the albedo painted inside the footprint; rgb is the colour,
//                     a is the blend weight (0 = no override, 1 = full paint).
//                     Overlapping sources composite (over) by their alpha.
struct OceanAlbedoSourceGPU
{
    float32 OriginExtent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float32 Color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(OceanAlbedoSourceGPU) == 32, "OceanAlbedoSourceGPU must be std140 (two vec4 lanes)");

// Per-frame parameters for the albedo bake compute (std140 UBO). Writes a
// camera-snapped RGBA8_UNORM albedo cascade (surface paint per texel) from the
// tagged albedo sources, with the foam cascade's resolution, LOD count and base
// scale. Layout MUST match OceanAlbedoParams in ocean_albedo_sim.comp byte-for-byte.
struct OceanAlbedoParamsGPU
{
    uint32 Resolution = 0;
    uint32 LodCount = 0;
    uint32 SourceCount = 0;
    uint32 _AlbedoPad0 = 0;

    OceanCascadeLayoutGPU Cascade;
    OceanAlbedoSourceGPU Sources[kMaxOceanAlbedoSources];
};
static_assert(sizeof(OceanAlbedoParamsGPU) ==
                  16 + sizeof(OceanCascadeLayoutGPU) + kMaxOceanAlbedoSources * sizeof(OceanAlbedoSourceGPU),
              "OceanAlbedoParamsGPU must be std140 (one scalar lane + cascade + source list)");

} // namespace GameEngine::Ocean
