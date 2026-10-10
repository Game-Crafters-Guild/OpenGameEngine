#pragma once

#include "Types/Color.h"
#include "Types/StringId.h"
#include "Types/StringUtils.h"
#include "Types/Types.h"
#include "Components/AssetRef.h"

#include <string_view>

namespace GameEngine::Components
{

// Global base-wave generator. FFT is the default; Gerstner remains available for
// low-cost authored water and is also the deterministic compute-failure fallback.
enum class OceanWaveMode : uint32
{
    Gerstner = 0,
    FFT = 1,
};

// Depth fog falloff shape for the ocean surface refraction and underwater pass.
// Exponential preserves the classic Beer-Lambert water absorption. Linear and
// Smooth remap the authored start/end distance before applying the per-channel
// density, giving more direct art control over when the seabed/ship disappears.
enum class OceanDepthFogFalloff : uint32
{
    Exponential = 0,
    Linear = 1,
    Smooth = 2,
};

enum class OceanTimeProviderMode : uint32
{
    Default = 0u,
    Custom = 1u,
    Paused = 2u,
    NetworkOffset = 3u,
    Timeline = 4u,
};

inline constexpr uint32 OceanDepthCachePathCapacity = 260u;
inline constexpr uint32 OceanPolygonWaterBodyPointCapacity = 8u;
inline constexpr uint32 OceanWaterBodyLocalWaveCapacity = 4u;
inline constexpr uint32 OceanWaterBodyExtraLocalWaveCapacity =
    OceanWaterBodyLocalWaveCapacity - 1u;

// Marks an entity as the ocean. One ocean per scene. The render feature builds a
// camera-following clipmap surface and drives the wave simulations from the
// spectrum below. The entity's world transform supplies the calm sea-level height
// (Y); the surface still follows the camera horizontally.
struct OceanSurface
{
    // Legacy serialized sea level. Hidden from authoring; runtime sea level comes
    // from the entity transform Y, with this only kept as a load fallback.
    float32 SeaLevel = 0.0f;

    // FFT is the default. Gerstner is a supported authored mode and the runtime
    // fallback when compute/readback data is unavailable.
    OceanWaveMode WaveMode = OceanWaveMode::FFT;

    // Water colors (linear RGBA), shown as swatches with pickers in the inspector.
    // DeepColor = deep-water diffuse base. The shallow/scatter tint is authored via
    // SubSurfaceShallowCol; the sky tint via SkyBase/SkyTowardsSun/SkyAwayFromSun.
    ColorLinear DeepColor{0.05f, 0.07f, 0.20f, 1.0f};

    // Whitecap color (the reference _FoamWhiteColor).
    ColorLinear FoamColor{1.0f, 0.996f, 0.972f, 1.0f};

    // Horizontal displacement (choppiness) multiplier. 0 = round swells,
    // 1 = sharp wind-chop crests.
    float32 ChoppyScale = 1.0f;

    // Fresnel falloff exponent for the reflection blend (higher = reflection
    // concentrated nearer the horizon).
    float32 FresnelPower = 5.0f;

    // Sky/planar reflection amount (0 = none, 1 = full). Scales the Fresnel
    // reflection blend (on top of Specular), so you can dial the mirrored sky /
    // planar capture up or down. 1 = the look without this control.
    float32 ReflectionStrength = 1.0f;

    // Subsurface-scatter (teal glow) intensity (the reference _SubSurfaceBase).
    float32 SubsurfaceStrength = 1.0f;

    // Whitecap coverage scale for the Jacobian/steepness foam.
    float32 FoamAmount = 1.0f;

    // --- Persistent foam simulation ---
    // Foam accumulates on breaking crests and decays over time, so whitecaps
    // build up and trail behind waves rather than being a per-pixel function of
    // the instantaneous wave shape.

    // Per-second exponential decay of accumulated foam. Lower = foam lingers
    // longer (longer whitecap trails); higher = foam dissipates quickly.
    float32 FoamFadeRate = 0.8f;

    // How much foam a breaking/pinching crest deposits per frame.
    float32 WaveFoamStrength = 1.0f;

    // Jacobian threshold below which a wave is "breaking" and deposits foam.
    // Higher = foam forms more readily (more coverage); lower = only the
    // sharpest folds foam.
    float32 WaveFoamCoverage = 0.55f;

    // World tiling (meters) of the bubbly foam surface texture.
    float32 FoamScale = 0.25f;

    // Softness of the foam coverage/threshold dissolve. Higher feathers whitecap
    // edges into bubbles; lower gives crisper foam borders.
    float32 FoamFeather = 0.4f;

    // --- Wind spray particles ---
    // Droplets torn from raised, breaking wave crests once the resolved scene
    // wind exceeds SprayWindThreshold. Emitted, simulated and drawn on the GPU
    // around the camera and carried downwind; lit like the water surface. Each
    // sprite stands for a small cluster of droplets, so the sizes are a few
    // centimeters: single millimeter droplets would be sub-pixel beyond a meter.
    bool Spray = true;
    // Droplets alive at once, per view.
    // @ge-tooltip Most spray droplet clusters alive at once in each view.
    uint32 SprayMaxParticles = 16384u;
    float32 SprayWindThreshold = 9.0f;
    // Crest points tested per second on each square meter of the spawn disk.
    // @ge-tooltip Crest points tested for spray each second, per square meter of the spawn disk.
    float32 SpraySpawnRate = 800.0f;
    // Radius (m) of the disk around the camera where droplets are born.
    // @ge-tooltip Radius in meters around the camera within which spray can be born.
    float32 SpraySpawnRadius = 15.0f;
    // Minimum crest score (0..1) required before a droplet detaches. Raise this
    // to restrict spray to the strongest breaking crests.
    float32 SprayEmissionThreshold = 0.08f;
    float32 SprayLifetime = 0.9f;
    float32 SprayStartSize = 0.05f;
    float32 SprayEndSize = 0.12f;
    // Initial upward launch speed. Crest and intersection droplets then
    // accelerate under standard world gravity so they arc back toward the
    // surface instead of drifting upward for their lifetime.
    float32 SprayUpVelocity = 1.6f;
    float32 SprayWindVelocityScale = 0.55f;
    float32 SprayOpacity = 0.85f;
    uint32 SprayRenderLayerMask = 1u;

    // Emits extra short splash bursts where opaque geometry cuts the waterline
    // (ship hulls, piers, rocks), found by comparing the wave surface with the
    // scene depth within SprayIntersectionBand meters.
    bool SprayGeometryIntersections = true;
    // Contact points tested per second on each square meter of the spawn disk.
    float32 SprayIntersectionSpawnRate = 20.0f;
    float32 SprayIntersectionBand = 1.25f;

    // Optional user foam-bubble texture. When assigned, the surface samples its red
    // channel for the whitecap bubble pattern instead of the built-in analytic
    // (Worley) foam. Tiled by FoamScale. Empty = procedural foam.
    TextureRef FoamTexture;

    // Debug overlay for foam/depth placement. 0 = normal, 1 = combined foam,
    // 2 = wave foam, 3 = shoreline/contact foam, 4 = freshness, 5 = latest deposit,
    // 6 = raw seabed depth ramp, 7 = normalized shallow mask, 8 = invalid/deep tiles.
    uint32 FoamDebugMode = 0;

    // --- Shoreline foam (driven by the sea-floor depth cascade) ---
    // Where the water thins over a tagged OceanSeabed, foam builds along the
    // waterline. ShorelineFoamMaxDepth is the seabed depth (meters) below which
    // shoreline foam appears — full at the waterline (0 m), fading to none at this
    // depth. ShorelineFoamStrength scales how strongly it adds foam. Inert with no
    // tagged seabed (the depth cascade reads deep water everywhere).
    float32 ShorelineFoamMaxDepth = 0.65f;
    float32 ShorelineFoamStrength = 2.0f;

    // --- Surface normals ---
    // Strength of the high-frequency surface normal detail and its world tiling
    // scale. Strength 0 flattens micro-ripples (mirror); higher sharpens chop.
    float32 NormalsStrength = 1.0f;
    float32 NormalsScale = 1.0f;

    // --- Diffuse scattering (view-weighted) ---
    // The lit surface scatters between a grazing tint (looking across the water)
    // and an up-look tint (looking down into it); DiffuseShadow tints faces the
    // sun doesn't reach. Linear RGBA swatches.
    ColorLinear Diffuse{0.0f, 0.0027f, 0.17f, 1.0f};
    ColorLinear DiffuseGrazing{0.0f, 0.0039f, 0.169f, 1.0f};
    ColorLinear DiffuseShadow{0.0f, 0.00135f, 0.085f, 1.0f};

