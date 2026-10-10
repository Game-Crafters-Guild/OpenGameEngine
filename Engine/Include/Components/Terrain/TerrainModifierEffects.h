#pragma once

#include "Components/AssetRef.h"
#include "Components/Terrain/TerrainModifierBlend.h"
#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Types/StringUtils.h"
#include "Types/Types.h"

#include <cstddef>
#include <algorithm>
#include <cmath>
#include <string_view>
#include <type_traits>

namespace GameEngine::Components
{

// ---- Terrain modifier EFFECTS ----------------------------------------------
//
// An effect says WHAT happens inside a region; the TerrainModifierVolume on the
// same entity says WHERE. Effects are deliberately shape-unaware: the bake hands
// each one a scalar weight per terrain texel (and, for the flatten effect, the
// volume's reference height) and the effect never asks whether that came from a
// circle, a rectangle or a spline.
//
// Effects stack within one volume in StackOrder — the inspector writes it from
// the visual order of the effect sections, exactly as the post-process effect
// stack does, so dragging a section re-orders the maths.
//
// ONE EXCEPTION, stated here because this is the line an author's mental model
// comes from: an effect whose Blend is TerrainModifierBlend::Average does not
// apply at its own StackOrder position. Such an effect composes with the other
// members of its POOL, which applies once at its highest-priority member's slot,
// so every other height effect on the same volume composes BEFORE it wherever
// either one sits in the stack. Dragging a pooled effect's section re-orders
// nothing.
//
// These are NOT a base class — ECS components must be trivially copyable POD, so
// the shared fields are macro-injected (the same reason TerrainModifiers.h does
// it). The build-time ComponentScanner cannot expand the macro, so the effects
// carry no generated reflection: scenes go through the hand schemas
// (TerrainSceneSchemas.cpp) and the editor adds them via AddTerrainEffectDefault
// (TerrainModifierComponents.h), never through ComponentFactory.

#define GE_TERRAIN_EFFECT_COMMON_FIELDS                                        \
    /* Position in the volume's effect stack (lower = applied first). */        \
    int32 StackOrder = 0;                                                       \
                                                                                \
    /* Skip this effect without removing it. TerrainModifierSystem reads this  \
       field, so the effect keeps it (ECS::ComponentFlags::KeepsOwnEnabledField):\
       the inspector's section dot edits it and the scene writer never saves a \
       ComponentDisabled tag for it. */                                        \
    bool Enabled = true;                                                       \
    static constexpr bool KeepsOwnEnabledField = true;                         \
    uint8 _EffectPad[3] = {};

// Longest pool group name, terminator included. The name exists to let a handful
// of regions say they are a system of their own; it is not an identifier space.
inline constexpr uint32 kTerrainPoolGroupCapacity = 32;

// The pool membership every poolable height effect carries. Macro-injected for
// the same reason the common fields are: ECS components must be trivially
// copyable POD, so these are not a base class.
//
// Read ONLY when Blend is Average — pooling is the whole meaning of Average.
//
// Leave the group EMPTY — the usual case — to blend with every other unnamed
// member. Naming a group is the opt-OUT: it moves this region into a pool of its
// own, which blends internally and OVERWRITES, rather than averages with, the
// pooled effects outside it. Compared as text and hashed at gather.
//
// A pool is keyed by (effect kind, group name), not by the name alone: a pool
// averages one quantity, and a flatten's is an absolute candidate HEIGHT while a
// height offset's, a noise's and a stamp's are DISPLACEMENTS. A Noise pool named
// "dunes" and a Flatten pool named "dunes" are therefore different pools.
#define GE_TERRAIN_EFFECT_POOL_FIELDS                                           \
    char PoolGroup[kTerrainPoolGroupCapacity] = {};

// Deference to ground claims. Separate from the pool fields because it is not a
// pooling concept: ownership is about WHO holds the ground, and the operator an
// effect happens to compose with says nothing about that, so this is read on
// EVERY blend mode.
//
// True masks this effect's weight by (1 - claim) where another volume has
// claimed the ground — the pooled member's share of its pool, the in-place
// effect's own blend weight. False composes straight through a claim, which is
// what a region that OWNS its ground wants: it must not defer to its own claim.
//
// Read PER EFFECT, so an owning run grades its ground while the path beside it
// stops at the claim, and read at the slot the effect applies in, so a claim
// only holds ground against effects that come AFTER it. A pool's coverage clamp
// stays on the UNMASKED weight sum, so three regions over a half-owned texel
// still move it half way.
#define GE_TERRAIN_EFFECT_CLAIM_FIELDS                                          \
    bool RespectClaims = false;                                                 \
    uint8 _ClaimPad[3] = {};

// Levels the terrain toward a target height.
// Common use: building sites, road beds, landing pads.
//
// With Blend = Average the flatten joins a POOL instead of writing at its own
// stack position: the pool accumulates (weight * target) and (weight) across its
// members and applies the weighted average once, so two routes crossing the same
// ground land between their grades rather than the later one overwriting the
// earlier. A lone pooled flatten is a pool of one, whose weighted average is its
// own target and whose blend is its own weight.
//
// The flatten is also the only pooled effect whose target can come from a spline
// volume's per-station route height; the other poolable kinds carry no route.
struct TerrainFlattenEffect
{
    GE_TERRAIN_EFFECT_COMMON_FIELDS

