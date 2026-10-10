#pragma once

#include "Components/AssetRef.h"
#include "Components/Terrain/Terrain.h" // TerrainDomain
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

inline constexpr uint32 kMinTerrainGrassBladeSegments = 1u;
inline constexpr uint32 kMaxTerrainGrassBladeSegments = 12u;
// smoothstep is undefined on an empty span: the root fade's end is held this far above its start.
inline constexpr float32 kMinTerrainGrassRootFadeSpan = 0.01f;
// The gradient may start below the root: a negative start puts part of the tip colour on the
// root itself. -1 is one blade height below it.
inline constexpr float32 kMinTerrainGrassRootFadeStart = -1.0f;

// How blades resolve their soft alpha, which is the card texture's alpha and nothing else — a
// blade BODY is opaque. Geometric grass binds no texture, so it has no soft alpha for either mode
// to resolve and both draw solid blades whose only shape is their geometry.
//
// Dither: alpha-test class. Blades write and test depth, so blade-over-blade occlusion is
// depth-correct regardless of draw order, and blades occlude water/transparents behind them.
// The soft alpha becomes screen-space dithered coverage: under MSAA, alpha-to-coverage turns it
// into subsample coverage; without MSAA it is a screen-door pattern (resolved smooth by TAA).
//
// Blend: classic alpha blending. A texture card's soft edges stay smooth without any AA, but
// blades write no depth and blade-over-blade compositing follows draw order, not camera distance —
// the far LOD band is drawn first so it can never composite over near blades, but blades within a
// band blend in placement order. Geometric grass gains nothing from this and still pays that
// ordering: at alpha 1.0 the placement order reads as an occlusion order rather than a blend.
//
// The mode is authored per component; grass instances from every terrain share one draw per LOD
// band, so a view whose active grass terrains disagree resolves to Dither (the depth-correct
// choice) for all of them.
enum class TerrainGrassRenderMode : uint8
{
    Dither = 0,
    Blend = 1,
};

// Grass placement is planar-only. The placement compute (terrain_grass_place.comp) lays blade
// candidates on a world-anchored XZ cell lattice inside a camera-relative window and roots each at
// y = WorldOriginY + height, with blade up = the world-Y-dominant terrain normal. On a cube-sphere
// planet (TerrainDomain::Spherical) the surface is dir*(R + relief + sculpt), so those same
// candidates float off the curved shell, adrift above the planet. Radial
// (on-sphere) placement is a separate arc (see the planet-grass design doc); until it lands a
// spherical terrain sources NO grass rather than floaters. One predicate so the extraction gate and
// its oracle can never disagree on which domains place grass.
inline constexpr bool TerrainGrassSupportsDomain(TerrainDomain domain)
{
    return domain == TerrainDomain::Planar;
}