    // --- Subsurface scattering ---
    // Shallow-water tint (toward shore; shaped by depth once the depth cascade
    // lands) and the scatter colour the sun lobe pushes through wave crests.
    ColorLinear SubSurfaceShallowCol{0.42f, 0.75f, 0.69f, 1.0f};

    // Depth at which the shallow tint fully fades to deep water (meters), and the
    // shaping exponent of that fade. Inert until the Phase 5 depth cascade.
    float32 SubSurfaceDepthMax = 10.0f;
    float32 SubSurfaceDepthPower = 2.5f;

    // Scatter colour, the ambient transmission present everywhere (Base), the
    // forward-scatter lobe gain toward the sun (Sun), and that lobe's falloff.
    ColorLinear SubSurfaceColour{0.0885f, 0.497f, 0.456f, 1.0f};
    float32 SubSurfaceBase = 0.0f;
    float32 SubSurfaceSun = 1.7f;
    float32 SubSurfaceSunFallOff = 5.0f;

    // --- Reflection (IOR-driven Schlick Fresnel) ---
    // Reflection intensity (caps the sky blend), microfacet roughness, and the
    // two indices of refraction that set the base reflectance R0.
    float32 Specular = 0.7f;
    float32 Roughness = 0.0f;
    float32 IorAir = 1.0f;
    float32 IorWater = 1.333f;

    // Use a planar reflection capture instead of only the procedural sky. The
    // procedural sky remains the fallback when the capture declines for a frame.
    bool PlanarReflections = true;

    // Blends the planar reflection capture over the procedural reflected sky.
    // 0 keeps the capture running but shows only sky; 1 uses the captured scene;
    // up to 3 over-drives it brighter than the sky for a stronger mirror.
    float32 PlanarReflectionStrength = 1.0f;

    // Resolution scale of the planar reflection capture relative to the main
    // view. 0.5 keeps the previous half-res cost; 1.0 captures at full view size.
    float32 PlanarReflectionScale = 0.5f;

    // --- Procedural sky dome ---
    // The reflected sky: zenith/base colour, the warmer tint toward the sun, and
    // the tint away from it; Directionality steers how sharply they blend.
    ColorLinear SkyBase{0.09f, 0.19f, 0.34f, 1.0f};
    ColorLinear SkyTowardsSun{0.27f, 0.39f, 0.49f, 1.0f};
    ColorLinear SkyAwayFromSun{0.06f, 0.13f, 0.26f, 1.0f};
    float32 SkyDirectionality = 1.0f;

    // --- Directional-light glitter ---
    // Tint, boost and falloff of the direct sun glitter spike on the water.
    // White preserves the scene light colour; tint this to colour the bright
    // reflected glints without changing the actual Directional Light.
    ColorLinear DirectionalLightColor{1.0f, 1.0f, 1.0f, 1.0f};
    float32 DirectionalLightBoost = 7.0f;
    float32 DirectionalLightFallOff = 275.0f;

    // --- Refraction / depth-fog transparency ---
    // Per-channel extinction of the scene seen through the water. The surface
    // samples a grab of the opaque scene colour and the scene depth, then fades
    // that grabbed colour toward the deep-water body by depth: thin water shows
    // the seabed, deep water hides it. Higher density fades faster, so blue (low
    // density) carries deepest while red/green absorb first (the classic
    // water-colour falloff). RefractionStrength offsets the screen-space grab UV
    // by the surface normal so ripples distort whatever is below. Authored as a
    // colour swatch for the per-channel densities (the alpha is unused).
    ColorLinear DepthFogDensity{0.9f, 0.3f, 0.35f, 1.0f};

    // Falloff shape and distance controls for depth fog. StartDistance keeps the
    // first meters clearer. EndDistance is the authored range for Linear/Smooth;
    // 0 uses an automatic range derived from the density. FalloffPower reshapes
    // the ramp (1 = neutral, >1 = slower near the camera, <1 = faster).
    OceanDepthFogFalloff DepthFogFalloff = OceanDepthFogFalloff::Exponential;
    float32 DepthFogStartDistance = 0.0f;
    float32 DepthFogEndDistance = 0.0f;
    float32 DepthFogFalloffPower = 1.0f;
    float32 RefractionStrength = 0.5f;

    // Reduces sky/planar reflection where shallow refraction is clear, so seabed
    // caustics are not buried by the reflected surface. 0 = no reduction; 1 =
    // current strong shallow-water reduction.
    float32 ShallowRefractionReflectionSuppression = 1.0f;

    // Shallow clarity window: the depth fog ramps up over the first meters of the
    // view ray's path through the water, so the seabed and its caustics stay
    // readable in the shallows whatever the density. Not applied seen from below.
    // Meters of view-ray path over which the fog ramps to full; shorter brings the deep color in sooner.
    float32 ShallowClarityDistance = 8.0f;
    // Fraction of the depth fog applied at the surface, where the ramp starts; 1 turns the window off.
    float32 ShallowClarityFloor = 0.22f;

    // --- Underwater caustics ---
    // Refracted-sunlight focusing painted onto whatever the water refracts
    // (requires refraction active). A procedural caustics web is sampled twice at
    // two scrolling scales, its lookup UVs bent by a distortion sample and raked
    // along the sun direction; the result modulates the refracted scene colour.
    // Inert when refraction is unavailable (the seabed isn't visible to refract).

    // World tiling (meters) of one caustics texture period — smaller tiles the web
    // tighter (more, smaller cells), larger spreads it out.
    float32 CausticsScale = 5.0f;

    // Mean intensity of the caustics web, subtracted so the effect brightens AND
    // darkens the scene around neutral rather than only adding light.
    float32 CausticsAverage = 0.07f;

    // Overall caustic contrast multiplied into the refracted scene colour.
    float32 CausticsStrength = 3.2f;

    // Seabed distance (meters) at which the caustics are sharpest; the web blurs
    // (lower mip) away from this depth, focusing the effect at one water depth.
    float32 CausticsFocalDepth = 2.0f;

    // How quickly caustics blur away from the focal depth. Smaller = a tighter
    // in-focus band; larger keeps caustics crisp across a wider depth range.
    float32 CausticsDepthOfField = 0.33f;

    // How strongly a distortion sample bends the caustic lookup UVs (0 = rigid
    // web, higher = more rippling/wobble as if seen through moving water).
    float32 CausticsDistortionStrength = 0.16f;

    // World tiling (meters) of the distortion sample that warps the caustic UVs.
    float32 CausticsDistortionScale = 25.0f;

    // Optional user caustics texture. When assigned, the underwater caustics sample
    // this image's red channel instead of the built-in procedural web (and skip the
    // encoded-normal UV distortion, which a plain image has no channel for). Tiled
    // by CausticsScale. Empty = procedural caustics.
    TextureRef CausticsTexture;

    // --- Underwater rendering ---
    // When the camera dips below the displaced surface, a fullscreen pass tints
    // the screen with a submerged colour, fades distant geometry into the deep
    // water by depth (reusing DepthFogDensity from below), and draws a bright
    // meniscus band along the screen-space waterline. The surface itself becomes
    // two-sided (its back faces shade with a flipped normal + subsurface glow).
    // This toggle only enables the effect; the engine still CPU-gates the
    // fullscreen pass to frames where the camera is genuinely submerged. Off
    // keeps the ocean a one-sided above-water surface (no underwater pass).
    bool Underwater = true;

    // Artistic width of the meniscus where the lens crosses the surface.
    // The shader converts it to a pixel-sized band. Zero disables the band.
    float32 MeniscusWidth = 0.05f;

    // World-space fade distance (meters) over which the underwater composite blends
    // across the displaced waterline. Larger values soften the transition.
    float32 WaterlineFadeDistance = 0.05f;

    // --- Underwater effects (drive the fullscreen OceanUnderwater pass) ---
    // Sun light scattered through the water (Henyey-Greenstein single scatter),
    // screen distortion (lens barrel + waterline pinch), procedural god-ray shafts,
    // and caustics projected onto the submerged seabed (shimmer visible from under
    // the water, not just through the surface). Routed to the pass via the feature.
    bool UnderwaterInscattering = true;
    float32 InscatterStrength = 1.0f;
    float32 InscatterPhaseG = 0.25f;   // 0 = isotropic scatter, toward 1 = forward-peaked
    bool UnderwaterDistortion = true;
    float32 DistortionStrength = 1.0f;
    bool UnderwaterGodRays = true;
    float32 GodRayStrength = 0.6f;
    float32 GodRayDensity = 32.0f;     // angular frequency of the shafts
    bool CausticsOnGeometry = true;
    // Caustics reflected onto geometry above the water; 0 (the default) declares no pass.
    // @ge-tooltip Brightness of the caustics the water reflects onto geometry above it. 0 turns them off (the default).
    float32 ReflectedCausticsStrength = 0.0f;
    float32 ReflectedCausticsHeight = 2.5f;
    float32 ReflectedCausticsFalloff = 0.45f;