    // On a PLANAR terrain: the TRUE world-space Y to flatten to. The terrain entity's own
    // translation is measured off during the bake, so the same value means the same altitude
    // wherever the terrain sits. When UseVolumeHeight is true this is instead an OFFSET from
    // the volume's reference height — the spline's own Y at the closest point for spline
    // volumes, so a sloped road flattens to its own profile, and the entity's Y for every
    // other shape, global included. That reference is a world Y too, so the sum is one as well.
    //
    // On a SPHERICAL terrain there is no "up" to be a Y, and the same field reads RADIALLY:
    // a signed offset from the nominal planet surface, or from the volume's own distance from
    // the planet centre under UseVolumeHeight (FillSphereFlattenEffect).
    float32 TargetHeight = 0.0f;
    bool UseVolumeHeight = true;
    uint8 _Pad0[3] = {};

    // How the target combines with the height already there. Set is the level-to-target
    // behaviour; Min makes the flatten a CUT that never raises ground already below the
    // target — a valley on a slope stops bulging its downhill side — and Max a fill that
    // never lowers ground already above it.
    TerrainModifierBlend Blend = TerrainModifierBlend::Set;
    uint8 _BlendPad[3] = {};

    // Blend radius in metres of height, read only by SmoothMin / SmoothMax.
    float32 BlendSmoothing = kDefaultBlendSmoothingM;

    // Pooled value: the resolved target, an absolute candidate HEIGHT. The pool
    // applies it by lerping the ground toward the weighted average.
    GE_TERRAIN_EFFECT_POOL_FIELDS
    GE_TERRAIN_EFFECT_CLAIM_FIELDS
};

static_assert(std::is_trivially_copyable_v<TerrainFlattenEffect>);
static_assert(std::is_standard_layout_v<TerrainFlattenEffect>);

// The authored pool group name. Bounded by the field rather than trusting a
// terminator: the buffer is scene text, and every reader — the gather that
// hashes it, the scene writer, the inspector — needs the same bound. A free
// template, so the components stay trivially copyable PODs.
template <typename TEffect>
inline std::string_view EffectPoolName(const TEffect& effect)
{
    return FixedStringView(effect.PoolGroup);
}

// Raises or lowers the terrain by a constant.
// Common use: sunken river beds, raised plateaus, embankments.
struct TerrainHeightOffsetEffect
{
    GE_TERRAIN_EFFECT_COMMON_FIELDS

    // Height change in world units, scaled by the volume weight.
    float32 Offset = 0.0f;