// @ge-no-add  Requires a Terrain on the same entity (only read inside Query<Terrain,...>); inert
// standalone, so it's reflected (MCP/snapshot + its custom inspector) but kept out of the Add menu.
struct TerrainGrass
{
    TerrainGrassRenderMode RenderMode = TerrainGrassRenderMode::Dither;
    // Blades per square metre AT THE CAMERA, independent of terrain size: the same value reads the
    // same on a 512 m and a 10 km terrain. Density falls off to zero at Range along DensityFalloff,
    // and blades leave by not being spawned, so there is no fade band.
    //
    // This density and this Range together over-subscribe the per-view instance budget, so the fit
    // shortens the RANGE and holds the near density (GrassFitPlacementToBudget). That is the
    // intended trade: density at the camera is what a field's look is judged on, distance is what
    // pays for it.
    float32 BladesPerSquareMeter = 21.0f;
    float32 Range = 500.0f;
    float32 DensityFalloff = 2.0f;
    float32 PlacementSeed = 3.0f;
    // Voronoi clumping. Blades are laid out by a low-discrepancy sequence, which is anti-clump by
    // construction — a field of it reads as mown rather than grown. A second, coarser, world-
    // anchored lattice groups blades into tufts that share a height, a facing and a colour. Edge
    // of a clump in metres; 0 or less turns the whole mechanism off and restores the plain field.
    float32 ClumpSize = 1.1f;
    // How far a clump's height departs from the authored one, +/- this fraction. 0 = every clump
    // the same height as every other.
    float32 ClumpHeightVariance = 0.3f;
    // How far each blade's yaw is pulled toward its clump's common facing. 0 = independent yaw per
    // blade (the unclumped field), 1 = every blade in a tuft faces identically.
    float32 ClumpAlignment = 0.45f;
    // How far each blade is pulled toward its clump's centre. Gathers the tuft without thinning
    // the field: the blade count per square metre is unchanged, only where they stand.
    float32 ClumpGather = 0.25f;
    float32 BladeHeight = 0.72f;
    // Sits just under BladeHeight * MaxWidthRatio. At or above that product the aspect ceiling below
    // clamps every blade, and the width the component reports stops being the width it renders.
    float32 BladeWidth = 0.043f;
    // Aspect ceiling: a blade is never wider than BladeHeight * MaxWidthRatio, whatever BladeWidth
    // asks for. It exists so a short blade cannot become a square, not to set the shipped width —
    // BladeWidth is the width control and this only intervenes at extreme aspect ratios.
    float32 MaxWidthRatio = 0.06f;
    uint32 BladeSegments = 5u;
    float32 RandomScale = 0.35f;
    uint32 LayerIndex = 0;
    float32 MaskThreshold = 0.45f;
    float32 WindDirection = -2.5f;
    float32 WindGustSpeed = 0.9f;
    float32 WindGustScale = 0.05f;
    float32 WindStrength = 1.0f;
    float32 WindRestingLean = 0.12f;
    float32 WindFlutterAmount = 0.16f;
    float32 WindFlutterSpeed = 2.2f;
    float32 WindSeed = 3.0f;
    float32 Brightness = 1.0f;
    // Per-blade value jitter: a blade's canopy is scaled by 1 +/- this. It carries the field's
    // blade-to-blade TONAL separation, which is what a noon field with the sun behind the camera has
    // left to read by: there the ground and the grass are lit almost identically and the root is the
    // terrain's own colour by construction, so a blade's own value is all that distinguishes it from
    // its neighbour.
    //
    // The grain is the BLADE, not the pixel, so raising this cannot make pixel noise — it darkens
    // the darkest blades, smoothly and with no threshold anywhere in the range. The ceiling is
    // therefore taste rather than a defect boundary, and the trade to weigh is field variety
    // against how dark the darkest blade may be.
    //
    // TRAP: it is driven by the instance's WIDTH random, unrehashed. The placement compute writes
    // one random per blade and the blade's width is a function of it (terrain_grass_place.comp,
    // blade.Rand = s.WidthRand, and the width's mix(0.65, 1.25, s.WidthRand)); the surface reads
    // that value as `tint` and scales the canopy by it directly. So per-blade brightness is a
    // MONOTONE FUNCTION OF BLADE WIDTH — at this default the widest blade in a field is always the
    // brightest (x1.42) and the narrowest always the dimmest (x0.58). That is the same coupling the
    // hue draw below deliberately breaks by rehashing (grassHash11(tint + 41.0)), for the same
    // reason. Decoupling this one is a look change and needs its own measurement, so it is a
    // follow-up rather than a silent fix; until then, raising this dial also strengthens the
    // width-brightness correlation.
    float32 RandomBrightness = 0.42f;
    // Hue jitter, in [0,1] where 1 is a 30-degree swing either way between whole tufts. Value-only
    // variation (the two Brightness knobs above) makes a field read as one colour lit unevenly; a
    // little hue spread is what makes it read as many plants. Blades inside a tuft break up against
    // each other with a fixed share of the same amount (kGrassBladeHueShare in grass_clump.glsl),
    // so this is the one dial for "how many different greens is this field", and the two
    // granularities sum: at 0.45 a tuft moves +/-13.5 degrees and a blade inside it +/-18.9.
    //
    // NEARLY free in brightness, not exactly: GrassHueShift rotates about (1,1,1)/sqrt(3), which
    // preserves R+G+B and the vector's length but NOT Rec.709 luma, so a rotation does move a
    // blade's value a little (a few 8-bit levels at this default on the shipped tip colour), and
    // the negative-albedo clamp after it is a further one-way change at large angles. Its ceiling
    // is nevertheless how much spread still reads as ONE plant rather than any brightness cost:
    // what grows with the dial is the spread between neighbouring blades, and far enough up that
    // spread reads as two species mixed rather than one sward. "Still a plausible green" is a
    // statement about the SHIPPED palette — a bound albedo texture multiplies in before this
    // rotation, so a textured blade's hue excursion is whatever its texture makes it.
    float32 HueVariation = 0.45f;
    // The blade's base belongs to the ground it grows out of. A blade is one gradient from the
    // terrain's own colour at the root to its canopy colour at the tip, and this shapes the canopy
    // end of it.
    //
    // RootShade dips the CANOPY end toward the root, over t < kGrassRootShadeRange (0.22). It rides
    // INSIDE that schedule, which weights the canopy chain by smoothstep(RootFadeStart,
    // RootFadeEnd, t), so it can only shape what that weight lets through — and over the whole-blade
    // span this component ships the weight there is 0.03 to 0.12, i.e. the ramp acts exactly where
    // the schedule discards its input. AT THE SHIPPED SPAN NO VALUE OF THIS DIAL CHANGES THE FIELD
    // (the full 0.82 -> 0.00 swing stays within two 8-bit levels). It shapes a blade only when
    // RootFadeEnd is authored short enough to raise the weight inside that range, so it belongs to
    // a short authored gradient; reach for the two RootFade thumbs first.
    //
    // It is weighted to almost nothing at the very bottom, so it cannot move the contact, and no
    // dial SCALES the contact: at a non-negative RootFadeStart the
    // root's albedo is the terrain's own resolved albedo exactly, measured at a close pose as
    // 99.95% of the lowest-height-bin fragments byte-identical to the ground's. A factor there
    // would put a value step on the contact line and read as the blade being a slightly different
    // colour from the ground rather than as shadow. A negative RootFadeStart is the deliberate
    // exception: it starts the blend under the root, so the root carries some tip colour and the
    // step is the authored choice.
    //
    // A RootFadeStart at or above the 0.22 recovery range is the other way to strand it: below the
    // fade start the canopy chain this ramp multiplies is discarded entirely. That is a second
    // route to the same dead end, not the only one — the shipped span already leaves it inert.
    float32 RootShade = 0.82f;
    // THE BLADE'S GRADIENT: the height fractions between which a blade leaves the ground colour it
    // grows out of and becomes its tip colour. Below RootFadeStart the blade IS the terrain; above
    // RootFadeEnd it is entirely itself. A negative start begins the blend below the root, so the
    // root already carries part of the tip colour. This is a material lever over the blade's albedo
    // only: the shading normal still settles onto the terrain normal over the whole blade, whatever
    // span is authored. The shipped span is the whole blade.
    float32 RootFadeStart = 0.0f;
    float32 RootFadeEnd = 1.0f;
    // How a blade is SHADED across its width, and what that costs in brightness.
    //
    // A blade is a near-vertical ribbon, so the normal its geometry actually has is near-horizontal.
    // Shading on it the whole way (BladeNormalForm 1) gives the strongest lit and shaded sides and
    // costs about a fifth of the field's brightness, because a horizontal normal catches far less of
    // a high sun than an up-facing one. 0 shades every blade on the up-dominant canopy normal: no
    // per-blade form, and the brightness of flat ground.
    float32 BladeNormalForm = 0.75f;
    // The brightness that form gives up, handed back. It stands in for the inter-blade scattering
    // nothing here models — light that misses one blade's face reaches it off its neighbours. It is
    // exactly zero at the blade base and in the far field, where the shading normal has settled onto
    // the canopy normal, nothing has been given up, and the grass has to match the ground it stands
    // in. In between it hands back the loss actually taken at each fragment, capped at what the
    // swing could have cost.
    //
    // It is NOT a uniform brightening: the loss is largest where the shading normal lost the most
    // light, so the darkest fragments are lifted hardest, and the dial therefore trades the field's
    // mean brightness against its blade-scale contrast. Lower it if a distant vista reads too
    // bright; 0 disables it. Raising it past the shipped value to cure a dark-looking field brings
    // back a bright band across the mid distance — see terrain_grass_surface.glsl, which carries the
    // measurements.
    float32 BladeScatterGain = 0.6f;
    // The master gate over the whole GROUNDING read, and nothing to do with cast shadows: it scales
    // the wind-gust shading, the root-shade ramp, and the blade's blend into the ground colour it
    // grows out of. At 0 a blade never adopts its terrain's colour at all, which destroys the
    // contact identity the rest of this component is built around, so it is an escape hatch for a
    // deliberately stylised field rather than a look dial.
    float32 GroundingStrength = 1.0f;
    // Sunlight through a thin blade, seen on the face turned away from the sun: scales that
    // transmission (0 switches it off). Added after the lighting, so cast shadows do not attenuate it.
    float32 Translucency = 0.35f;
    bool TextureGrass = false;
    float32 TextureCardsPerSquareMeter = 1.5f;
    float32 TextureSize = 1.0f;
    bool UseSplatRootColor = true;
    uint32 RootColor = 0xFF335F1Au;
    uint32 TipColor = 0xFFB3DB4Du;
    // The colour of that transmitted light, multiplied by the sun's colour: the blade's tint as seen
    // against the sun, independent of RootColor / TipColor and of the brightness and hue jitter.
    uint32 BacklightColor = 0xFFE6F06Au;
    Components::TextureRef AlbedoTextureAssetGuid;
    Components::TextureRef AlphaTextureAssetGuid;
    Components::TextureRef NormalTextureAssetGuid;
    uint32 AtlasColumns = 1u;
    uint32 AtlasRows = 1u;
    uint32 AtlasTileCount = 1u;
    float32 AlphaCutoff = 0.35f;
    float32 NormalStrength = 0.75f;
};

static_assert(std::is_trivially_copyable_v<TerrainGrass>,
              "TerrainGrass must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<TerrainGrass>,
              "TerrainGrass must be standard layout for ECS storage");

} // namespace GameEngine::Components