    // --- Flow field ---
    // A 2D horizontal current field (meters/second) advects the persistent foam
    // (the foam sim reprojects its previous world position through this cascade) and
    // scrolls the high-frequency surface detail-normal UVs, so foam and ripples
    // drift along the flow rather than sitting in place. Sourced from tagged
    // OceanFlowSource entities (rectangular regions of constant flow) and
    // OceanFlowMapSource entities (texture-driven currents), baked into a
    // camera-snapped flow cascade. Off leaves foam static and ripples stationary.
    bool Flow = true;

    // --- Dynamic (interactive) waves ---
    // A 2D wave-equation simulation layered on top of the FFT spectrum:
    // ripples spread and decay from impulses injected by tagged OceanWaveImpulse
    // entities (e.g. a buoyant body striking the water). The dynamic height is
    // added into the surface displacement (vertex stage) and folded into the
    // surface normal, so interactive ripples actually deform the geometry and
    // light correctly. Off disables the sim (the surface reads spectrum waves only).
    bool DynamicWaves = true;

    // --- Clip surface (cut holes) ---
    // Cuts holes in the water surface (harbors, docks, boat interiors). Tagged
    // OceanClipSource entities (rectangular regions) composite a clip state into a
    // camera-snapped clip cascade; where the surface samples a clip value above
    // 0.5 the fragment is discarded (nothing renders, leaving what's behind the
    // water visible). Off leaves the surface solid everywhere.
    bool ClipSurface = false;

    // The clip state the surface starts at outside every clip source (0 = surface
    // solid by default, sources punch holes; 1 = surface clipped by default,
    // sources carve the water back in). Lets a project clip the whole ocean and
    // restore only authored regions. Inert unless ClipSurface is on.
    float32 DefaultClippingState = 0.0f;

    // --- Albedo (decals / paint on the surface) ---
    // Paints colour onto the water surface albedo (decals, spilled paint, dye).
    // Tagged OceanAlbedoSource entities (rectangular regions) composite a colour +
    // coverage into a camera-snapped albedo cascade; the surface blends its base
    // water colour toward the painted colour BEFORE lighting and UNDER foam, so
    // reflections + whitecaps still read over the paint. Off leaves the base water
    // colour unchanged.
    bool Albedo = false;

    // --- Intersection foam (screen-space, from scene depth) ---
    // Where the water surface meets opaque scene geometry (rocks, terrain, piers),
    // foam builds along the contact line. IntersectionFoamDepth (meters) is the
    // water-to-geometry distance shaping a narrow bright rim plus a wider noisy
    // wash; smaller values make crisper lines. IntersectionFoamStrength scales the
    // effect. Unlike shoreline foam (from the seabed depth cascade), this reads
    // the per-frame scene depth buffer, so it works with any opaque geometry.
    float32 IntersectionFoamDepth = 1.0f;
    float32 IntersectionFoamStrength = 0.5f;

    // Foam relief uses the same dissolve pattern as coverage. Zero keeps it flat.
    // @ge-tooltip Relief of textured foam. Zero keeps foam flat. Only used with a foam texture.
    float32 FoamNormalStrength = 0.35f;
    // @ge-tooltip Amount of submerged bubbles drawn under textured foam. Only used with a foam texture.
    float32 FoamBubbleCoverage = 0.35f;
    // @ge-tooltip Apparent depth of the submerged bubble layer as the view angle changes.
    float32 FoamBubbleParallax = 0.10f;
    // @ge-tooltip Surface roughness where textured foam covers the water.
    float32 FoamRoughness = 0.65f;
    // Optional linear RGB tangent-space normal map (XY slopes, Z up).
    // Empty retains analytic ripples. NormalsScale multiplies the texture frequency.
    // @ge-tooltip Optional detail normal map for small ripples. Assigning it turns on the detailed surface shading. Empty keeps the analytic ripples.
    TextureRef NormalTexture;
};

// Renderer-level configuration for the ocean. Separate from OceanSurface (the
// per-material look) so a project can dial the LOD/scale budget and the global
// wind in one place, and pick a quality tier. One per scene, alongside the
// OceanSurface. All optional — the defaults reproduce the built-in behavior.
struct OceanRenderer
{
    // Quality tier. Gates which simulations run. The FFT resolution + cascade-array
    // sizes are compile-time, so the tier does not resize them — it trims the
    // expensive optional sims. High runs every sim; Medium drops flow + dynamic
    // waves; Low keeps only the FFT wave shape.
    // 0 = Low, 1 = Medium, 2 = High (matches Ocean::OceanQuality).
    uint32 QualityOverride = 2;

    // Number of camera-snapped LOD layers the sim cascades (foam/flow/depth/clip/
    // albedo) and the displacement reach toward the horizon. Higher covers more
    // distance at the cost of fill. Clamped to the engine ceiling
    // (kMaxOceanLodCascades) and to each sim's allocated layer count.
    uint32 LodCount = 7;

    // Texel resolution (per axis) of the snapped sim cascades (foam/flow/dyn-waves/
    // seabed/clip/albedo). A change resizes those cascade textures at runtime.
    // Default matches the engine's built-in cascade resolution; raise (e.g. 384,
    // 512) for sharper near-camera foam/flow detail at more VRAM.
    uint32 LodDataResolution = 256;

    // World extent (meters) of the finest (LOD 0) cascade. Each higher LOD doubles
    // it; with the engine's cascade layer count this covers out to MinScale*2^(N-1).
    // SMALLER MinScale concentrates resolution nearer the camera (less coverage).
    // MaxScale caps the coarsest layer (trims the LOD count so the coarsest cascade
    // stays <= MaxScale). Defaults match the engine's built-in cascade scale.
    float32 MinScale = 64.0f;
    float32 MaxScale = 512.0f;

    // Surface geometry budget for the camera-following concentric LOD rings. The
    // default 2x maps closely to 50 cells per tile and resolves noticeably more
    // near-camera wave shape. Combine with GeometryDownSampleFactor to trade
    // detail for fewer triangles; the compatible budget is
    // 256 * UpSample / DownSample and is clamped to [8, 1024].
    uint32 GeometryUpSampleFactor = 2;

    // Surface geometry budget divider. 1 keeps the upsampled density; 2 quarters
    // the triangle count, 4 sixteenths it, and so on.
    uint32 GeometryDownSampleFactor = 1;

    // Scales the dispersion gravity fed to every wave sim (FFT + dynamic waves),
    // so a project can make the whole sea feel heavier/slower (<1) or lighter and
    // faster (>1) without re-authoring per-octave gravity.
    float32 GravityMultiplier = 1.0f;

    // Time provider controls for deterministic preview/bakes and slow-motion water.
    // TimeScale multiplies the engine's accumulated ocean time and the ocean sim
    // delta time. 0 freezes animated waves, foam decay/advection, and dynamic-wave
    // propagation. TimeOffset shifts the animated phase without changing speed.
    float32 TimeScale = 1.0f;
    float32 TimeOffset = 0.0f;

    // Time-provider routing. Custom is the backward-compatible default and maps
    // TimeScale/TimeOffset/UseFixedTime below. Default follows engine time,
    // NetworkOffset adds a synchronized clock offset/rate, Paused holds
    // PausedTime, and Timeline consumes an externally-authored timeline value.
    OceanTimeProviderMode TimeProvider = OceanTimeProviderMode::Custom;
    float32 NetworkTimeOffset = 0.0f;
    float32 NetworkTimeRate = 1.0f;
    float32 TimelineTime = 0.0f;
    float32 TimelinePlaybackRate = 1.0f;
    bool TimelinePlaying = false;
    float32 PausedTime = 0.0f;

    // When enabled, FixedTime is used as the absolute ocean animation time and the
    // per-frame ocean sim delta is forced to 0. This holds FFT waves, wave masks,
    // foam, and dynamic-wave state at a stable phase for capture.
    bool UseFixedTime = false;
    float32 FixedTime = 0.0f;