    TerrainModifierBlend Blend = TerrainModifierBlend::Add;
    uint8 _BlendPad[3] = {};

    // Blend radius in metres of height, read only by SmoothMin / SmoothMax.
    float32 BlendSmoothing = kDefaultBlendSmoothingM;

    // Pooled value: the offset, a DISPLACEMENT. The pool adds the weighted
    // average once, so two overlapping embankments raise the ground by their
    // average rather than by their sum.
    GE_TERRAIN_EFFECT_POOL_FIELDS
    GE_TERRAIN_EFFECT_CLAIM_FIELDS
};

static_assert(std::is_trivially_copyable_v<TerrainHeightOffsetEffect>);
static_assert(std::is_standard_layout_v<TerrainHeightOffsetEffect>);

// Upper bound on any authored fBM octave count. Past a handful of octaves the
// lattice is finer than the heightfield can resolve, so the extra octaves cost
// bake time and add nothing — but the cost is per texel and unbounded, and scene
// text is not a trusted input (the parser takes any uint32, the inspector's own
// clamp only guards the editor). The bake clamps against this so a hand-edited
// or corrupt octave count cannot turn a bake into a hang. Mirrors the relief
// path's kMaxReliefOctaves.
inline constexpr uint32 kMaxNoiseOctaves = 8u;

// Adds procedural fBM noise displacement.
// Common use: terrain detail, roughness, erosion-like effects.
struct TerrainNoiseEffect
{
    GE_TERRAIN_EFFECT_COMMON_FIELDS

    TerrainModifierBlend Blend = TerrainModifierBlend::Add;
    uint8 _BlendPad[3] = {};
    float32 Frequency = 8.0f;       // Noise frequency (higher = more detail)
    float32 Amplitude = 5.0f;       // Height displacement in world units
    uint32 Octaves = 4;             // fBM octave count, clamped to kMaxNoiseOctaves
    uint32 Seed = 0;                // Noise seed (0 = a fixed default seed)
    float32 Lacunarity = 2.0f;      // Frequency multiplier per octave
    float32 Persistence = 0.5f;     // Amplitude multiplier per octave

    // Blend radius in metres of height, read only by SmoothMin / SmoothMax.
    float32 BlendSmoothing = kDefaultBlendSmoothingM;

    // ---- Erosion block -----------------------------------------------------
    // Carves gradient-aligned gullies into this effect's own noise, using the
    // noise basis's analytic gradient. Strength 0 leaves the noise untouched,
    // byte for byte, and is the only value that costs nothing to evaluate.
    // These are the authoring defaults for TerrainECS::Erosion::ErosionParams,
    // which deliberately carries none of its own.
    float32 ErosionStrength = 0.0f;      // 0 = off
    uint32 ErosionOctaves = 4;           // Gully octaves, clamped to kMaxNoiseOctaves
    float32 ErosionFrequency = 2.0f;     // Multiplier on Frequency
    float32 ErosionDetail = 0.5f;        // How far fine gullies keep to steep ground
    float32 ErosionGullyWeight = 0.5f;   // How strongly gullies branch off each other
    float32 ErosionEdgeRounding = 0.35f; // 0 = crisp branching, 1 = rounded
    float32 ErosionFade = 0.7f;          // Fade-out at peaks and valleys

    // Pooled value: the fBM sample, a DISPLACEMENT. The pool adds the weighted
    // average once, so two overlapping noise regions CROSSFADE between their
    // fields instead of summing into twice the roughness at the overlap.
    GE_TERRAIN_EFFECT_POOL_FIELDS
    GE_TERRAIN_EFFECT_CLAIM_FIELDS
};

static_assert(std::is_trivially_copyable_v<TerrainNoiseEffect>);
static_assert(std::is_standard_layout_v<TerrainNoiseEffect>);

// Projects a height texture over the volume's extent.
// Common use: cliff details, craters, terrain features from sculpt data.
struct TerrainStampEffect
{
    GE_TERRAIN_EFFECT_COMMON_FIELDS

    TerrainModifierBlend Blend = TerrainModifierBlend::Add;
    uint8 _BlendPad[3] = {};
    float32 HeightScale = 10.0f;    // Scale applied to the stamp texture values
    float32 Rotation = 0.0f;        // Rotation in degrees around Y axis

    // Stamp texture asset reference (GUID); masks are Texture assets by
    // contract. When zero, uses a flat white texture (a flat raise/lower).
    Components::TextureRef StampAssetGuid;

    // Blend radius in metres of height, read only by SmoothMin / SmoothMax.
    float32 BlendSmoothing = kDefaultBlendSmoothingM;

    // Pooled value: the scaled mask sample, a DISPLACEMENT. The pool adds the
    // weighted average once, so two overlapping craters average instead of
    // stacking into one twice as deep.
    GE_TERRAIN_EFFECT_POOL_FIELDS
    GE_TERRAIN_EFFECT_CLAIM_FIELDS
};

static_assert(std::is_trivially_copyable_v<TerrainStampEffect>);
static_assert(std::is_standard_layout_v<TerrainStampEffect>);

// Sets splatmap material weights.
// Common use: painting rock on cliffs, dirt on paths, snow on peaks.
struct TerrainPaintLayerEffect
{
    GE_TERRAIN_EFFECT_COMMON_FIELDS

    // Which material layer to paint (0-3).
    uint32 LayerIndex = 0;

    // Paint strength (0-1). Blended with existing weights.
    float32 Strength = 1.0f;

    // When true, replaces the existing mix: each texel lerps toward the pure
    // layer by the volume's ramp weight, so the footprint reads as this layer
    // alone and the falloff ring is as soft as any additive edge.
    // When false, adds weight and renormalizes.
    bool Replace = false;
    uint8 _Pad0[3] = {};

    // Claimed ground keeps the material the claimant painted: this stroke's
    // weight is masked by (1 - claim), and a fully claimed texel is left exactly
    // as it was because the pass skips the composite entirely at zero weight.
    // The skip is the mechanism, not an optimization — the composite's additive
    // arm renormalizes, so calling it with zero weight can still cost an LSB.
    GE_TERRAIN_EFFECT_CLAIM_FIELDS
};

static_assert(std::is_trivially_copyable_v<TerrainPaintLayerEffect>);
static_assert(std::is_standard_layout_v<TerrainPaintLayerEffect>);

// Assigns materials by authored rule rows. These rows are the ONLY thing that
// places material on a terrain.
// Common use: the whole-terrain surface (grass low, rock steep, snow high), and
// shaped overrides that re-rule one region.
//
// Rows composite into the splat in order, each with TerrainPaintLayerEffect's
// blend/replace semantics, so a rules effect and a paint effect on one volume
// interleave by StackOrder like any other pair.
//
// Evaluated at BAKE time on the planar splat, which is why the rows carry no
// per-frame inputs. The sphere classifies per fragment and has no splat to bake
// into; when rules reach it they evaluate THESE SAME ROWS in the surface shader,
// so the seam is an evaluation site, not a second data model.
struct TerrainSurfaceRulesEffect
{
    GE_TERRAIN_EFFECT_COMMON_FIELDS

    // Rows in [0, RuleCount). Values above kMaxTerrainSurfaceRules are a loud
    // error where they arrive and are clamped where they are read, so a hand-
    // edited scene cannot walk off the array.
    uint32 RuleCount = 0;

    // Claimed ground keeps the material the claimant painted. Masks the volume
    // weight every row is scaled by, so the whole block defers together — a rule
    // block is one effect at one slot, and rows within it do not have slots of
    // their own to read ownership at.
    //
    // Arming this drops the terrain's GPU SPLAT bake to the CPU: the kernel
    // evaluates one rule row per texel with no ownership buffer to read, so the
    // packer refuses the batch rather than painting through claimed ground on
    // one arm and stopping at it on the other.
    GE_TERRAIN_EFFECT_CLAIM_FIELDS