    // Optional runtime budget for enabled OceanDepthCacheSource components. 0 keeps
    // the previous behavior (all eligible caches are composed). Values >0 keep the
    // highest-priority nearest caches around the active ocean camera and evict the
    // rest from the composed runtime texture until the camera approaches them.
    uint32 MaxActiveDepthCaches = 0u;

    // Optional runtime hidden top-down mesh raster capture for dynamic geometry
    // seabed depth. Off by default because it renders a secondary orthographic
    // world pass. When enabled, regular MeshRenderer geometry matching
    // RasterDepthCaptureRenderLayerMask is captured into a one-frame-latent R32F
    // water-depth texture and merged with saved caches, analytic seabeds, and live
    // contributors by the seabed-depth compute.
    bool RasterDepthCapture = false;
    uint32 RasterDepthCaptureResolution = 512u;
    uint32 RasterDepthCaptureRenderLayerMask = 0xFFFFFFFFu;
    float32 RasterDepthCaptureSizeX = 512.0f;
    float32 RasterDepthCaptureSizeZ = 512.0f;
    float32 RasterDepthCaptureTopPadding = 64.0f;
    float32 RasterDepthCaptureDeepWaterDepth = 60000.0f;

    // --- Global wind (drives the spectrum when authored here) ---
    // The reference renderer exposes a single global wind that every spectrum
    // reads. GlobalWindSpeed (m/s) and GlobalWindDirection (degrees, 0 = +X) feed
    // the FFT spectrum; GlobalWindTurbulence is the directional spread
    // (0 = waves all along the wind, higher = a more chaotic, spread sea). These
    // override the OceanWaveSpectrum's own wind when this renderer is present, so
    // wind lives in one place. Set GlobalWindSpeed to 0 for a flat calm sea.
    float32 GlobalWindSpeed = 8.0f;
    float32 GlobalWindDirection = 30.0f;
    float32 GlobalWindTurbulence = 0.145f;

    // Optional asset-backed global wave spectrum. When assigned it replaces the
    // first scene OceanWaveSpectrum component as the FFT base spectrum,
    // while the GlobalWind* controls above may still override wind if enabled.
    OceanWaveSpectrumRef SpectrumAsset;

    // Collision provider routing: 0 Auto, 1 GPU queries, 2 baked FFT CPU,
    // 3 analytic Gerstner, 4 None. Auto prefers completed GPU/combined-surface
    // data, then a compatible .oceanfft bake, then Gerstner.
    uint32 CollisionProvider = 0u;
    OceanFFTCollisionRef FFTCollisionAsset;
    uint32 MaxCollisionQueryCount = 8192u;
    OceanSettingsRef AnimatedWavesCollisionSettings;
    OceanSettingsRef DynamicWaveSettings;
    OceanSettingsRef FoamSettings;
    OceanSettingsRef ShadowSettings;

    // Performance reserve: combine the 16 tileable FFT displacement cascades into a
    // single cascade once per frame. The current viewer-centered implementation is
    // bypassed for visible waves/foam until it can be made world-stable; the direct
    // 16-cascade path remains authoritative.
    bool CombineDisplacementCascade = false;
};

// Identifies one scene-authored field by its full preset path, including the
// component prefix ("Surface.FoamRoughness"). Zero marks an unused entry.
// @ge-no-add
struct OceanPresetOverride
{
    StringId FieldIdentifier = 0;
};

// Room for an override on every preset-driven field (212 today) with headroom for new ones.
inline constexpr uint32 kOceanPresetOverrideCapacity = 512u;

// Live preset binding shared by renderer/surface/spectrum/water-body components.
// Fields absent from Overrides continue to track the live preset.
struct OceanPresetBinding
{
    OceanPresetRef Preset;
    bool LiveLinked = true;
    OceanPresetOverride Overrides[kOceanPresetOverrideCapacity] = {};
};

// Defines a bounded rectangular water body using the existing ocean cascades. This
// is the first water-body authoring layer: lakes, pools, reservoirs, and broad
// river segments can restore water only inside their footprint, optionally add a
// body-wide current, and contribute a matching underwater volume. The underlying
// surface is still the shared ocean renderer; this component authors where it is
// allowed to exist.
struct OceanWaterBody
{
    // Half-extent (meters) of the water body footprint on X and Z, centered on the
    // entity origin. 0 or negative disables the body.
    // @ge-tooltip Half the body's width in meters along world X, measured from the entity origin; the water spans twice this.
    float32 ExtentX = 100.0f;
    // @ge-tooltip Half the body's length in meters along world Z, measured from the entity origin; the water spans twice this.
    float32 ExtentZ = 100.0f;

    // Clip the ocean everywhere by default and restore it inside this body's
    // footprint. Uses the ocean clip cascade, so OceanSurface.ClipSurface does not
    // need to be authored manually when this is enabled.
    // @ge-tooltip Draw ocean water only inside this body. The coarsest clip level covers all authored water; nearer levels refine the shoreline.
    bool ConfineSurface = true;
    // Inward clip transition in meters. 0 keeps the legacy normalized rectangular
    // edge smoothing.
    float32 ClipFeather = 0.0f;

    // Add a box-shaped underwater volume matching the body footprint. The water
    // surface is the entity origin Y; the volume extends downward by UnderwaterDepth.
    bool UnderwaterVolume = true;
    float32 UnderwaterDepth = 20.0f;

    // Optional body-wide current (meters/second in world XZ). Uses the existing flow
    // cascade and is still quality-gated by OceanRenderer.
    bool Flow = false;
    float32 FlowX = 0.0f;
    float32 FlowZ = 0.0f;