    TerrainSurfaceRule Rules[kMaxTerrainSurfaceRules] = {};
};

static_assert(std::is_trivially_copyable_v<TerrainSurfaceRulesEffect>);
static_assert(std::is_standard_layout_v<TerrainSurfaceRulesEffect>);

// Marks the ground this volume owns. Writes NO height.
// Common use: a carriageway that later cuts must grade up to and stop at.
//
// A claim is the only way one modifier can defer to another by OWNERSHIP rather
// than by value: Min/Max/SmoothMin/SmoothMax let a modifier defer to what is
// already there, but never to who put it there. A road bed and the bank beside
// it can hold the same height and still need different treatment.
//
// Writing no height is what makes it safe to put a claim over finished ground:
// re-asserting the surface with a flatten instead would level the carriageway to
// the volume's own reference, which is the graded profile rather than the
// finished ground, and refill anything the route bridges.
struct TerrainGroundClaimEffect
{
    GE_TERRAIN_EFFECT_COMMON_FIELDS

    // How strongly this region owns its ground. The claim at a texel is Strength
    // times the volume's own shape weight, so the claim inherits the volume's
    // falloff. Claims combine by MAX, never by sum: ownership is not additive
    // and two overlapping claimants must not exceed full ownership.
    //
    // Ownership SATURATES: the accumulated claim is clamped to [0, 1] where it is
    // written, so a Strength above 1 owns the ground fully and no harder. There is
    // nothing above full ownership to express, and a pooled flatten's masked
    // weight carries a factor of 1 - claim — an unclamped value inverts that
    // blend rather than deepening it.
    float32 Strength = 1.0f;
};

static_assert(std::is_trivially_copyable_v<TerrainGroundClaimEffect>);
static_assert(std::is_standard_layout_v<TerrainGroundClaimEffect>);

// Regional grass targets relative to the terrain's global grass profile. Author
// that profile as the wild maximum; regions can shorten or thin it without
// increasing the placement budget. The volume owns shape, falloff and priority.
// Targets compose as lerp(current, target, weight), starting at (1,1), so a later
// region with scale 1 can restore the global profile over an earlier reduction.
// This affects planar grass only; it writes neither terrain height nor materials.
struct TerrainGrassEffect
{
    GE_TERRAIN_EFFECT_COMMON_FIELDS

    float32 HeightScale = 1.0f;   // [0,1], applied to final ribbon/card height
    float32 DensityScale = 1.0f;  // [0,1], stable thinning of existing candidates
};

// Script writes also pass through this finite, bounded admission at gather.
inline float32 ClampTerrainGrassEffectScale(float32 value)
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 1.0f;
}

// The height multiplier below which a region stops reading as short grass and starts reading as
// the ground colour under it. Measured at the shipped 0.72 m blade over a fixed interior rect with
// density held at 100%: the interior's luminance contrast is 60% of the untouched field at 0.50
// and 45% at 0.35, so the crossing sits between them.
//
// A READABILITY THRESHOLD, NOT A CLAMP. Nothing enforces it: the stored multiplier is admitted
// over the whole [0,1] range by ClampTerrainGrassEffectScale above, and every value below this one
// keeps changing the look - it changes it into ground rather than into shorter grass. It exists so
// the authoring text can name one number.
inline constexpr float32 kTerrainGrassHeightReadableFloor = 0.5f;
static_assert(std::is_trivially_copyable_v<TerrainGrassEffect>);
static_assert(std::is_standard_layout_v<TerrainGrassEffect>);

#undef GE_TERRAIN_EFFECT_COMMON_FIELDS
#undef GE_TERRAIN_EFFECT_POOL_FIELDS
#undef GE_TERRAIN_EFFECT_CLAIM_FIELDS

} // namespace GameEngine::Components