    // Optional per-body wave override. WaveWeight scales vertical wave energy
    // (0 calm, 1 global spectrum), WaveChop scales horizontal crest sharpness.
    bool WaveOverride = false;
    float32 WaveWeight = 0.35f;
    float32 WaveChop = 0.35f;
    float32 WaveFeather = 10.0f;
    // When this entity also has an OceanWaveSpectrum, derive WaveWeight, WaveChop,
    // and the compact local wave packet from that spectrum instead of the scalar
    // LocalWave fields below. When a local FFT stream is available, spectrum bodies
    // can also blend to that stream; otherwise the compact packet remains active.
    bool UseLocalSpectrum = false;
    // Optional asset-backed local spectrum. When assigned it takes precedence over
    // the same-entity OceanWaveSpectrum component, letting multiple bounded bodies
    // share one authored spectrum asset.
    OceanWaveSpectrumRef LocalSpectrumAsset;
    // LocalWaveCount includes the legacy scalar wave below as slot 0. Additional
    // slots come from the Extra arrays and let small water bodies carry a compact
    // authored wave packet without a separate FFT cascade.
    uint32 LocalWaveCount = 1u;
    float32 LocalWaveAmplitude = 0.0f;
    float32 LocalWaveWavelength = 12.0f;
    float32 LocalWaveDirectionDegrees = 0.0f;
    float32 LocalWaveExtraAmplitude[OceanWaterBodyExtraLocalWaveCapacity] = {};
    float32 LocalWaveExtraWavelength[OceanWaterBodyExtraLocalWaveCapacity] = {
        12.0f, 12.0f, 12.0f};
    float32 LocalWaveExtraDirectionDegrees[OceanWaterBodyExtraLocalWaveCapacity] = {};
    // Surface material preset: color, optical response, foam and detail textures.
    // @ge-tooltip Ocean preset whose surface values (colour, optics, foam, detail textures) apply inside this water body. Empty keeps the ocean's own surface.
    OceanPresetRef MaterialOverride;
    // @ge-tooltip Where water-body materials overlap, higher priorities apply over lower ones.
    int32 MaterialPriority = 0;
};

// Defines a polygon-filled bounded water body. The points are local-space X/Z
// vertices on the entity plane; WorldTransform turns them into world XZ. The clip
// cascade fills the polygon exactly, while CPU surface queries use the same
// polygon so physics/gameplay agree with the visible water. Underwater volume and
// body flow also use the polygon footprint.
struct OceanPolygonWaterBody
{
    uint32 PointCount = 4u;
    float32 PointX[OceanPolygonWaterBodyPointCapacity] = {
        -50.0f, 50.0f, 50.0f, -50.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float32 PointZ[OceanPolygonWaterBodyPointCapacity] = {
        -50.0f, -50.0f, 50.0f, 50.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    bool ConfineSurface = true;
    // Inward clip transition in meters along the polygon edge.
    float32 ClipFeather = 2.0f;
    bool UnderwaterVolume = true;
    float32 UnderwaterDepth = 20.0f;

    bool Flow = false;
    float32 FlowX = 0.0f;
    float32 FlowZ = 0.0f;

    bool WaveOverride = false;
    float32 WaveWeight = 0.35f;
    float32 WaveChop = 0.35f;
    float32 WaveFeather = 10.0f;
    // When this entity also has an OceanWaveSpectrum, derive WaveWeight, WaveChop,
    // and the compact local wave packet from that spectrum instead of the scalar
    // LocalWave fields below.
    bool UseLocalSpectrum = false;
    // Optional asset-backed local spectrum. When assigned it takes precedence over
    // the same-entity OceanWaveSpectrum component.
    OceanWaveSpectrumRef LocalSpectrumAsset;
    // LocalWaveCount includes the legacy scalar wave below as slot 0. Additional
    // slots come from the Extra arrays and let polygon water bodies carry a compact
    // authored wave packet without a separate FFT cascade.
    uint32 LocalWaveCount = 1u;
    float32 LocalWaveAmplitude = 0.0f;
    float32 LocalWaveWavelength = 12.0f;
    float32 LocalWaveDirectionDegrees = 0.0f;
    float32 LocalWaveExtraAmplitude[OceanWaterBodyExtraLocalWaveCapacity] = {};
    float32 LocalWaveExtraWavelength[OceanWaterBodyExtraLocalWaveCapacity] = {
        12.0f, 12.0f, 12.0f};
    float32 LocalWaveExtraDirectionDegrees[OceanWaterBodyExtraLocalWaveCapacity] = {};
    // Surface material preset: color, optical response, foam and detail textures.
    // @ge-tooltip Ocean preset whose surface values (colour, optics, foam, detail textures) apply inside this water body. Empty keeps the ocean's own surface.
    OceanPresetRef MaterialOverride;
    // @ge-tooltip Where water-body materials overlap, higher priorities apply over lower ones.
    int32 MaterialPriority = 0;
};

// Tags an entity as a clip source for the ocean's clip cascade. Inside a
// rectangular footprint centered on the entity origin, the surface clip state is
// set (cutting a hole or, with the ocean's DefaultClippingState at 1, restoring
// the water). The surface discards fragments where the sampled clip value exceeds
// 0.5. Place on an empty/marker entity; its world transform supplies the footprint
// center (XZ). Requires the ocean's ClipSurface toggle on. Inert when clipping is
// disabled or the bake never initialized.
struct OceanClipSource
{
    // Half-extent (meters) of the clip footprint on X and Z, centered on the
    // entity origin. Outside this rectangle the source contributes no clip.
    float32 ExtentX = 25.0f;
    float32 ExtentZ = 25.0f;

    // Clip value written inside the footprint (1 = clipped/hole, 0 = solid water).
    // With the ocean's DefaultClippingState at 0, a value of 1 cuts a hole; with
    // the default at 1, a value of 0 restores the water inside the footprint.
    float32 ClipState = 1.0f;
    // Inward clip transition in meters. 0 keeps the legacy normalized rectangular
    // edge smoothing.
    float32 Feather = 0.0f;
};

// Tags an entity as an albedo source for the ocean's albedo cascade. Inside a
// rectangular footprint centered on the entity origin, the surface albedo is
// painted toward Color by Coverage. Overlapping sources composite (over) by their
// coverage. Drives surface decals / paint. Place on an empty/marker entity; its
// world transform supplies the footprint center (XZ). Requires the ocean's Albedo
// toggle on. Inert when albedo is disabled or the bake never initialized.
struct OceanAlbedoSource
{
    // Half-extent (meters) of the albedo footprint on X and Z, centered on the
    // entity origin. Outside this rectangle the source contributes no paint.
    float32 ExtentX = 25.0f;
    float32 ExtentZ = 25.0f;

    // Albedo colour painted inside the footprint (linear RGB), shown as a swatch
    // with a picker in the inspector.
    ColorLinear Color{0.8f, 0.1f, 0.1f, 1.0f};

    // Blend weight of the paint (0 = no override, 1 = fully replace the water
    // colour). The colour's own alpha is ignored; this is the coverage knob.
    float32 Coverage = 1.0f;
};

// Tags an entity as a flow source for the ocean's flow cascade. Inside a
// rectangular footprint centered on the entity origin, the flow field is set to
// a constant horizontal current (meters/second). Overlapping sources accumulate.
// Drives foam advection (foam drifts along the current) and detail-UV scroll.
// Place on an empty/marker entity; its world transform supplies the footprint
// center (XZ). A whirlpool/river can use several of these or an OceanFlowMapSource.
struct OceanFlowSource
{
    // Half-extent (meters) of the flow footprint on X and Z, centered on the
    // entity origin. Outside this rectangle the source contributes no flow.
    float32 ExtentX = 50.0f;
    float32 ExtentZ = 50.0f;

    // Horizontal current velocity (meters/second) on world X and Z inside the
    // footprint. Positive FlowX pushes foam/ripples toward +X.
    float32 FlowX = 2.0f;
    float32 FlowZ = 0.0f;
};

// Tags an entity as a texture-driven flow source for the ocean's flow cascade.
// Inside a rectangular footprint centered on the entity origin, the assigned
// texture's red/green channels become a horizontal current: RG 0.5/0.5 is neutral,
// values below/above 0.5 push toward negative/positive world X/Z, then Strength
// scales the resulting velocity. This matches the reference flow-map authoring
// style while still layering with constant flow sources, water-body currents, and
// spline rivers. Use a linear/non-sRGB vector texture for exact velocities.
struct OceanFlowMapSource
{
    // Half-extent (meters) of the flow-map footprint on X and Z, centered on the
    // entity origin. The texture maps across this rectangle once.
    float32 ExtentX = 50.0f;
    float32 ExtentZ = 50.0f;

    // Texture RG encodes flow direction and magnitude around neutral 0.5.
    // Empty = no contribution.
    TextureRef FlowMap;

    // Velocity multiplier in meters/second for the decoded RG vector.
    float32 Strength = 3.0f;

    // Optional inward fade distance in meters, useful when the texture edge should
    // blend into neighboring analytic currents instead of ending abruptly.
    float32 Feather = 0.0f;

    // Optional constant world-space velocity added to the decoded texture vector.
    float32 BiasX = 0.0f;
    float32 BiasZ = 0.0f;
};

// Tags an entity as a texture-authored wave mask for the ocean's wave-mask
// cascade. Inside a rectangular footprint centered on the entity origin, the
// assigned texture's red channel becomes wave weight/amplitude (0 = calm, 1 =
// normal/global waves), and green becomes horizontal chop scale. Use this for
// painted calm water under docks, rough wind patches, or authored river/lake
// transition zones. It layers with water-body wave boxes and polygons.
struct OceanWaveMaskTextureSource
{
    // Half-extent (meters) of the wave-mask footprint on X and Z, centered on the
    // entity origin. The texture maps across this rectangle once.
    float32 ExtentX = 50.0f;
    float32 ExtentZ = 50.0f;

    // Texture RG encodes wave weight and chop. Empty = no contribution.
    TextureRef WaveMaskMap;

    // Per-channel shaping applied as target = texture.rg * scale + bias.
    float32 WeightScale = 1.0f;
    float32 ChopScale = 1.0f;
    float32 WeightBias = 0.0f;
    float32 ChopBias = 0.0f;

    // Blend amount for the authored texture and optional inward fade distance in
    // meters, so painted masks can soften into the surrounding global spectrum.
    float32 Coverage = 1.0f;
    float32 Feather = 0.0f;
};

// Continuous triangle ribbons rasterized into flow, depth, clip and albedo fields.
// MaxSegmentLength sets the tessellation along the curve. The ribbon uses its own
// tile-binned GPU buffer, independent of point-source caps.
struct OceanSplineInput
{
    // Half-width (meters) of the continuous river / band ribbon.
    float32 Width = 20.0f;

    // Water body: clip the ocean everywhere by default and restore it inside this
    // spline band, making the spline a primary river/canal/pool boundary instead
    // of only a visual modifier. CPU surface queries are constrained to the same
    // tessellated band.
    // @ge-tooltip Draw ocean water only inside this spline band. The coarsest clip level covers all authored water; nearer levels refine the shoreline.
    bool ConfineSurface = false;
    // Width in meters of the clip and paint transition, centered on the ribbon
    // edge, and of the inward current fade. Zero uses ten percent of the ribbon
    // width; the edge transition is never narrower than two cascade texels.
    float32 ClipFeather = 0.0f;

    // Extrude the continuous ribbon into an underwater volume. Its top
    // follows the sampled world Y; the volume extends downward by
    // UnderwaterDepth. Intended for fly-through river/canal bodies.
    bool UnderwaterVolume = false;
    float32 UnderwaterDepth = 20.0f;

    // Flow (river): set the flow field along the curve to FlowSpeed (m/s) in the
    // spline tangent direction, so foam + surface detail drift downstream. Needs
    // the ocean's Flow toggle.
    bool Flow = true;
    float32 FlowSpeed = 3.0f;
    bool ReverseFlow = false; // flip the current to run against the tangent

    // Clip: cut the surface along the curve (a canal / channel). Needs ClipSurface.
    bool Clip = false;

    // Albedo: paint the surface along the curve toward Color by Coverage (a visible
    // foam line / riverbed tint). Needs the ocean's Albedo toggle.
    bool Albedo = false;
    ColorLinear Color{0.85f, 0.88f, 0.95f, 1.0f};
    float32 AlbedoCoverage = 0.8f;

    // Depth: rasterize a live shallow/depth band into the seabed-depth cascade. This
    // affects shallow colour, shoreline foam, shallow-wave attenuation, and CPU
    // depth queries without baking a permanent .oceandepth asset. Use for moving
    // river/channel authoring or temporary dredged paths.
    bool Depth = false;
    float32 DepthMeters = 1.5f;
    float32 DepthFeather = 1.0f;
    // Longest ribbon segment, in spline-space meters: the curve is sampled at
    // least this densely along its arc length.
    // @ge-tooltip Longest ribbon segment in meters. Smaller follows tight curves more closely at the cost of more triangles.
    float32 MaxSegmentLength = 2.0f;
};

// Confines the underwater overlay to a bounded water volume — an axis-aligned box
// centered on the entity origin (Crest's volume / fly-through underwater mode).
// With at least one enabled volume in the scene the underwater look switches from
// the infinite SeaLevel plane to "inside a volume": the camera is submerged only
// when it sits inside a box, and the overlay's fog depth is measured down from
// that box's top. Use for pools / lakes / aquaria that shouldn't flood the whole
// world. Requires the ocean's Underwater toggle. Place on an entity; its world
// position is the box center. Inert when the Underwater toggle is off.
//
// The fullscreen overlay uses the box as both a fly-through underwater volume and
// a screen-space portal mask, so looking into the volume from outside shows the
// submerged fog/tint only through the volume silhouette.
struct OceanUnderwaterVolume
{
    // Half-extents (meters) of the box on world X / Y / Z, centered on the entity
    // origin. The water surface inside the volume is the box top (origin.y + ExtentY).
    float32 ExtentX = 30.0f;
    float32 ExtentY = 10.0f;
    float32 ExtentZ = 30.0f;
};

// Suppresses the underwater overlay inside a box, even when the camera is below
// the global sea plane or inside an OceanUnderwaterVolume. Use for dry interiors,
// portal volumes, submarines, tunnels, or aquarium glass spaces that should not
// inherit the ocean fog/tint. Place on an entity; its world position is the box
// center. Inert when the ocean's Underwater toggle is off.
struct OceanUnderwaterExclusionVolume
{
    float32 ExtentX = 10.0f;
    float32 ExtentY = 10.0f;
    float32 ExtentZ = 10.0f;
};

// Opt-in depth-ordering proxy for above-water underwater portal silhouettes.
// Use this on transparent glass panes, windows, or other alpha-blended geometry
// that should stay in front of an aquarium/lake portal even though it does not
// write normal scene depth. It does not affect camera submersion or dry volumes;
// it only caps the water segment in the portal post pass. Place on an entity; its
// world position is the box center.
struct OceanUnderwaterPortalOccluder
{
    float32 ExtentX = 10.0f;
    float32 ExtentY = 10.0f;
    float32 ExtentZ = 0.25f;
};

// Injects an impulse into the ocean's dynamic-wave simulation each frame: a
// localized vertical displacement (a dent or bump) at the entity origin that the
// wave-equation sim then propagates as spreading, decaying ripples. Use for a
// buoyant body's wake, a splash, or a dropped object. The injection is additive
// every frame the component is enabled, so a moving source leaves a trail; pulse
// it (enable for one frame) for a single expanding ring. Requires the ocean's
// DynamicWaves toggle on. Place on the body; its world transform supplies the
// impact XZ. Inert when no dynamic-wave sim is running.
struct OceanWaveImpulse
{
    // Radius (meters) of the injected disturbance footprint, centered on the
    // entity origin. Larger spreads the impulse over more texels (a broad swell);
    // smaller concentrates it (a sharp splash).
    float32 Radius = 1.5f;

    // Vertical displacement (meters) deposited at the impact center each frame,
    // falling off to zero at Radius. Positive lifts the water (a bump); negative
    // dents it (an impact crater the surrounding water rushes to fill).
    float32 Amplitude = -0.5f;
};

// Tags a moving body so it disturbs the water as it travels — the reference's
// SphereWaterInteraction. Each frame the extraction measures the entity's
// horizontal and vertical speed (from its world position delta) and injects a
// velocity-scaled dynamic-wave impulse at its location, so boats/objects leave
// wakes and water-entry/exit splashes automatically (no per-frame OceanWaveImpulse
// toggling). Needs the OceanSurface DynamicWaves toggle on. A still body injects
// nothing.
struct OceanWaterInteraction
{
    // Footprint (meters) of the disturbance the body pushes into the water.
    float32 Radius = 2.0f;

    // Wake strength: vertical displacement per (meter/second) of horizontal speed.
    // The injected dent scales with speed, so faster bodies carve deeper wakes.
    float32 Strength = 0.15f;

    // Speed (m/s) below which no wake is injected (a near-still body leaves the
    // water calm rather than dribbling tiny ripples from numerical jitter).
    float32 MinSpeed = 0.5f;

    // Vertical motion strength: signed displacement per (meter/second) of vertical
    // speed. Downward motion dents the water; upward motion lifts it. Set 0 to
    // keep the legacy horizontal-wake-only behavior.
    float32 VerticalStrength = 0.10f;
    float32 MinVerticalSpeed = 0.5f;

    // Absolute clamp on the auto-generated impulse amplitude, preventing teleports
    // or one-frame spawn corrections from injecting extreme waves.
    float32 MaxAmplitude = 2.0f;

    // Nested radial impulses approximate a sphere's broad pressure field without
    // requiring a mesh raster. The outer sphere uses Radius; each inner sphere
    // shrinks and scales its contribution by these factors.
    uint32 NestedSphereCount = 2u;
    float32 NestedRadiusScale = 0.5f;
    float32 NestedWeight = 0.45f;

    // Derive wake speed relative to authored currents and the sampled wave motion,
    // then lead the injection center along that relative velocity. These controls
    // prevent a drifting object from generating a wake when it is stationary in
    // the moving water frame.
    bool FlowRelativeVelocity = true;
    float32 VelocityLead = 0.12f;
    float32 WaveMotionCompensation = 1.0f;
    float32 SpeedClamp = 35.0f;

    // Movement longer than this in one frame is treated as a teleport and only
    // reseeds history. LargeWaveBoost adds pressure when the ambient surface is
    // already strongly displaced. DebugSubsteps distributes the impulse along the
    // swept path for low frame-rate inspection (1 is the normal production path).
    float32 TeleportDistance = 25.0f;
    float32 LargeWaveBoost = 0.25f;
    uint32 DebugSubsteps = 1u;
};

// Wind-driven wave spectrum. Higher wind speed → bigger, longer waves. Drives
// the FFT spectrum (the reference parity) and small legacy/local wave packets.
struct OceanWaveSpectrum
{
    // Sustained wind speed (m/s). 0 is flat calm; typical: 2 (calm) .. 20 (storm).
    float32 WindSpeed = 8.0f;

    // Wind heading in degrees (0 = +X, 90 = +Z).
    float32 WindDirectionDegrees = 30.0f;

    // --- FFT spectrum (the reference parity) ---
    // Directional spreading of the FFT spectrum (the reference WindTurbulence). 0 = all
    // waves travel along the wind, higher = more spread/chaotic sea.
    float32 Turbulence = 0.145f;

    // Overall wave energy multiplier (the reference _multiplier). Scales the per-octave
    // power curve; the dominant knob for calm vs. rough seas.
    float32 Multiplier = 1.0f;

    // Horizontal displacement / choppiness (the reference _chop). 0 = round swells,
    // higher sharpens crests and flattens troughs.
    float32 Chop = 1.6f;

    // Wave-speed multiplier (the reference _gravityScale). >1 = faster, more energetic
    // waves; scales the simulation time fed to the dispersion.
    float32 GravityScale = 1.0f;

    // Time-loop period in seconds for the FFT (0 = no looping). Used for baked
    // collision / deterministic playback.
    float32 LoopPeriod = 0.0f;

    // --- Per-octave spectrum shaping (FFT) ---
    // The FFT spectrum is built from 14 wavelength octaves (smallest ~6 cm ripple
    // to ~16 km swell). These raw arrays let a project sculpt the wave energy
    // band-by-band: SpectrumPower scales each octave's amplitude (1 = the default
    // wind curve, 0 = silence that band, >1 = boost it); ChopScales sets the
    // per-octave horizontal choppiness (multiplied by the global Chop above);
    // GravityScales tunes each octave's wave speed (1 = physical). The inspector
    // shows these as flat 14-element arrays (no custom curve widget yet), so leave
    // them at their identity defaults for the standard look. OctaveDisabled mutes
    // an octave entirely (true = no energy in that band), a quick way to drop the
    // longest swells or the smallest ripples without zeroing the power.
    float32 SpectrumPower[14] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float32 ChopScales[14] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                              1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float32 GravityScales[14] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    bool OctaveDisabled[14] = {false, false, false, false, false, false, false,
                               false, false, false, false, false, false, false};

    // --- Legacy local wave-packet controls ---
    // Directional spreading for bounded water-body wave packets: 0 = along wind,
    // 1 = spread.
    float32 DirectionalSpread = 0.35f;

    // Overall amplitude scale for generated local wave packets.
    float32 AmplitudeScale = 1.0f;

    // Largest generated wavelength (meters); sets the coarsest local packet wave.
    float32 MaxWavelength = 250.0f;

    // --- Shape FFT controls (reference parity) ---
    // Overall contribution weight of the wave shape. Scales the whole displacement
    // field (geometry + normals): 0 = flat water, 1 = full waves.
    float32 Weight = 1.0f;

    // Clamp on the maximum horizontal displacement (meters) a surface point is
    // pushed from rest by choppy waves. Limits the crest pinch so the choppy
    // displacement can't fold the grid or open gaps at the screen edges.
    float32 MaxHorizontalDisplacement = 15.0f;

    // Clamp on the maximum vertical displacement (meters) of the wave height.
    float32 MaxVerticalDisplacement = 10.0f;

    // How strongly waves are flattened in shallow water by the sea-floor depth
    // cascade. 0 = waves ignore depth; 1 = fully flattened toward the shoreline.
    // Inert (no attenuation) when no seabed is present.
    float32 RespectShallowWaterAttenuation = 1.0f;
};

// Tags an entity as seabed for the ocean's sea-floor depth cascade. The ocean
// bakes a top-down depth map (sea-level-to-floor distance) per camera-snapped LOD
// from every tagged seabed, which drives the shallow-water colour and shoreline
// foam. The seabed surface is described analytically as a plane so the bake needs
// no geometry render pass: a base height at the entity origin, sloped by a
// gradient over the XZ plane, evaluated only inside a rectangular footprint.
// Place this on the terrain/seabed entity (its world transform supplies the
// origin XZ + base height). A flat seabed leaves the slope at zero.
struct OceanSeabed
{
    // Half-extent (meters) of the seabed footprint on X and Z, centered on the
    // entity origin. Outside this rectangle the seabed contributes no depth
    // sample (the texel keeps deep water). 0 or negative disables the footprint.
    float32 ExtentX = 500.0f;
    float32 ExtentZ = 500.0f;

    // Seabed height (meters, world Y) at the entity origin. Depth at a texel is
    // SeaLevel - seabedHeight, so a floor below sea level gives positive depth
    // (shallows fade in as it rises toward the waterline).
    float32 BaseHeight = -8.0f;

    // Linear slope of the seabed height across the footprint (meters of height
    // per meter of world distance) on X and Z. Lets a single tagged plane model
    // a beach that rises toward shore: positive SlopeX raises the floor toward +X.
    float32 SlopeX = 0.0f;
    float32 SlopeZ = 0.0f;
};

// Points the ocean at a saved depth cache file. The cache is a world-space grid
// of positive water depths below calm sea level; it is loaded once and sampled by
// the seabed-depth bake before analytic seabeds and dynamic contributors are
// applied. Use this for authored/baked terrain depth that should survive without
// recapturing geometry every frame. Multiple enabled sources are composed into
// one runtime cache over their union bounds; overlaps use the shallowest depth.
struct OceanDepthCacheSource
{
    bool UseWhenStale = true;

    // Path to a binary OceanDepthCacheAsset file, usually relative to the project
    // working directory (for example Assets/Ocean/Harbor.oceandepth).
    char Path[OceanDepthCachePathCapacity] = {};
    // Optional typed asset reference to a .oceandepth asset. When set, runtime
    // loading resolves this GUID to its registered path and uses it instead of the
    // raw Path string, so saved depth caches can be assigned from the asset picker.
    OceanDepthCacheRef CacheAsset;

    // Dependency revision is an authoring/tool hook: increment SourceRevision when
    // terrain/mesh inputs that feed this cache change. The editor bake command
    // captures MeshRenderer triangles plus terrain heightfields, copies
    // SourceRevision into BakedRevision, and bumps CacheRevision after it writes
    // Path/CacheAsset. Runtime loading treats all three revisions as part of the
    // cache key, so a same-path rewrite can force reload even on coarse file
    // systems.
    uint32 SourceRevision = 0u;
    uint32 BakedRevision = 0u;
    uint32 CacheRevision = 0u;

    // Authoring settings used by the editor/tool rebake command. Runtime loading
    // only needs Path; these fields describe how to regenerate that cache from
    // scene geometry when requested.
    uint32 BakeWidth = 512u;
    uint32 BakeHeight = 512u;
    uint32 RenderLayerMask = 0xFFFFFFFFu;
    // When true, the editor/engine scene-mesh bake only captures MeshRenderer
    // entities that also have OceanMeshDepthContributor. This is the exact
    // triangle-cache companion to the runtime contributor's cheaper bounds stamp.
    bool BakeOnlyMeshDepthContributors = false;
    bool BakeLoadMissingAssets = true;
    bool BakeIncludeDisabledRenderers = false;
    bool BakeIncludeSkinnedMeshes = false;
    // Include Terrain/TiledTerrain heightfields in the editor scene bake. Mesh
    // triangles and terrain heightfields are composed into one cache; overlaps use
    // the shallowest sampled depth.
    bool BakeIncludeTerrainHeightfields = true;
    float32 BakeOriginX = -512.0f;
    float32 BakeOriginZ = -512.0f;
    float32 BakeSizeX = 1024.0f;
    float32 BakeSizeZ = 1024.0f;
    float32 BakeSeaLevel = 0.0f;
    float32 BakeDeepWaterDepth = 60000.0f;

    // Runtime streaming controls. StreamRadius <= 0 keeps this cache always
    // eligible. Values >0 load it only while the active ocean camera is within
    // StreamRadius meters of the bake rectangle. StreamPriority breaks ties when
    // OceanRenderer.MaxActiveDepthCaches trims the active set (higher wins).
    float32 StreamRadius = 0.0f;
    uint32 StreamPriority = 0u;

    // Path up to its first null byte; a full buffer may hold none.
    std::string_view GetPath() const { return FixedStringView(Path); }
};

// Dynamic analytic depth contributor for the ocean's sea-floor depth cascade. This
// is for moving/temporary objects that should affect shallow-water colour,
// shoreline foam, and shallow-wave attenuation without becoming permanent seabed:
// hulls, floating platforms, temporary sandbars, gates, or gameplay blockers.
// Place on an entity; its world transform supplies the footprint center (XZ).
// Unlike intersection foam, this writes into the ocean depth cache, so it affects
// every depth-driven ocean feature that samples that cache.
struct OceanDepthContributor
{
    // Half-extent (meters) of the contributor footprint on X and Z, centered on the
    // entity origin. 0 or negative disables the footprint.
    float32 ExtentX = 5.0f;
    float32 ExtentZ = 5.0f;

    // Depth below calm sea level (meters) contributed inside the footprint. Smaller
    // values are shallower. For a hull just under the surface, use ~0.2..2 m.
    float32 Depth = 1.0f;

    // Edge feather in meters. 0 is a hard stamp; higher values fade the contributor
    // out near the footprint edge so foam/shallow colour transition smoothly.
    float32 Feather = 0.5f;

    // 0 = rectangular footprint, 1 = elliptical footprint. Intermediate values
    // blend toward the ellipse test in the shader; use 1 for rounded hull/object
    // occlusion and 0 for dock/platform-like shapes.
    float32 Roundness = 1.0f;
};

// Mesh/bounds-backed live depth contributor. Place this on a rendered moving
// object with LocalBounds + WorldTransform (usually the same entity as a
// MeshRenderer). Each frame the ocean projects the transformed local bounds into
// the seabed-depth cascade as a shallow rounded footprint. This is cheaper than
// rendering triangles into the depth cache and works well for boats, docks,
// platforms, gates, and other moving geometry where an AABB/ellipse approximation
// is acceptable. For exact static geometry, bake an OceanDepthCacheSource instead.
struct OceanMeshDepthContributor
{
    // Only contribute when the optional MeshRenderer's renderLayerMask overlaps
    // this mask. Ignored when the entity has no MeshRenderer.
    uint32 RenderLayerMask = 0xFFFFFFFFu;

    // Extra footprint padding in meters around the projected world-space bounds.
    float32 ExtentPadding = 0.0f;

    // Added to the computed depth (SeaLevel - bounds top). Positive values push
    // the contribution deeper, useful for hulls whose visual top sits above water.
    float32 DepthBias = 0.25f;

    // Clamp on the contributed depth. MinDepth keeps above-water/straddling
    // objects from writing exactly zero unless desired; MaxDepth avoids one bad
    // bound making a huge deep-water stamp.
    float32 MinDepth = 0.05f;
    float32 MaxDepth = 100.0f;

    // Edge feather and rectangle/ellipse blend, matching OceanDepthContributor.
    float32 Feather = 0.5f;
    float32 Roundness = 0.75f;
};

// Makes a rigid body float on the ocean. The buoyancy system samples wave height
// at probe points around the body and applies upward force (+ righting torque)
// and drag through the physics force API. Requires a PhysicsBody on the entity.
struct OceanBuoyancy
{
    // Upward force per meter of submergence per unit mass. ~mass*Strength*depth.
    // The default roughly balances gravity at ~half submergence.
    float32 BuoyancyStrength = 20.0f;

    // Half-extent (meters) of the probe footprint around the body origin. Four
    // probes are placed at the corners, giving natural buoyancy + self-righting.
    float32 ProbeRadius = 0.5f;

    // Draft (meters): distance from the body origin down to its effective
    // underside. Submergence is measured at this depth, so the body floats with
    // visible freeboard (top above water) instead of flush at the surface. Set
    // to roughly the body's half-height. Equilibrium sits near ~Gravity/Strength
    // submerged, so the default Strength/Draft float a unit body about half out.
    float32 Draft = 0.5f;

    // Waterline trim (meters): raises (+) or lowers (-) the resting waterline so
    // the user can dial in exactly how much of the body sits in the water.
    // Positive floats it higher (more freeboard, less submerged); negative sinks
    // it deeper. Independent of mass/strength — a direct rest-height offset.
    float32 WaterLineOffset = 0.0f;

    // Velocity-proportional damping while submerged (scaled by submerged fraction).
    float32 LinearDrag = 1.0f;
    float32 AngularDrag = 1.0f;
};

// Typed ocean input components. All share the same ordering, blend, geometry,
// footprint, and feather contract represented by IOceanInputDrawSource at runtime.
// Priority is evaluated first and entity ID breaks ties deterministically.
enum class OceanInputBlend : uint32
{
    Replace = 0u,
    Additive = 1u,
    Multiply = 2u,
    Minimum = 3u,
    Maximum = 4u,
};

enum class OceanInputGeometryType : uint32
{
    Rectangle = 0u,
    EngineMesh = 1u,
    Line = 2u,
    Trail = 3u,
    Particle = 4u,
    SplineBand = 5u,
    CustomNative = 6u,
};

struct OceanAnimatedWaveInput
{
    int32 Priority = 0;
    OceanInputBlend Blend = OceanInputBlend::Additive;
    OceanInputGeometryType Geometry = OceanInputGeometryType::Rectangle;
    float32 ExtentX = 25.0f;
    float32 ExtentZ = 25.0f;
    float32 Feather = 2.0f;
    float32 Amplitude = 0.5f;
    float32 Wavelength = 12.0f;
    float32 DirectionDegrees = 0.0f;
    float32 Chop = 1.0f;
};

struct OceanHeightInput
{
    int32 Priority = 0;
    OceanInputBlend Blend = OceanInputBlend::Additive;
    OceanInputGeometryType Geometry = OceanInputGeometryType::Rectangle;
    float32 ExtentX = 25.0f;
    float32 ExtentZ = 25.0f;
    float32 Feather = 2.0f;
    float32 Height = 0.0f;
    bool AbsoluteHeight = false;
};

struct OceanFoamInput
{
    int32 Priority = 0;
    OceanInputBlend Blend = OceanInputBlend::Additive;
    OceanInputGeometryType Geometry = OceanInputGeometryType::Rectangle;
    float32 ExtentX = 25.0f;
    float32 ExtentZ = 25.0f;
    float32 Feather = 2.0f;
    float32 Amount = 1.0f;
    TextureRef Texture;
    bool UseVertexColor = false;
};

struct OceanDynamicWaveInput
{
    int32 Priority = 0;
    OceanInputBlend Blend = OceanInputBlend::Additive;
    OceanInputGeometryType Geometry = OceanInputGeometryType::Rectangle;
    float32 ExtentX = 2.0f;
    float32 ExtentZ = 2.0f;
    float32 Feather = 0.5f;
    float32 Amplitude = 0.5f;
};

struct OceanFlowInput
{
    int32 Priority = 0;
    OceanInputBlend Blend = OceanInputBlend::Additive;
    OceanInputGeometryType Geometry = OceanInputGeometryType::Rectangle;
    float32 ExtentX = 25.0f;
    float32 ExtentZ = 25.0f;
    float32 Feather = 2.0f;
    float32 FlowX = 0.0f;
    float32 FlowZ = 0.0f;
};

struct OceanClipInput
{
    int32 Priority = 0;
    OceanInputBlend Blend = OceanInputBlend::Replace;
    OceanInputGeometryType Geometry = OceanInputGeometryType::Rectangle;
    float32 ExtentX = 25.0f;
    float32 ExtentZ = 25.0f;
    float32 Feather = 2.0f;
    float32 ClipState = 1.0f;
};

struct OceanAlbedoInput
{
    int32 Priority = 0;
    OceanInputBlend Blend = OceanInputBlend::Replace;
    OceanInputGeometryType Geometry = OceanInputGeometryType::Rectangle;
    float32 ExtentX = 25.0f;
    float32 ExtentZ = 25.0f;
    float32 Feather = 2.0f;
    ColorLinear Color{0.8f, 0.1f, 0.1f, 1.0f};
    float32 Coverage = 1.0f;
};

struct OceanDepthInput
{
    int32 Priority = 0;
    OceanInputBlend Blend = OceanInputBlend::Minimum;
    OceanInputGeometryType Geometry = OceanInputGeometryType::Rectangle;
    float32 ExtentX = 25.0f;
    float32 ExtentZ = 25.0f;
    float32 Feather = 2.0f;
    float32 Depth = 2.0f;
    float32 Roundness = 0.0f;
};

struct OceanShadowInput
{
    int32 Priority = 0;
    OceanInputBlend Blend = OceanInputBlend::Replace;
    OceanInputGeometryType Geometry = OceanInputGeometryType::Rectangle;
    float32 ExtentX = 25.0f;
    float32 ExtentZ = 25.0f;
    float32 Feather = 2.0f;
    float32 HardShadow = 1.0f;
    float32 SoftShadow = 1.0f;
};

// Authored Gerstner packet that can be used as a shape input independent of the
// global FFT/Gerstner base mode. Slots beyond WaveCount are ignored.
inline constexpr uint32 OceanGerstnerShapeWaveCapacity = 8u;
struct OceanGerstnerShape
{
    int32 Priority = 0;
    float32 ExtentX = 100.0f;
    float32 ExtentZ = 100.0f;
    float32 Feather = 10.0f;
    uint32 WaveCount = 1u;
    float32 Amplitude[OceanGerstnerShapeWaveCapacity] = {0.5f};
    float32 Wavelength[OceanGerstnerShapeWaveCapacity] = {
        12.0f, 12.0f, 12.0f, 12.0f, 12.0f, 12.0f, 12.0f, 12.0f};
    float32 DirectionDegrees[OceanGerstnerShapeWaveCapacity] = {};
    float32 Chop[OceanGerstnerShapeWaveCapacity] = {
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
};

} // namespace GameEngine::Components
