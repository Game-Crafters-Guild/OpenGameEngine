#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainBakeCache.h"
#include "Components/SceneEntityTag.h"
#include "TerrainECS/TerrainModifierComponents.h" // the canonical set the gather reads (change gate)
#include "TerrainECS/TerrainAtlas.h" // shared coarse field lattice
#include "TerrainECS/TerrainSizingPlan.h" // same unified/atlas engagement rule as extraction
#include "TerrainECS/TerrainSurfaceRuleEval.h"   // per-texel rule row evaluation
#include "Noise/FractalNoise2D.h"
#include "TerrainECS/TerrainRuleNoise.h"
#include "TerrainECS/TerrainSplatComposite.h"    // the RGBA8 splat write paint and rules share
#include "CBTTerrain/CBTSphereFaceMap.h"  // ClassifySphereCapEdit, SphereEditRegions (planet bake)
#include "CBTTerrain/CBTPlanetShading.h"  // AnyTangent (sphere modifier tangent frame)
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "Mathematics/Matrix4x4.h"
#include "Spline/SplineData.h"
#include "Terrain/TerrainTypes.h" // kMaxTerrainMaterialLayers (the paint-layer clamp bound)
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"
#include "Core/Engine.h"
#include "Core/CpuProfiler.h"
#include "Assets/AssetManager.h"
#include "Assets/TextureAsset.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/ParallelAlgorithms.h"
#include "JobSystem/TaskHandle.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <string>
#include <utility>

namespace GameEngine::TerrainECS
{
namespace
{

// Simple hash combine for change detection.
uint64 HashCombine(uint64 seed, uint64 value)
{
    return seed ^ (value + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

// The height kinds that can join a pool. The splat kinds and the ground claim
// carry no blend at all, so they can never be pooled.
bool IsPoolableKind(ResolvedEffect::Kind kind)
{
    return kind == ResolvedEffect::Kind::Flatten
        || kind == ResolvedEffect::Kind::HeightOffset
        || kind == ResolvedEffect::Kind::Noise
        || kind == ResolvedEffect::Kind::Stamp;
}

// A pooled effect's value is an absolute candidate HEIGHT for the flatten and a
// DISPLACEMENT for every other poolable kind. The pool LERPS the ground toward
// the first and ADDS the second; that is the only difference between the two
// arms, and everything else about a pool — the accumulation, the coverage clamp,
// the claim mask — is shared.
//
// Applying the height arm to a displacement would drag the ground toward the
// displacement's magnitude as if it were an altitude: a +5 m embankment pooled
// as a height would pull 100 m terrain down to 5 m.
bool PooledValueIsHeight(ResolvedEffect::Kind kind)
{
    return kind == ResolvedEffect::Kind::Flatten;
}

// The blend and pooling fields live in the UNION, so every kind-general reader
// goes through these rather than naming one member and silently reading a
// neighbour's bytes. Valid only for a poolable kind; the callers all establish
// that first.
Components::TerrainModifierBlend EffectBlend(const ResolvedEffect& fx)
{
    switch (fx.EffectKind)
    {
    case ResolvedEffect::Kind::Flatten:      return fx.Flatten.Blend;
    case ResolvedEffect::Kind::HeightOffset: return fx.HeightOffset.Blend;
    case ResolvedEffect::Kind::Noise:        return fx.Noise.Blend;
    case ResolvedEffect::Kind::Stamp:        return fx.Stamp.Blend;
    default:
        assert(false && "EffectBlend read on a kind that carries no blend");
        return Components::TerrainModifierBlend::Add;
    }
}

float32 EffectBlendSmoothing(const ResolvedEffect& fx)
{
    switch (fx.EffectKind)
    {
    case ResolvedEffect::Kind::Flatten:      return fx.Flatten.BlendSmoothing;
    case ResolvedEffect::Kind::HeightOffset: return fx.HeightOffset.BlendSmoothing;
    case ResolvedEffect::Kind::Noise:        return fx.Noise.BlendSmoothing;
    case ResolvedEffect::Kind::Stamp:        return fx.Stamp.BlendSmoothing;
    default:
        assert(false && "EffectBlendSmoothing read on a kind that carries no blend");
        return 0.0f;
    }
}

StringId EffectPoolGroup(const ResolvedEffect& fx)
{
    switch (fx.EffectKind)
    {
    case ResolvedEffect::Kind::Flatten:      return fx.Flatten.PoolGroup;
    case ResolvedEffect::Kind::HeightOffset: return fx.HeightOffset.PoolGroup;
    case ResolvedEffect::Kind::Noise:        return fx.Noise.PoolGroup;
    case ResolvedEffect::Kind::Stamp:        return fx.Stamp.PoolGroup;
    default:
        assert(false && "EffectPoolGroup read on a kind that cannot pool");
        return kDefaultHeightPool;
    }
}

// The kinds that carry a RespectClaims flag: the four height kinds and the two
// splat kinds. The ground claim itself does not — a claim does not defer to a
// claim.
bool EffectCanRespectClaims(ResolvedEffect::Kind kind)
{
    return IsPoolableKind(kind) || ResolvedEffect::IsSplat(kind);
}

bool EffectRespectClaims(const ResolvedEffect& fx)
{
    switch (fx.EffectKind)
    {
    case ResolvedEffect::Kind::Flatten:      return fx.Flatten.RespectClaims;
    case ResolvedEffect::Kind::HeightOffset: return fx.HeightOffset.RespectClaims;
    case ResolvedEffect::Kind::Noise:        return fx.Noise.RespectClaims;
    case ResolvedEffect::Kind::Stamp:        return fx.Stamp.RespectClaims;
    case ResolvedEffect::Kind::PaintLayer:   return fx.PaintLayer.RespectClaims;
    case ResolvedEffect::Kind::Rules:        return fx.Rules.RespectClaims;
    default:
        assert(false && "EffectRespectClaims read on a kind that carries no claim flag");
        return false;
    }
}

// Fold one volume's ownership into the claim buffer at one texel.
//
// MAX, never sum: two claimants cannot own more than all of it, and the operator
// is idempotent under overlap so the claim does not depend on how many volumes
// happen to cover a texel. Saturated at 1 HERE, where ownership enters the
// buffer, so every reader gets a mask in [0, 1]: a reader weights itself by
// 1 - owned, and a Strength above 1 would otherwise make that negative and push
// its effect the wrong way instead of holding it back harder.
//
// ONE implementation, called by the height pass and the splat pass on their own
// grids. Two implementations of one operator is exactly what let a claim invert
// the blend on a single path in an earlier slice.
void AccumulateGroundClaim(float32& owned, float32 strength, float32 shapeWeight)
{
    owned = std::min(1.0f, std::max(owned, strength * shapeWeight));
}

// The effect's name as an author sees it in the inspector. Only the warnings use
// it, and they must name the section the author has to go and change.
const char* EffectKindName(ResolvedEffect::Kind kind)
{
    switch (kind)
    {
    case ResolvedEffect::Kind::Flatten:      return "Flatten";
    case ResolvedEffect::Kind::HeightOffset: return "Height Offset";
    case ResolvedEffect::Kind::Noise:        return "Noise";
    case ResolvedEffect::Kind::Stamp:        return "Stamp";
    case ResolvedEffect::Kind::PaintLayer:   return "Paint Layer";
    case ResolvedEffect::Kind::Rules:        return "Surface Rules";
    case ResolvedEffect::Kind::GroundClaim:  return "Ground Claim";
    default:                                 return "effect";
    }
}

// An effect that joins a POOL rather than writing at its own stack position.
// The one predicate every consumer asks — the planar bake that accumulates it,
// the GPU packer and the sphere path that refuse it, the warnings that reason
// about its claim masking — so none of them can disagree about what pooling is.
bool IsPooled(const ResolvedEffect& fx)
{
    return IsPoolableKind(fx.EffectKind)
        && EffectBlend(fx) == Components::TerrainModifierBlend::Average;
}

bool ModifierHasKind(const ResolvedModifier& mod, ResolvedEffect::Kind kind)
{
    return std::any_of(mod.Effects.begin(), mod.Effects.end(),
                       [kind](const ResolvedEffect& fx) { return fx.EffectKind == kind; });
}

const ResolvedEffect* FindEffectOfKind(const ResolvedModifier& mod, ResolvedEffect::Kind kind)
{
    for (const auto& fx : mod.Effects)
        if (fx.EffectKind == kind)
            return &fx;
    return nullptr;
}

// Footprint overlap in XZ, on the gathered AABBs. Both claim warnings ask it.
bool ModifiersOverlapXZ(const ResolvedModifier& a, const ResolvedModifier& b)
{
    return a.BoundsMinX <= b.BoundsMaxX && b.BoundsMinX <= a.BoundsMaxX
        && a.BoundsMinZ <= b.BoundsMaxZ && b.BoundsMinZ <= a.BoundsMaxZ;
}

// True when any effect in this modifier's stack is pooled.
bool HasPooledEffect(const ResolvedModifier& mod)
{
    if (mod.ModType != ResolvedModifier::Type::Volume)
        return false;
    for (const auto& fx : mod.Effects)
        if (IsPooled(fx))
            return true;
    return false;
}

// The pooled FLATTEN in a modifier's stack, or null. Distinct from the general
// predicate above and deliberately kept: the per-station route grade is the
// flatten's alone, so the polyline scan is conditioned on this rather than on
// pooling in general. One component of a type per entity, so a volume carries at
// most one flatten and therefore at most one pooled flatten.
const ResolvedEffect* FindPooledFlatten(const ResolvedModifier& mod)
{
    if (mod.ModType != ResolvedModifier::Type::Volume)
        return nullptr;
    for (const auto& fx : mod.Effects)
        if (fx.EffectKind == ResolvedEffect::Kind::Flatten && IsPooled(fx))
            return &fx;
    return nullptr;
}

// Extract yaw from a WorldTransform matrix (rotation around Y axis).
float32 ExtractYaw(const Components::WorldTransform& xf)
{
    // atan2(forward.x, forward.z) from the 3rd column of the rotation matrix.
    return std::atan2(xf.matrix[8], xf.matrix[10]);
}

// The authored bound and the bound the fBM loop actually enforces have to be the
// same number, or a count between them would clamp at the gather and then sum a
// different number of octaves than the author asked for.
static_assert(Components::kMaxNoiseOctaves == Noise::kMaxOctaves,
              "the authored octave cap and the noise module's loop backstop must agree");

// Polynomial smooth-min (the SDF union with a blend radius): C1 everywhere, so two masses
// meeting produce a rounded saddle instead of a crease. `k` is in the same units as a and b.
// Undefined at k <= 0 — callers guard.
float32 PolynomialSmoothMin(float32 a, float32 b, float32 k)
{
    const float32 t = std::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return (b + (a - b) * t) - k * t * (1.0f - t);
}

// The whole blend algebra, in normalized height. `value` is the effect's contribution:
// a DISPLACEMENT for Add/Subtract, a candidate HEIGHT for Set and the union operators.
// Every arm is weight-feathered so a volume's falloff skirt never steps.
//
// Add is the default arm, so any value outside the enum reaching a component still bakes
// as Add. Add/Subtract/Set keep their literal shipped expressions rather than a common
// lerp rewrite, because (h + v) - h is not h's neighbour in float and the migration-parity
// bakes compare bytes.
//
// `smoothingNorm` is the SmoothMin/SmoothMax blend radius already divided by the height
// scale; a non-positive radius degenerates to the hard operator.
float32 BlendHeight(float32 current, float32 value, float32 weight,
                    Components::TerrainModifierBlend blend, float32 smoothingNorm)
{
    switch (blend)
    {
    case Components::TerrainModifierBlend::Subtract:
        return current - value * weight;
    case Components::TerrainModifierBlend::Set:
        return current + (value - current) * weight;
    case Components::TerrainModifierBlend::Min:
        return current + (std::min(current, value) - current) * weight;
    case Components::TerrainModifierBlend::Max:
        return current + (std::max(current, value) - current) * weight;
    case Components::TerrainModifierBlend::SmoothMin:
    {
        const float32 unioned = smoothingNorm > 0.0f
            ? PolynomialSmoothMin(current, value, smoothingNorm)
            : std::min(current, value);
        return current + (unioned - current) * weight;
    }
    case Components::TerrainModifierBlend::SmoothMax:
    {
        // smoothMax(a, b, k) == -smoothMin(-a, -b, k).
        const float32 unioned = smoothingNorm > 0.0f
            ? -PolynomialSmoothMin(-current, -value, smoothingNorm)
            : std::max(current, value);
        return current + (unioned - current) * weight;
    }
    case Components::TerrainModifierBlend::Add:
    default:
        return current + value * weight;
    }
}

// Pack the priority-sorted resolved modifiers into the GPU height-bake row layout
// (slice-1b). Returns false — the caller then falls back to the CPU bake — if any
// HEIGHT-affecting modifier is a type/shape the slice-1 kernel does not implement
// (Stamp/Spline/SculptZone, or a spline shape). Splat-only modifiers never touch height,
// so they are simply not packed.
bool PackModifiersForGpuBake(const std::vector<ResolvedModifier>& mods,
                             std::vector<ModifierGpu>& out)
{
    out.clear();
    out.reserve(mods.size());

    // Shape framing for a volume's effects: the kernel keys on
    // circle-vs-rectangle plus the entity-space extents.
    auto packShape = [](const ResolvedModifier& m) {
        ModifierGpu g{};
        g.Shape = (m.Shape == Components::TerrainModifierShape::Circle)
                      ? static_cast<uint32>(ModifierGpuShape::Circle)
                      : static_cast<uint32>(ModifierGpuShape::Rectangle);
        g.CenterX = m.Position.x;
        g.CenterZ = m.Position.z;
        g.Yaw = m.YawRadians;
        g.Radius = m.Radius;
        g.RectHalfX = m.RectHalfX;
        g.RectHalfZ = m.RectHalfZ;
        g.Falloff = m.Falloff;
        return g;
    };

    for (const auto& m : mods)
    {
        if (m.IsSplatOnlyModifier())
            continue; // paint layer / paint zone / paint-only volume — no height contribution
        if (m.Shape == Components::TerrainModifierShape::Spline)
            return false;
        // The kernel's ComputeWeight knows circles and rectangles only, and a
        // global volume packed as either would bake its authored half-extents as
        // a footprint instead of covering the world. There is no SHAPE_GLOBAL to
        // pack it as, so the whole bake falls back to the CPU — the same refusal
        // a spline shape takes.
        if (m.GlobalScope)
            return false;

        if (m.ModType == ResolvedModifier::Type::Volume)
        {
            // The kernel evaluates shape weight per modifier record, so a volume
            // packs as one record per height effect. That is exact only while
            // the volume's weight is the plain shape falloff the kernel also
            // computes: an inward feather or a master weight has no kernel
            // twin, and effects it cannot evaluate force the whole bake to CPU.
            //
            // The inward-feather refusal is what holds CPU/GPU parity: the
            // kernel implements only ShapeFalloffWeight's outward-only branch,
            // so a volume with BOTH falloffs — whose weight is one ramp spanning
            // the edge, not a band that stops at it — must never reach it.
            if (m.FalloffInward > 0.0f || m.Weight != 1.0f)
                return false;
            for (const auto& fx : m.Effects)
            {
                // Splat-only effects contribute nothing to the heightfield — ApplySplatModifiers
                // owns them, on whichever arm runs the splat — so they neither pack a row nor
                // refuse the height bake. Refusing on them would price a rules or paint effect
                // as if it were an unimplemented HEIGHT effect, dropping the whole terrain's GPU
                // height bake to the CPU because of a modifier that never touches a height.
                if (ResolvedEffect::IsSplat(fx.EffectKind))
                    continue;
                // Pooling is a cross-modifier accumulate-then-apply, and the
                // kernel evaluates one modifier record per texel with no scratch
                // to accumulate into. Refused rather than packed, for EVERY
                // poolable kind: the kernel declares no Average id and its blend
                // switch falls through to Add, so a packed pooled effect would
                // bake as an additive raise instead of an average.
                if (IsPooled(fx))
                    return false;
                ModifierGpu g = packShape(m);
                if (fx.EffectKind == ResolvedEffect::Kind::Flatten)
                {
                    // The kernel has no reference-height lookup: only an
                    // absolute target is expressible.
                    if (fx.Flatten.UseVolumeHeight)
                        return false;
                    g.Type = static_cast<uint32>(ModifierGpuType::Flatten);
                    g.Blend = static_cast<uint32>(fx.Flatten.Blend);
                    g.BlendSmoothing = fx.Flatten.BlendSmoothing;
                    g.NoiseFreqOrTarget = fx.Flatten.TargetHeight;
                }
                else if (fx.EffectKind == ResolvedEffect::Kind::Noise)
                {
                    // The kernel has no erosion twin yet (no erosion_filter.glsl),
                    // so an armed erosion block forces the whole bake to CPU
                    // rather than silently baking un-eroded noise on the GPU.
                    if (fx.Noise.Erosion.Strength > 0.0f)
                        return false;
                    g.Type = static_cast<uint32>(ModifierGpuType::Noise);
                    g.Blend = static_cast<uint32>(fx.Noise.Blend);
                    g.BlendSmoothing = fx.Noise.BlendSmoothing;
                    g.Octaves = fx.Noise.Octaves;
                    g.NoiseFreqOrTarget = fx.Noise.Frequency;
                    g.NoiseAmp = fx.Noise.Amplitude;
                    g.NoiseLacunarity = fx.Noise.Lacunarity;
                    g.NoisePersistence = fx.Noise.Persistence;
                    g.Seed = fx.Noise.Seed;
                }
                else
                {
                    return false; // height-offset / stamp not GPU-bakeable in slice 1
                }
                out.push_back(g);
            }
            continue;
        }

        // A sculpt zone's payload has no kernel twin: the whole bake falls to CPU.
        return false;
    }
    return true;
}

// Record one tile's sample rect into the GPU bake batch (slice-1b/1c) so the extraction
// system can dispatch the height evaluation on the GPU. The caller only calls this when the
// GPU eval-skip path is active (flag on + eligible + modifiers bakeable).
void RecordGpuBakeTile(TiledTerrainData& tiled, const TerrainTileData& tp, const TileCoord& coord,
                       float32 tileSize, int32 minX, int32 minZ, int32 maxX, int32 maxZ)
{
    GpuHeightBakeTileRequest r{};
    r.TileX = coord.X;
    r.TileZ = coord.Z;
    r.RectMinX = minX;
    r.RectMinZ = minZ;
    r.RectMaxX = maxX;
    r.RectMaxZ = maxZ;
    r.OriginX = tp.WorldOriginX;
    r.OriginZ = tp.WorldOriginZ;
    r.TileSize = tileSize;
    tiled.GpuBakeBatch.Tiles.push_back(r);
}

constexpr float32 kDegToRad = 0.01745329251994329577f;

// Bilinear sample of a single-channel mask at UV in [0,1] (clamped), texel
// centers aligned to the UV corners (heightfield-style edge mapping).
float32 SampleMaskBilinear(const float32* texels, uint32 width, uint32 height,
                           float32 u, float32 v)
{
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);

    const float32 fx = u * static_cast<float32>(width - 1);
    const float32 fz = v * static_cast<float32>(height - 1);
    const uint32 x0 = static_cast<uint32>(fx);
    const uint32 z0 = static_cast<uint32>(fz);
    const uint32 x1 = std::min(x0 + 1, width - 1);
    const uint32 z1 = std::min(z0 + 1, height - 1);
    const float32 tx = fx - static_cast<float32>(x0);
    const float32 tz = fz - static_cast<float32>(z0);

    const float32 s00 = texels[static_cast<size_t>(z0) * width + x0];
    const float32 s10 = texels[static_cast<size_t>(z0) * width + x1];
    const float32 s01 = texels[static_cast<size_t>(z1) * width + x0];
    const float32 s11 = texels[static_cast<size_t>(z1) * width + x1];

    const float32 s0 = s00 + tx * (s10 - s00);
    const float32 s1 = s01 + tx * (s11 - s01);
    return s0 + tz * (s1 - s0);
}

// A pooled effect's pool id: the authored group name hashed, or the DEFAULT
// POOL for the (usual) unnamed case.
//
// kDefaultHeightPool rather than the hash of the empty string, so that a name
// someone does author can never collide with the pool every unnamed member
// shares. Blending is the default and the name is the opt-OUT: leaving it blank
// is how a route says "I am part of the same ground as everything else".
//
// The id alone is NOT the pool key — the bake pairs it with the effect kind, so
// two kinds sharing a name stay in separate pools.
template <typename TEffect>
StringId ResolvePoolGroup(const TEffect& effect)
{
    const std::string_view name = Components::EffectPoolName(effect);
    if (name.empty())
        return kDefaultHeightPool;
    const StringId id = HashStringId(name);
    // A named group that hashed onto the sentinel would silently join the shared
    // pool instead of separating from it — the one failure this scheme can have,
    // and one displaced name is a better answer than one silent merge.
    return id == kDefaultHeightPool ? id + 1 : id;
}

// A volume's authored shape mapped onto the resolved-modifier shape the bounds,
// weight and sphere machinery already speak. Both spline shapes resolve to
// Shape::Spline; ResolvedModifier::SplineFillInterior carries the path/area
// distinction from there. Global resolves to Rectangle and sets
// ResolvedModifier::GlobalScope, which every shape branch tests first — the one
// consumer that still reads the resolved shape for a global volume is the Stamp
// mask basis, and Rectangle is what points it at the volume's authored
// half-extents, the only projection domain a shapeless volume can offer.
Components::TerrainModifierShape VolumeShapeToModifierShape(Components::TerrainVolumeShape shape)
{
    switch (shape)
    {
    case Components::TerrainVolumeShape::Rectangle:
    case Components::TerrainVolumeShape::Global:
        return Components::TerrainModifierShape::Rectangle;
    case Components::TerrainVolumeShape::Circle:
        return Components::TerrainModifierShape::Circle;
    case Components::TerrainVolumeShape::SplinePath:
    case Components::TerrainVolumeShape::SplineArea:
        break;
    }
    return Components::TerrainModifierShape::Spline;
}

// Compute world-space AABB for a modifier. Spline-shaped modifiers need their
// ResolvedSpline set first — call this after the spline is resolved.
void ComputeModifierBounds(ResolvedModifier& mod)
{
    const float32 px = mod.Position.x;
    const float32 pz = mod.Position.z;
    const float32 f = mod.Falloff;

    if (mod.GlobalScope)
    {
        // No footprint: the AABB every consumer intersects against is the whole
        // plane. Infinity rather than a large finite number so no terrain, at
        // any origin or size, can sit outside it — the sample-index conversion
        // clamps (ModifierSampleIndex), which is what keeps an infinite box from
        // reaching an undefined float-to-int cast.
        constexpr float32 inf = std::numeric_limits<float32>::infinity();
        mod.BoundsMinX = -inf;
        mod.BoundsMinZ = -inf;
        mod.BoundsMaxX = inf;
        mod.BoundsMaxZ = inf;
        return;
    }

    if (mod.Shape == Components::TerrainModifierShape::Spline)
    {
        // The spline's own geometry defines the region, not the entity position:
        // union the world-space segment bounds (already inflated by each
        // segment's swept radius in RebuildSplineCache) and pad by the falloff
        // skirt. An unresolved spline yields an inverted box, which every bake
        // loop reads as "no samples".
        mod.BoundsMinX = std::numeric_limits<float32>::max();
        mod.BoundsMinZ = std::numeric_limits<float32>::max();
        mod.BoundsMaxX = -std::numeric_limits<float32>::max();
        mod.BoundsMaxZ = -std::numeric_limits<float32>::max();
        if (!mod.ResolvedSpline)
            return;
        for (const auto& segBounds : mod.ResolvedSpline->SegmentBounds)
        {
            mod.BoundsMinX = std::min(mod.BoundsMinX, segBounds.min.x - f);
            mod.BoundsMinZ = std::min(mod.BoundsMinZ, segBounds.min.z - f);
            mod.BoundsMaxX = std::max(mod.BoundsMaxX, segBounds.max.x + f);
            mod.BoundsMaxZ = std::max(mod.BoundsMaxZ, segBounds.max.z + f);
        }
    }
    else if (mod.Shape == Components::TerrainModifierShape::Circle)
    {
        const float32 r = mod.Radius + f;
        mod.BoundsMinX = px - r;
        mod.BoundsMinZ = pz - r;
        mod.BoundsMaxX = px + r;
        mod.BoundsMaxZ = pz + r;
    }
    else // Rectangle
    {
        // Compute rotated rectangle AABB (conservative).
        const float32 c = std::cos(mod.YawRadians);
        const float32 s = std::sin(mod.YawRadians);
        const float32 hx = mod.RectHalfX + f;
        const float32 hz = mod.RectHalfZ + f;
        const float32 extX = std::abs(c * hx) + std::abs(s * hz);
        const float32 extZ = std::abs(s * hx) + std::abs(c * hz);
        mod.BoundsMinX = px - extX;
        mod.BoundsMinZ = pz - extZ;
        mod.BoundsMaxX = px + extX;
        mod.BoundsMaxZ = pz + extZ;
    }
}

// Bilinear sample of an R8 mask normalized to [0,1], edge-clamped like
// SampleMaskBilinear (texel centers aligned to UV corners).
float32 SampleMaskR8Bilinear(const uint8* texels, uint32 width, uint32 height,
                             float32 u, float32 v)
{
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const float32 fx = u * static_cast<float32>(width - 1);
    const float32 fz = v * static_cast<float32>(height - 1);
    const uint32 x0 = static_cast<uint32>(fx);
    const uint32 z0 = static_cast<uint32>(fz);
    const uint32 x1 = std::min(x0 + 1, width - 1);
    const uint32 z1 = std::min(z0 + 1, height - 1);
    const float32 tx = fx - static_cast<float32>(x0);
    const float32 tz = fz - static_cast<float32>(z0);
    constexpr float32 kInv255 = 1.0f / 255.0f;
    const float32 s00 = texels[static_cast<size_t>(z0) * width + x0] * kInv255;
    const float32 s10 = texels[static_cast<size_t>(z0) * width + x1] * kInv255;
    const float32 s01 = texels[static_cast<size_t>(z1) * width + x0] * kInv255;
    const float32 s11 = texels[static_cast<size_t>(z1) * width + x1] * kInv255;
    const float32 s0 = s00 + tx * (s10 - s00);
    const float32 s1 = s01 + tx * (s11 - s01);
    return s0 + tz * (s1 - s0);
}

// XZ scale from a world-transform matrix (basis-vector lengths of columns 0/2).
// A zone's effective world half-extents are its local extents times this scale.
void ExtractXZScale(const Components::WorldTransform& xf, float32& scaleX, float32& scaleZ)
{
    scaleX = std::sqrt(xf.matrix[0] * xf.matrix[0] + xf.matrix[1] * xf.matrix[1]
                       + xf.matrix[2] * xf.matrix[2]);
    scaleZ = std::sqrt(xf.matrix[8] * xf.matrix[8] + xf.matrix[9] * xf.matrix[9]
                       + xf.matrix[10] * xf.matrix[10]);
}

// Map a zone payload's accumulated dirty texel rect into a world-space AABB via
// the zone transform (RectHalfX/Z already fold the transform scale), padded by
// falloff so an edge dab re-bakes the falloff skirt. Sets HasPayloadDirty +
// the PayloadDirty* fields; leaves them false when the payload has no dirt.
void ComputeZonePayloadWorldRect(ResolvedModifier& mod, const TerrainZonePayload& payload)
{
    mod.HasPayloadDirty = false;
    if (!payload.DirtyAny || payload.Width < 2 || payload.Height < 2)
        return;

    const float32 invW = 1.0f / static_cast<float32>(payload.Width - 1);
    const float32 invH = 1.0f / static_cast<float32>(payload.Height - 1);
    // Expand the texel rect by one texel (bilinear reach) before mapping to UV.
    const float32 u0 = std::clamp((payload.DirtyMinX - 1) * invW, 0.0f, 1.0f);
    const float32 u1 = std::clamp(payload.DirtyMaxX * invW, 0.0f, 1.0f);
    const float32 v0 = std::clamp((payload.DirtyMinZ - 1) * invH, 0.0f, 1.0f);
    const float32 v1 = std::clamp(payload.DirtyMaxZ * invH, 0.0f, 1.0f);

    // UV -> zone-local (the payload spans [-RectHalf, +RectHalf] in each axis).
    const float32 lx[2] = {(2.0f * u0 - 1.0f) * mod.RectHalfX, (2.0f * u1 - 1.0f) * mod.RectHalfX};
    const float32 lz[2] = {(2.0f * v0 - 1.0f) * mod.RectHalfZ, (2.0f * v1 - 1.0f) * mod.RectHalfZ};

    const float32 c = std::cos(mod.YawRadians);
    const float32 s = std::sin(mod.YawRadians);
    float32 minX = std::numeric_limits<float32>::max();
    float32 minZ = std::numeric_limits<float32>::max();
    float32 maxX = -std::numeric_limits<float32>::max();
    float32 maxZ = -std::numeric_limits<float32>::max();
    for (int32 ci = 0; ci < 2; ++ci)
        for (int32 cj = 0; cj < 2; ++cj)
        {
            const float32 wx = mod.Position.x + c * lx[ci] - s * lz[cj];
            const float32 wz = mod.Position.z + s * lx[ci] + c * lz[cj];
            minX = std::min(minX, wx);
            minZ = std::min(minZ, wz);
            maxX = std::max(maxX, wx);
            maxZ = std::max(maxZ, wz);
        }

    mod.HasPayloadDirty = true;
    mod.PayloadDirtyMinX = minX - mod.Falloff;
    mod.PayloadDirtyMinZ = minZ - mod.Falloff;
    mod.PayloadDirtyMaxX = maxX + mod.Falloff;
    mod.PayloadDirtyMaxZ = maxZ + mod.Falloff;
}

// Analytic sphere modifiers (sculpt shape-accuracy S2): GE_TERRAIN_ANALYTIC_MODIFIERS=1 evaluates
// closed-form-primitive modifiers (circular flattens today) AT SAMPLE TIME instead of rasterizing
// them into sculpt-store texels — exact circles at any planet radius. Default OFF (dark-ship).
// One flag read for the whole terrain stack (S3 shares it with the brush's transient stroke path):
// TerrainService::AnalyticModifiersEnabled, which reads the env fresh so a mid-session flip is a
// state change (it folds into the terrain-state hash and forces the clearing full re-bake — the
// no-double-apply transition the dark-ship oracles toggle).
bool AnalyticModifiersEnabled()
{
    return TerrainService::AnalyticModifiersEnabled();
}

uint64 ComputeTerrainStateHash(ECS::World& world, TerrainService& terrainService)
{
    auto hashFloat = [](float32 f) -> uint64 {
        uint32 u;
        std::memcpy(&u, &f, sizeof(u));
        return static_cast<uint64>(u);
    };

    uint64 hash = 0;
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity,
                  const Components::Terrain& terrain,
                  const Components::WorldTransform& worldXf)
        {
            hash = HashCombine(hash, hashFloat(worldXf.matrix[12]));
            hash = HashCombine(hash, hashFloat(worldXf.matrix[13]));
            hash = HashCombine(hash, hashFloat(worldXf.matrix[14]));
            hash = HashCombine(hash, hashFloat(terrain.SizeX));
            hash = HashCombine(hash, hashFloat(terrain.SizeZ));
            hash = HashCombine(hash, hashFloat(terrain.HeightScale));
            hash = HashCombine(hash, hashFloat(terrain.SamplesPerMeter));

            // Domain + planet radius shape the sphere modifier bake: footprints are authored in
            // world METRES, so a radius change moves every modifier's ANGULAR footprint on the
            // sphere. Folding them makes a planet resize a terrain-state change -> full sphere
            // re-bake at the new radius, re-deriving the modifier layer exactly (the freehand dab
            // layer is remapped by SphereSculptLayer::Configure; the modifier layer is derived
            // data, so re-derivation beats resampling). Spherical-only so planar radius noise
            // (an unused field) can never churn planar re-bakes.
            hash = HashCombine(hash, static_cast<uint64>(terrain.Domain));
            if (terrain.Domain == Components::TerrainDomain::Spherical)
            {
                hash = HashCombine(hash, hashFloat(terrain.PlanetRadius));
                // Base relief is the surface the sphere bake DERIVES FROM: a flatten stores the
                // per-direction offset that cancels the relief, so an Amplitude / Frequency /
                // Octaves edit invalidates every baked offset exactly the way a radius edit does.
                // Default-constructed when the component is absent, matching the bake's own
                // fallback, so a present-but-default relief hashes identically to an absent one —
                // otherwise a present-vs-absent hash split would disagree with bakes that are
                // byte-identical.
                const auto* reliefPtr = world.GetComponent<Components::TerrainPlanetRelief>(entity);
                const Components::TerrainPlanetRelief relief =
                    reliefPtr ? *reliefPtr : Components::TerrainPlanetRelief{};
                hash = HashCombine(hash, hashFloat(relief.Amplitude));
                hash = HashCombine(hash, hashFloat(relief.Frequency));
                hash = HashCombine(hash, static_cast<uint64>(relief.Octaves));
                // The analytic-modifier flag (S2) partitions WHICH modifiers bake into the store
                // vs publish as placements, so a flip is a terrain-state change: the forced full
                // re-bake clears the store's modifier layer and re-derives it under the new
                // partition — the no-double-apply / no-stale-bake transition in both directions.
                hash = HashCombine(hash, AnalyticModifiersEnabled() ? 0x414e414cull : 0ull);
            }

            // Base source (§3.1): enum + heightmap ref + decoded-content
            // version, so a source change or heightmap hot-reload reads as a
            // terrain-state change and triggers a full re-bake (a region diff
            // can't scope a base swap). The version lookup never decodes.
            hash = HashCombine(hash, static_cast<uint64>(terrain.BaseSource));
            if (terrain.BaseSource == Components::TerrainBaseSource::HeightmapAsset
                && !terrain.TerrainAssetGuid.IsNull())
            {
                const GUID heightmapGuid = terrain.TerrainAssetGuid.ToGuid();
                hash = HashCombine(hash, static_cast<uint64>(std::hash<GUID>{}(heightmapGuid)));
                hash = HashCombine(hash, terrainService.GetHeightmapContentVersion(heightmapGuid));
            }

            hash = HashCombine(hash, static_cast<uint64>(terrain.TerrainDataHandle));
            hash = HashCombine(hash, static_cast<uint64>(terrain.TerrainDataGeneration));
            hash = HashCombine(hash, static_cast<uint64>(terrain.TiledTerrainHandle));
            hash = HashCombine(hash, static_cast<uint64>(terrain.TiledTerrainGeneration));

            TerrainHandle handle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration};
            if (const auto* data = terrainService.GetTerrainData(handle))
            {
                hash = HashCombine(hash, static_cast<uint64>(data->Heightfield.GetWidth()));
                hash = HashCombine(hash, static_cast<uint64>(data->Heightfield.GetHeight()));
                hash = HashCombine(hash, static_cast<uint64>(data->Config.LODLevels));
                hash = HashCombine(hash, static_cast<uint64>(data->Config.PatchGridSize));
            }
            else
            {
                hash = HashCombine(hash, 0x4d495353494e4748ULL); // Missing heightfield handle.
            }

            TiledTerrainHandle tiledHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration};
            if (const auto* tiled = terrainService.GetTiledTerrainData(tiledHandle))
            {
                // Deliberately excludes tile RESIDENCY (Revision, Tiles.size): a
                // tile streaming in/out must not move this hash. It used to, which
                // forced fullBake=true and re-baked + version-bumped EVERY resident
                // tile on every streaming frame — the E6-review#3 collider-churn
                // storm. Residency now wakes a scoped new-tile bake instead
                // (AnyTiledTileNeedsModifierBake / BakeTiledStreamedTiles). Only
                // genuine CONFIG changes (tile count, tile world size) belong here,
                // since those do require a full re-bake.
                hash = HashCombine(hash, static_cast<uint64>(tiled->Config.TilesPerAxisX));
                hash = HashCombine(hash, static_cast<uint64>(tiled->Config.TilesPerAxisZ));
                hash = HashCombine(hash, hashFloat(tiled->Config.TileWorldSize));
            }
        });

    return hash;
}

// True when any enabled tiled terrain has a tile whose authored modifiers have not been
// applied yet — i.e. a tile just streamed in. Covers BOTH LODs: a Full arrival needs its
// height + splat bake, and a COARSE arrival needs the surface rules composited over its
// coarse splat, or the distant ring renders the unbaked base and pops on upgrade. Wakes the
// gather (ShouldGather) and forces a bake pass (Update) even though the terrain-
// state hash is unchanged (residency is deliberately out of that hash). The bake
// scopes to the un-applied tiles alone (BakeTiledStreamedTiles), so a stream-in
// costs one tile's bake — never a full re-bake / collider re-cook of every tile.
// O(resident tiles) flag scan, short-circuits on the first hit; touches no
// samples (the idle-scan perf oracle is unaffected).
bool AnyTiledTileNeedsModifierBake(ECS::World& world, TerrainService& terrainService)
{
    bool needs = false;
    world.Query<ECS::Read<Components::Terrain>>()
        .Each([&](ECS::EntityHandle /*entity*/, const Components::Terrain& terrain)
        {
            if (needs)
                return;
            if (terrain.TiledTerrainHandle == 0 && terrain.TiledTerrainGeneration == 0)
                return;
            TiledTerrainHandle handle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration};
            const auto* tiled = terrainService.GetTiledTerrainData(handle);
            if (!tiled)
                return;
            for (const auto& [coord, tilePtr] : tiled->Tiles)
            {
                if (!tilePtr)
                    continue;
                const bool fullNeedsBake =
                    tilePtr->LodState == TileLodState::Full && !tilePtr->ModifiersApplied;
                const bool coarseNeedsRules =
                    tilePtr->LodState == TileLodState::Coarse && !tilePtr->CoarseSplatBaked;
                if (fullNeedsBake || coarseNeedsRules)
                {
                    needs = true;
                    return;
                }
            }
        });
    return needs;
}

// True when an enabled tiled terrain would page but its height page overlay is not built yet
// (TiledTerrainData::AwaitsPageOverlay): wakes the gather so the overlay is baked. O(terrains).
bool AnyTiledTerrainAwaitsPageOverlay(ECS::World& world, TerrainService& terrainService)
{
    bool awaits = false;
    world.Query<ECS::Read<Components::Terrain>>()
        .Each([&](ECS::EntityHandle /*entity*/, const Components::Terrain& terrain)
        {
            if (awaits || (terrain.TiledTerrainHandle == 0 && terrain.TiledTerrainGeneration == 0))
                return;
            const auto* tiled = terrainService.GetTiledTerrainData(
                TiledTerrainHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration});
            awaits = tiled && tiled->AwaitsPageOverlay();
        });
    return awaits;
}

// Component-level base-source inputs only: BaseSource, heightmap GUID, and
// decoded-content version per enabled terrain. Deliberately handle- and
// creation-independent (unlike ComputeTerrainStateHash) so the modifier-free
// re-fill trigger doesn't fire when extraction merely creates terrain data —
// the creation fill already applies the correct base, and startup must stay
// bake-free.
uint64 ComputeBaseInputHash(ECS::World& world, TerrainService& terrainService)
{
    uint64 hash = 0;
    world.Query<ECS::Read<Components::Terrain>>()
        .Each([&](ECS::EntityHandle /*entity*/, const Components::Terrain& terrain)
        {
            hash = HashCombine(hash, static_cast<uint64>(terrain.BaseSource));
            if (terrain.BaseSource == Components::TerrainBaseSource::HeightmapAsset
                && !terrain.TerrainAssetGuid.IsNull())
            {
                const GUID heightmapGuid = terrain.TerrainAssetGuid.ToGuid();
                hash = HashCombine(hash, static_cast<uint64>(std::hash<GUID>{}(heightmapGuid)));
                hash = HashCombine(hash, terrainService.GetHeightmapContentVersion(heightmapGuid));
            }
        });
    return hash;
}

// Build a world-space copy of an entity's spline into `storage` and return a
// stable pointer to it, or null when the entity's spline is not resolvable yet
// (no data slot, disabled, or fewer than two control points — all transient
// authoring states). Control points are authored in entity-local space while the
// evaluator's spatial queries work in world coordinates, so the copy is
// transformed by the entity's WorldTransform and its cache rebuilt.
const Spline::SplineData* AppendWorldSpline(std::deque<Spline::SplineData>& storage,
                                            SplineECS::SplineService& service,
                                            const Components::SplineComponent& splineComp,
                                            const Components::WorldTransform& xf)
{
    const SplineECS::SplineHandle handle(splineComp.SplineDataIndex,
                                         splineComp.SplineDataGeneration);
    const auto* splineData = service.GetSplineData(handle);
    if (!splineData || !splineData->IsValid())
        return nullptr;

    Mathematics::Matrix4x4 worldM;
    std::memcpy(worldM.Data(), xf.matrix, sizeof(xf.matrix));

    Spline::SplineData& worldSpline = storage.emplace_back(*splineData);
    for (auto& pt : worldSpline.Points)
        pt.Position = worldM.TransformPoint(pt.Position);
    Spline::RebuildSplineCache(worldSpline);
    return &worldSpline;
}

// Passed to the apply helpers when the bake is not region-clamped.
constexpr int32 kRegionUnbounded = std::numeric_limits<int32>::max();

// Reduction seeds for a heightfield min/max scan: min starts above any real
// height, max below, so the first sample replaces both. Named so the sentinel
// isn't a bare literal propagated into cached ranges.
constexpr float32 kHeightRangeSeedMin = 1e30f;
constexpr float32 kHeightRangeSeedMax = -1e30f;

// Monotonic count of per-tile min/max full scans, for the perf-regression oracle
// (GetTileHeightRescanCountForTests). Idle frames must not increment it; a region
// edit must increment it by the touched-tile count, never the resident-tile count.
std::atomic<uint64> g_TileHeightRescanCount{0};

// Monotonic count of procedural-splat texels renormalized by the spread settle flush
// (RenormalizeTileSplatRows), for the settle-spike bound oracle: no single settle frame
// may renormalize more than the per-frame texel budget (+ one row of round-up slop), and
// the total across a settle equals the resident tiles' splat area (each texel once).
std::atomic<uint64> g_SplatRenormalizeTexelCount{0};

// Monotonic count of change-gate openings that reached GatherModifiers, for the
// quiet-frame oracle (GetGatherCountForTests). An idle frame must not increment it.
std::atomic<uint64> g_GatherCount{0};

// Kill switch for the incremental per-tile height range (block grid).
// GE_TERRAIN_INCREMENTAL_MINMAX=0 forces every region range refresh back to a full
// O(tile-samples) rescan — the pre-change behavior and the A/B measurement lever.
// Default ON.
bool IncrementalTileRangeEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("GE_TERRAIN_INCREMENTAL_MINMAX");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// Kill switch for fanning the large-region modifier bake (per-tile base + modifier
// + splat re-eval) across the job pool. GE_TERRAIN_MODIFIER_PARALLEL=0 forces the
// serial per-tile path — the A/B lever + safety valve. Default ON.
bool ModifierParallelBakeEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("GE_TERRAIN_MODIFIER_PARALLEL");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// GE_TERRAIN_BAKE_TIMING=1 logs the per-bake CPU cost split (height + splat pass
// ms, touched-tile count, parallel/serial). The engine-side GE_CPU_PROFILE scopes
// record to a CpuProfiler instance the editor's reader can't see across the
// static-lib singleton boundary, so this is the only editor-side attribution.
bool BakeTimingEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("GE_TERRAIN_BAKE_TIMING");
        return v && v[0] != '0';
    }();
    return enabled;
}

// Minimum total touched-texel count to justify the parallel dispatch. Below it the
// serial path runs (a brush dab is ~2k texels/tile — the fan-out would cost more
// than the work); a large-radius modifier edit is ~10^5+ texels and fans out.
static constexpr uint64 kParallelBakeMinTexels = 96u * 1024u;

// Blocks-per-axis of the coarse per-tile height min/max grid (TerrainTileData::
// HeightBlock*). 8x8 = 64 blocks per tile: a 48x48 sculpt dab on a 1025^2 tile
// overlaps ~1-2 of the 128x128 blocks, so a region range refresh rescans ~2 blocks
// (~32k reads) instead of the full ~1M-sample tile — the profiled ~4.6 ms/dab hitch
// collapses to ~0.2 ms while the tile CachedMin/MaxH stays byte-identical to a full
// scan (blocks partition the tile exactly). Larger dims shrink the rescan but grow
// the O(dim^2) aggregate; 8 balances a single-tile brush against a tile-spanning drag.
static constexpr uint32 kHeightRangeBlockDim = 8;

// Sample-column [x0,x1) spanned by block index b along an axis of `dim` samples
// partitioned into `blocks` blocks. Integer split covers [0,dim) exactly (no gaps,
// no overlap), so summing the per-block extremes reproduces a full-tile min/max.
static inline void HeightBlockSampleSpan(uint32 b, uint32 blocks, uint32 dim,
                                         uint32& x0, uint32& x1)
{
    x0 = static_cast<uint32>(static_cast<uint64>(b) * dim / blocks);
    x1 = static_cast<uint32>(static_cast<uint64>(b + 1) * dim / blocks);
}

// Rescan one block's cell rectangle and store its min/max into the grid slot.
static void RescanTileHeightBlock(TerrainTileData& tile, uint32 bx, uint32 bz)
{
    const auto& hf = tile.Heightfield;
    const uint32 w = hf.GetWidth();
    const uint32 h = hf.GetHeight();
    uint32 x0, x1, z0, z1;
    HeightBlockSampleSpan(bx, tile.HeightBlockDim, w, x0, x1);
    HeightBlockSampleSpan(bz, tile.HeightBlockDim, h, z0, z1);
    float32 lo = kHeightRangeSeedMin;
    float32 hi = kHeightRangeSeedMax;
    const float32* s = hf.GetRawSamples();
    for (uint32 z = z0; z < z1; ++z)
    {
        const float32* row = s + static_cast<std::size_t>(z) * w;
        for (uint32 x = x0; x < x1; ++x)
        {
            lo = std::min(lo, row[x]);
            hi = std::max(hi, row[x]);
        }
    }
    const std::size_t idx = static_cast<std::size_t>(bz) * tile.HeightBlockDim + bx;
    tile.HeightBlockMinH[idx] = lo;
    tile.HeightBlockMaxH[idx] = hi;
}

// Aggregate every block's cached min/max into the tile CachedMin/MaxH. O(blocks).
static void AggregateTileHeightBlocks(TerrainTileData& tile)
{
    float32 lo = kHeightRangeSeedMin;
    float32 hi = kHeightRangeSeedMax;
    for (std::size_t i = 0; i < tile.HeightBlockMinH.size(); ++i)
    {
        lo = std::min(lo, tile.HeightBlockMinH[i]);
        hi = std::max(hi, tile.HeightBlockMaxH[i]);
    }
    tile.CachedMinH = lo;
    tile.CachedMaxH = hi;
}

// (Re)build the whole per-tile block grid from the current heights + aggregate the
// tile range. O(tile samples) — paid on the first edit of a tile, a heightfield
// resize, or a streamed-in tile whose grid is stale; NOT per dab.
static void RebuildTileHeightBlocks(TerrainTileData& tile)
{
    const uint32 w = tile.Heightfield.GetWidth();
    const uint32 h = tile.Heightfield.GetHeight();
    const uint32 dim = std::min({kHeightRangeBlockDim, std::max(1u, w), std::max(1u, h)});
    tile.HeightBlockDim = dim;
    tile.HeightBlockMinH.assign(static_cast<std::size_t>(dim) * dim, kHeightRangeSeedMin);
    tile.HeightBlockMaxH.assign(static_cast<std::size_t>(dim) * dim, kHeightRangeSeedMax);
    for (uint32 bz = 0; bz < dim; ++bz)
        for (uint32 bx = 0; bx < dim; ++bx)
            RescanTileHeightBlock(tile, bx, bz);
    AggregateTileHeightBlocks(tile);
}

// Full-tile range refresh: rebuild the block grid + aggregate. Kept for the paths
// that change (or may change) the whole tile — first bake, streamed-in tile,
// GPU-bake readback adopt — where a targeted region isn't known.
void RefreshTileHeightRange(TerrainTileData& tile)
{
    GE_CPU_PROFILE_SCOPE("Terrain.TileHeightRescan");
    g_TileHeightRescanCount.fetch_add(1, std::memory_order_relaxed);
    RebuildTileHeightBlocks(tile);
}

// Region-scoped range refresh: rescan only the grid blocks the edited sample rect
// [minX,maxX]x[minZ,maxZ] (inclusive) overlaps, then re-aggregate. Byte-identical
// to RefreshTileHeightRange (blocks partition the tile; unedited blocks keep their
// exact extremes). Falls back to a full rebuild when the grid is absent or its dim
// no longer matches the heightfield (resize) — the one case a full scan is owed.
void RefreshTileHeightRangeRegion(TerrainTileData& tile,
                                  int32 minX, int32 minZ, int32 maxX, int32 maxZ)
{
    GE_CPU_PROFILE_SCOPE("Terrain.TileHeightRescan");
    g_TileHeightRescanCount.fetch_add(1, std::memory_order_relaxed);
    if (!IncrementalTileRangeEnabled())
    {
        RebuildTileHeightBlocks(tile); // A/B lever: full rescan every refresh.
        return;
    }
    const uint32 w = tile.Heightfield.GetWidth();
    const uint32 h = tile.Heightfield.GetHeight();
    const uint32 wantDim = std::min({kHeightRangeBlockDim, std::max(1u, w), std::max(1u, h)});
    const std::size_t wantCount = static_cast<std::size_t>(wantDim) * wantDim;
    if (tile.HeightBlockDim != wantDim || tile.HeightBlockMinH.size() != wantCount)
    {
        RebuildTileHeightBlocks(tile);
        return;
    }
    minX = std::clamp(minX, 0, static_cast<int32>(w) - 1);
    maxX = std::clamp(maxX, 0, static_cast<int32>(w) - 1);
    minZ = std::clamp(minZ, 0, static_cast<int32>(h) - 1);
    maxZ = std::clamp(maxZ, 0, static_cast<int32>(h) - 1);
    if (minX > maxX || minZ > maxZ)
        return;
    const uint32 dim = tile.HeightBlockDim;
    const uint32 bx0 = static_cast<uint32>(minX) * dim / w;
    const uint32 bx1 = static_cast<uint32>(maxX) * dim / w;
    const uint32 bz0 = static_cast<uint32>(minZ) * dim / h;
    const uint32 bz1 = static_cast<uint32>(maxZ) * dim / h;
    for (uint32 bz = bz0; bz <= bz1 && bz < dim; ++bz)
        for (uint32 bx = bx0; bx <= bx1 && bx < dim; ++bx)
            RescanTileHeightBlock(tile, bx, bz);
    AggregateTileHeightBlocks(tile);
}

// Aggregate the resident (Full) tiles' CACHED per-tile min/max into the global
// height range the splat normalization uses. O(tiles), NOT O(all samples): each
// tile's CachedMin/MaxH is kept current by the streaming integration (on load) and
// by RefreshTileHeightRange in the bakes (on edit), so this never re-scans samples.
// A per-frame O(all-samples) rescan here was the tiled-terrain idle/edit hammer
// (4 tiles * 1025^2 = millions of reads); revision/cache bookkeeping replaces it.
void ComputeResidentGlobalHeightRange(const TiledTerrainData& tiled,
                                      float32& outMinH, float32& outMaxH)
{
    // Floor the aggregate to the deterministic procedural-noise band
    // [0, kTileNoiseAmplitude] rather than seeding at the resident tiles' observed
    // extremes. The base never leaves the band (the noise by construction, a 16-bit
    // heightmap by its [0, 1] decode, a flat base at 0; a raw float heightmap outside
    // it widens the band as an authored edit does), so the bakes that ASSIGN
    // CachedGlobal* from this (BakeTiledFull / BakeTiledRegion) reproduce
    // [0, kTileNoiseAmplitude] regardless of which tiles are resident — matching
    // the TiledTerrainData seed the streaming job path uses, so a height edit no
    // longer re-arms residency drift for the whole terrain. Authored edits that
    // push samples outside the band widen it (the min/max below), keeping the
    // snow/rock height thresholds stable as the user carves.
    outMinH = 0.0f;
    outMaxH = kTileNoiseAmplitude;
    for (const auto& [coord, tilePtr] : tiled.Tiles)
    {
        if (!tilePtr || tilePtr->LodState != TileLodState::Full)
            continue;
        outMinH = std::min(outMinH, tilePtr->CachedMinH);
        outMaxH = std::max(outMaxH, tilePtr->CachedMaxH);
    }
}

// Kill switch for the modifier change gate: GE_TERRAIN_CHANGE_GATING=0
// restores the ungated per-frame gather + hash exactly (prefix-'0' parse,
// per the ChangeFilter::Enabled convention).
bool ChangeGatingEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("GE_TERRAIN_CHANGE_GATING");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// Kill switch for interactive geometry-drag re-bake coalescing:
// GE_TERRAIN_MODIFIER_COALESCE=0 restores the pre-change per-frame full-footprint
// re-bake (the before/after measurement lever and the runtime A/B). Default ON.
bool ModifierCoalesceEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("GE_TERRAIN_MODIFIER_COALESCE");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// Kill switch for the deferred stroke-settle splat renormalize (#526).
// GE_TERRAIN_DEFER_RESPLAT=0 restores the pre-#526 behavior: a range-shifting
// height edit renormalizes every resident tile's splat immediately, in the same
// frame, instead of deferring to a stroke-settle flush. It exists to A/B the
// deferral against the immediate path when diagnosing splat-normalization seams.
bool DeferSplatResplatEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("GE_TERRAIN_DEFER_RESPLAT");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// GE_TERRAIN_INCREMENTAL_QUADTREE=0 restores the pre-fix behavior: a height edit
// marks the whole global quadtree dirty (QuadtreeDirty), forcing a full rebuild
// that rescans EVERY resident tile's heightfield min/max per dab — O(all samples)
// per stroke frame, the dominant sculpt-stroke hitch on large tiled terrains. The
// default patches only the tiles the edit actually touched into the global
// quadtree, which is byte-identical to a full rebuild (untouched tiles' nodes are
// unchanged). Kept as a kill switch to A/B the two paths.
bool IncrementalQuadtreeEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("GE_TERRAIN_INCREMENTAL_QUADTREE");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// GE_TERRAIN_SPREAD_SETTLE=0 restores the pre-fix one-shot settle flush: the deferred
// whole-terrain splat renormalize runs for EVERY resident tile in the single settle
// frame — the ~1.4s stroke-release spike on large tiled terrains (a 16-tile 2 km
// terrain regenerates 16 x 1025^2 procedural-splat texels at once). The default spreads
// it across settle frames at a bounded texel budget, settling byte-identically over ~1s
// of wall-clock without any single frame hitching. Kept as an A/B lever.
bool SpreadSettleEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("GE_TERRAIN_SPREAD_SETTLE");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// Per-frame procedural-splat texel budget for the spread settle renormalize. Sized so a
// settle frame's splat regen stays well under the ~50 ms interactive frame target on
// DebugFast (one 1025^2 tile is ~1.05M texels, so this is ~1/4 of a tile per frame). The
// spread visits every resident tile's splat exactly once across as many frames as the
// budget requires, so a 16-tile terrain settles over ~1 s of frames instead of one spike.
// Per-frame work is bounded by this budget plus at most one texel row of round-up slop.
constexpr std::size_t kSplatRenormalizeTexelsPerFrame = 262144;

// One Changed<> probe per modifier archetype. Parameter edits stamp the
// modifier component column; moves stamp WorldTransform (self-moves through
// the entity's own write, parent-moves through the hierarchy's write into the
// subtree it recomputes — the hierarchy only visits chunks whose inputs
// changed, so WorldTransform stamps track real movement). Scoping the
// WorldTransform scan to the modifier archetype keeps both probes bounded by
// modifier chunk count. Detection only — no stamping (reads never stamp).
template<typename CompT>
bool ModifierChunksChanged(ECS::World& world, ECS::ChangeGate& gate)
{
    bool changed = false;
    {
        auto scan = world.Query<ECS::Read<CompT>>();
        scan.template Changed<CompT>(gate);
        scan.BatchEach([&](const CompT*, std::size_t) { changed = true; });
    }
    if (!changed)
    {
        auto scan = world.Query<ECS::Read<CompT>, ECS::Read<Components::WorldTransform>>();
        scan.template Changed<Components::WorldTransform>(gate);
        scan.BatchEach([&](const CompT*, const Components::WorldTransform*, std::size_t) {
            changed = true;
        });
    }
    return changed;
}

// Column-only probe for a component the gather reads as a passenger on a volume
// entity. An effect's region and transform belong to the volume, so its own
// column is the only part of it that can change independently — the volume's
// own probe above already covers the shared WorldTransform.
template<typename CompT>
bool EffectColumnChanged(ECS::World& world, ECS::ChangeGate& gate)
{
    bool changed = false;
    auto scan = world.Query<ECS::Read<CompT>>();
    scan.template Changed<CompT>(gate);
    scan.BatchEach([&](const CompT*, std::size_t) { changed = true; });
    return changed;
}

// Added/Removed/Disabled lifecycle probe for one modifier type. Removed<T> and
// GetDisabled<T> are the only detectors for a component that leaves the gather —
// removed, destroyed, or switched off (its own tag or its entity): the row leaves
// the queried archetypes, so the Changed<T> scans can't see it. Switching one
// back on stamps the chunk it lands in, which the scans do see.
template<typename CompT>
bool ModifierLifecycleChanged(ECS::World& world)
{
    return !world.GetAdded<CompT>().empty() || !world.GetRemoved<CompT>().empty() ||
           !world.GetDisabled<CompT>().empty();
}

// Fold each probe over the canonical component lists rather than restating them.
// `||` short-circuits, so a hit still skips the remaining scans.
template<typename... Ts>
bool AnyLifecycleChanged(ECS::World& world, ComponentTypeList<Ts...>)
{
    return (ModifierLifecycleChanged<Ts>(world) || ...);
}

template<typename... Ts>
bool AnyRootChunksChanged(ECS::World& world, ECS::ChangeGate& gate, ComponentTypeList<Ts...>)
{
    return (ModifierChunksChanged<Ts>(world, gate) || ...);
}

template<typename... Ts>
bool AnyEffectColumnsChanged(ECS::World& world, ECS::ChangeGate& gate, ComponentTypeList<Ts...>)
{
    return (EffectColumnChanged<Ts>(world, gate) || ...);
}

// ---- Spherical (planet) modifier bake helpers -------------------------------------------
// A local terrain modifier applies to a planet by projecting each atlas texel's WORLD DIRECTION
// into a tangent plane at the modifier's position on the sphere: the same shape falloff + value
// (noise / sculpt payload) the planar bake uses, evaluated in tangent-plane metres. Because the
// contribution is a pure function of world direction, two texels straddling a cube edge sample
// the same direction and get the identical offset — seam-free by construction (the #488/#490
// cross-face discipline). The sphere sculpt atlas stores WORLD METRES (additive over the base
// relief), so — unlike the planar heightfield's normalized [0,1] samples — there is no
// /heightScale here.

// Non-volume roots whose payload the sphere bake can apply: a sculpt zone's
// freehand payload. A paint zone needs a per-face sphere splat layer that does
// not exist yet, so it is surfaced honestly (a one-shot log + an Inspector
// notice), never silently dropped. A volume never reaches here — the overload
// below filters its effect stack instead. Keep this in lockstep with the gizmo's
// `sphereSupported` flags (TerrainModifierGizmo.cpp) and the inspector's
// SphereUnsupported note.
bool IsSphereSupportedModifier(ResolvedModifier::Type t)
{
    return t == ResolvedModifier::Type::SculptZone;
}

// Shape matters as well as type: SphereShapeWeight implements Circle and
// Rectangle in the tangent plane and has no spherical analogue of the spline
// SDF, so a supported TYPE carrying Shape::Spline would bake as a rectangle.
// It joins the unsupported set (one-shot warning) instead.
bool IsSphereSupportedModifier(const ResolvedModifier& mod)
{
    if (mod.Shape == Components::TerrainModifierShape::Spline)
        return false;
    // A global volume has no footprint to resolve into a spherical cap: the
    // placement machinery is built on a centre direction and a tangent frame,
    // and "everywhere on the planet" has neither. It joins the unsupported set
    // (one-shot warning) rather than baking as a 50 m rectangle.
    if (mod.GlobalScope)
        return false;
    if (mod.ModType != ResolvedModifier::Type::Volume)
        return IsSphereSupportedModifier(mod.ModType);
    // A volume bakes on a planet when at least one of its effects has a sphere
    // path. Effects that don't (paint, height offset, any POOLED effect) are
    // skipped and warned about by AppendSpherePlacements, exactly as an
    // unsupported modifier type is.
    for (const auto& fx : mod.Effects)
        if (!IsPooled(fx) &&
            (fx.EffectKind == ResolvedEffect::Kind::Noise ||
             fx.EffectKind == ResolvedEffect::Kind::Flatten ||
             fx.EffectKind == ResolvedEffect::Kind::Stamp))
            return true;
    return false;
}

// World-metre footprint radius of a modifier's shape (+ falloff) — the spherical cap extent.
//
// Only meaningful for a modifier with a footprint. A global volume has none, and
// the number this returns for one (its authored rect extents) is meaningless —
// safe today because every caller is behind IsSphereSupportedModifier, which
// refuses a global. The one unguarded call records it into ModifierBakeSnapshot,
// whose readers are the sphere region diff, gated by the snapshot's own
// SphereSupported flag. Anything that starts reading this on the planar path
// must test ResolvedModifier::GlobalScope first.
float32 ModifierFootprintRadius(const ResolvedModifier& mod)
{
    const float32 base = mod.Shape == Components::TerrainModifierShape::Circle
                             ? mod.Radius
                             : std::max(mod.RectHalfX, mod.RectHalfZ);
    return base + std::max(mod.Falloff, 0.0f);
}

// What a sphere placement applies inside its region. A volume yields one
// placement per height effect and a sculpt zone one for its payload, so this is
// an effect kind rather than a modifier type.
enum class SpherePlacementKind : uint8 { Flatten, Noise, Stamp, SculptZone };

// A modifier resolved onto the sphere: centre direction + tangent frame + shape + value params.
struct SphereModifierPlacement
{
    SpherePlacementKind Type;
    std::array<float32, 3> N{};   // centre direction = normalize(worldPos)
    std::array<float32, 3> E1{};  // tangent frame at N (AnyTangent) — the local X axis
    std::array<float32, 3> E2{};  // = cross(N, E1) — the local Z axis
    float32 CosYaw = 1.0f, SinYaw = 0.0f;
    Components::TerrainModifierShape Shape = Components::TerrainModifierShape::Circle;
    float32 Radius = 0.0f, RectHalfX = 0.0f, RectHalfZ = 0.0f, Falloff = 0.0f;
    // Volume shape refinements. FalloffInward is the inner half of the shared
    // ShapeFalloffWeight ramp; defaults = the pre-volume behaviour.
    float32 FalloffInward = 0.0f, MasterWeight = 1.0f;
    float32 PlanetRadius = 0.0f, FootprintRadius = 0.0f;
    // |worldPos| — the entity's own distance from the planet centre, which is
    // the radial analogue of a planar volume's entity-Y reference height.
    float32 CenterDistance = 0.0f;
    // noise
    float32 Frequency = 0.0f, Amplitude = 0.0f, Lacunarity = 2.0f, Persistence = 0.5f;
    uint32 Octaves = 1u, Seed = 1u;
    // Zeroed = disarmed, so a placement that never fills it bakes plain noise.
    Erosion::ErosionParams Erosion{};
    // sculpt-zone payload (offsets in world metres, sampled in tangent-plane UV)
    const float32* Offsets = nullptr;
    uint32 Width = 0u, Height = 0u;
    // flatten: radial target (metres from planet centre) the base relief is levelled toward.
    float32 TargetRadius = 0.0f;
    // stamp: R mask (world-metre displacement = HeightScale * mask) sampled in tangent-plane UV
    // over the (Radius|RectHalf)+Falloff extent, rotated by yaw + the stamp's own rotation.
    const float32* StampMask = nullptr;
    uint32 StampWidth = 0u, StampHeight = 0u;
    float32 StampHeightScale = 0.0f;
    float32 StampCosRot = 1.0f, StampSinRot = 0.0f, StampExtentX = 0.0f, StampExtentZ = 0.0f;
    Components::TerrainModifierBlend StampBlend = Components::TerrainModifierBlend::Add;
    bool Valid = false;
};

// The shape/frame half of a placement: everything that comes from the region
// rather than from what is being applied inside it. Valid stays false for a
// modifier at the planet centre, which has no direction.
SphereModifierPlacement MakeSphereFrame(const ResolvedModifier& mod, float32 planetRadius)
{
    SphereModifierPlacement p{};
    p.PlanetRadius = planetRadius;
    const float32 len = std::sqrt(mod.Position.x * mod.Position.x + mod.Position.y * mod.Position.y +
                                  mod.Position.z * mod.Position.z);
    if (len <= 0.0f)
        return p;
    p.CenterDistance = len;
    p.N = {mod.Position.x / len, mod.Position.y / len, mod.Position.z / len};
    p.E1 = CBTTerrain::AnyTangent(p.N);
    p.E2 = {p.N[1] * p.E1[2] - p.N[2] * p.E1[1], p.N[2] * p.E1[0] - p.N[0] * p.E1[2],
            p.N[0] * p.E1[1] - p.N[1] * p.E1[0]};
    p.CosYaw = std::cos(mod.YawRadians);
    p.SinYaw = std::sin(mod.YawRadians);
    p.Shape = mod.Shape;
    p.Radius = mod.Radius;
    p.RectHalfX = mod.RectHalfX;
    p.RectHalfZ = mod.RectHalfZ;
    p.Falloff = mod.Falloff;
    p.FalloffInward = mod.FalloffInward;
    p.MasterWeight = mod.Weight;
    p.FootprintRadius = ModifierFootprintRadius(mod);
    p.Valid = true;
    return p;
}

void FillSphereNoise(SphereModifierPlacement& p, const ModifierNoiseParams& params)
{
    p.Type = SpherePlacementKind::Noise;
    p.Frequency = params.Frequency;
    p.Amplitude = params.Amplitude;
    p.Octaves = params.Octaves;
    p.Seed = params.Seed != 0 ? params.Seed : 12345u;
    p.Lacunarity = params.Lacunarity;
    p.Persistence = params.Persistence;
    p.Erosion = params.Erosion;
}

// Radial analogue of planar flatten's target. UseVolumeHeight levels to the entity's distance from
// the planet centre — drag it in / out to raise / lower the pad, exactly like the planar entity-Y
// drag — plus TargetHeight as a signed radial offset; otherwise TargetHeight is measured from the
// nominal planet surface. (Spline volumes never reach here: Shape::Spline has no sphere analogue,
// so the reference is always the entity's own distance.)
void FillSphereFlattenEffect(SphereModifierPlacement& p, const ModifierFlattenParams& params)
{
    p.Type = SpherePlacementKind::Flatten;
    p.TargetRadius = params.UseVolumeHeight ? (p.CenterDistance + params.TargetHeight)
                                            : (p.PlanetRadius + params.TargetHeight);
}

void FillSphereStamp(SphereModifierPlacement& p, const ResolvedModifier& mod,
                     const ModifierStampParams& params)
{
    // Same tangent-plane 2D sample as SculptZone, but scaled by HeightScale, rotated by the
    // stamp's own rotation (on top of the entity yaw), and Add/Subtract-blended. A null mask is
    // the documented flat-white fallback (a flat raise / lower disc — no asset needed).
    p.Type = SpherePlacementKind::Stamp;
    p.StampMask = params.MaskTexels;
    p.StampWidth = params.MaskWidth;
    p.StampHeight = params.MaskHeight;
    p.StampHeightScale = params.HeightScale;
    p.StampBlend = params.Blend;
    const float32 rot = mod.YawRadians + params.Rotation * kDegToRad;
    p.StampCosRot = std::cos(-rot); // inverse-rotate the tangent-plane sample, mirror of the planar path
    p.StampSinRot = std::sin(-rot);
    const bool circle = mod.Shape == Components::TerrainModifierShape::Circle;
    p.StampExtentX = (circle ? mod.Radius : mod.RectHalfX) + mod.Falloff;
    p.StampExtentZ = (circle ? mod.Radius : mod.RectHalfZ) + mod.Falloff;
}

void FillSphereSculptZone(SphereModifierPlacement& p, const ModifierSculptZoneParams& params)
{
    p.Type = SpherePlacementKind::SculptZone;
    p.Offsets = params.Offsets;
    p.Width = params.Width;
    p.Height = params.Height;
}

// Every sphere placement one modifier contributes: one for a sculpt zone,
// one per sphere-bakeable effect for a volume (in stack order — the sphere store
// accumulates additively, but flatten's lerp-to-target is order-sensitive).
// `outUnsupported` is set when the modifier carries anything the sphere path
// cannot bake, so the caller's one-shot warning still fires for a volume whose
// stack is only partly bakeable.
void AppendSpherePlacements(const ResolvedModifier& mod, float32 planetRadius,
                            std::vector<SphereModifierPlacement>& out, bool& outUnsupported)
{
    const SphereModifierPlacement frame = MakeSphereFrame(mod, planetRadius);
    if (!frame.Valid)
        return;

    if (mod.ModType != ResolvedModifier::Type::Volume)
    {
        if (mod.ModType != ResolvedModifier::Type::SculptZone)
        {
            outUnsupported = true;
            return;
        }
        SphereModifierPlacement p = frame;
        FillSphereSculptZone(p, mod.SculptZone);
        out.push_back(p);
        return;
    }

    for (const auto& fx : mod.Effects)
    {
        // A pool accumulates across its members over a bake REGION and applies
        // once; the sphere bakes cube-face rects of a sculpt layer and has no
        // such region to accumulate into. Refused here, where the placement
        // would otherwise be filled: SphereModifierPlacement carries no blend
        // for a flatten at all, so a pooled one would bake as a plain radial
        // level — the silent wrong answer rather than an honest refusal.
        if (IsPooled(fx))
        {
            outUnsupported = true;
            continue;
        }

        SphereModifierPlacement p = frame;
        switch (fx.EffectKind)
        {
        case ResolvedEffect::Kind::Noise:   FillSphereNoise(p, fx.Noise); break;
        case ResolvedEffect::Kind::Flatten: FillSphereFlattenEffect(p, fx.Flatten); break;
        case ResolvedEffect::Kind::Stamp:   FillSphereStamp(p, mod, fx.Stamp); break;
        // Paint needs a per-face sphere splat layer, and a constant height offset
        // has no sphere twin yet: surfaced, never silently applied.
        case ResolvedEffect::Kind::PaintLayer:
        case ResolvedEffect::Kind::HeightOffset:
            outUnsupported = true;
            continue;
        }
        out.push_back(p);
    }
}

// Shape falloff weight [0,1] at tangent-plane local coords (l0,l1) metres. Shares
// ShapeFalloffWeight with ComputeWeight, so a rim reads the same on a planet as on
// a plane by construction.
float32 SphereShapeWeight(const SphereModifierPlacement& p, float32 l0, float32 l1)
{
    float32 distFromEdge;
    if (p.Shape == Components::TerrainModifierShape::Circle)
    {
        distFromEdge = p.Radius - std::sqrt(l0 * l0 + l1 * l1);
    }
    else // Rectangle: rotate into the modifier's local frame (R(-yaw)).
    {
        const float32 lx = std::abs(p.CosYaw * l0 + p.SinYaw * l1);
        const float32 lz = std::abs(-p.SinYaw * l0 + p.CosYaw * l1);
        distFromEdge = std::min(p.RectHalfX - lx, p.RectHalfZ - lz);
    }
    return ShapeFalloffWeight(distFromEdge, p.Falloff, p.FalloffInward) * p.MasterWeight;
}

// Height offset (metres) this modifier contributes at UNIT world direction (dx,dy,dz). reliefAtDir
// is the closed-form base relief (metres) at this direction — used only by Flatten to cancel the
// relief so the pad lands flat; 0 is fine for the other types.
//
// BLEND SUPPORT ON THE SPHERE. Each placement contributes independently and the caller SUMS the
// results, so there is no accumulated height for an operator to read: only modes expressible as
// a self-contained offset work here. That is Add and Subtract; Set already falls through to Add
// on the stamp branch for exactly this reason, and Min / Max / SmoothMin / SmoothMax — whose
// operand IS the height already there — fall through the same way. Supporting them means turning
// the summation at the BakePlanetModifierRegions call site into an ordered fold over a running
// height, which is a change to the sphere bake's contract, not to this function.
float32 EvaluateSpherePlacement(const SphereModifierPlacement& p, float32 dx, float32 dy, float32 dz,
                                float32 reliefAtDir)
{
    if (!p.Valid)
        return 0.0f;
    // Reject the far hemisphere: only directions near N project meaningfully into the tangent plane.
    if (dx * p.N[0] + dy * p.N[1] + dz * p.N[2] <= 0.0f)
        return 0.0f;
    const float32 l0 = p.PlanetRadius * (dx * p.E1[0] + dy * p.E1[1] + dz * p.E1[2]);
    const float32 l1 = p.PlanetRadius * (dx * p.E2[0] + dy * p.E2[1] + dz * p.E2[2]);
    const float32 weight = SphereShapeWeight(p, l0, l1);
    if (weight <= 0.0f)
        return 0.0f;
    if (p.Type == SpherePlacementKind::Flatten)
    {
        // The additive offset that makes planetRadius + relief + offset == TargetRadius, feathered
        // by weight → lerp(planetRadius + relief, TargetRadius, weight), the radial analogue of the
        // planar h' = lerp(h, target, weight). Because reliefAtDir is the same closed-form the GPU
        // displaces with, the pad is flat (to CPU/GPU float parity).
        return (p.TargetRadius - p.PlanetRadius - reliefAtDir) * weight;
    }
    if (p.Type == SpherePlacementKind::Noise)
    {
        // Frequency = cycles across the footprint diameter (matches the planar freq/worldSize intuition).
        const float32 freqScale =
            p.FootprintRadius > 0.0f ? p.Frequency / (2.0f * p.FootprintRadius) : p.Frequency;
        // Same erosion gate as the planar bake: strength 0 keeps the untouched
        // fBM call so the sphere stays byte-identical when the block is off.
        const float32 noiseVal = p.Erosion.Strength > 0.0f
            ? Erosion::ErodedFBMNoise2D(l0, l1, freqScale, p.Amplitude, p.Octaves, p.Seed,
                                      p.Lacunarity, p.Persistence, p.Erosion)
            : Noise::FBMNoise2D(l0, l1, freqScale, p.Amplitude, p.Octaves, p.Seed, p.Lacunarity,
                         p.Persistence);
        return noiseVal * weight;
    }
    if (p.Type == SpherePlacementKind::Stamp)
    {
        // Tangent-plane bilinear sample (mirror of SculptZone), extent-normalized and inverse-
        // rotated by the stamp rotation; null mask = flat white (a uniform disc). HeightScale scales
        // the [0,1] mask to world metres. Only Add / Subtract are honoured on the sphere; every
        // mode needing a base-height read falls through to Add (see the header comment).
        float32 maskValue = 1.0f;
        if (p.StampMask != nullptr && p.StampWidth != 0u && p.StampHeight != 0u)
        {
            const float32 slx = p.StampCosRot * l0 - p.StampSinRot * l1;
            const float32 slz = p.StampSinRot * l0 + p.StampCosRot * l1;
            const float32 invX = p.StampExtentX > 0.0f ? 1.0f / p.StampExtentX : 0.0f;
            const float32 invZ = p.StampExtentZ > 0.0f ? 1.0f / p.StampExtentZ : 0.0f;
            maskValue = SampleMaskBilinear(p.StampMask, p.StampWidth, p.StampHeight,
                                           0.5f + 0.5f * slx * invX, 0.5f + 0.5f * slz * invZ);
        }
        const float32 disp = p.StampHeightScale * maskValue * weight;
        return p.StampBlend == Components::TerrainModifierBlend::Subtract ? -disp : disp;
    }
    // SculptZone: sample the R32F offset payload (world metres) in tangent-plane UV.
    if (p.Offsets == nullptr || p.Width == 0u || p.Height == 0u)
        return 0.0f;
    const float32 lx = p.CosYaw * l0 + p.SinYaw * l1;
    const float32 lz = -p.SinYaw * l0 + p.CosYaw * l1;
    const float32 invX = p.RectHalfX > 0.0f ? 1.0f / p.RectHalfX : 0.0f;
    const float32 invZ = p.RectHalfZ > 0.0f ? 1.0f / p.RectHalfZ : 0.0f;
    const float32 offset = SampleMaskBilinear(p.Offsets, p.Width, p.Height,
                                              0.5f + 0.5f * lx * invX, 0.5f + 0.5f * lz * invZ);
    return offset * weight;
}

// The (face, UV rect) regions a modifier at `worldPos` with `footprintRadius` world metres touches
// on a planet of `planetRadius` — the spherical cap classification, exactly like a #488 dab.
// `virtualDim` is the live sculpt grid dim: the sampled-AABB classification undershoots the
// toward-edge cap extent by up to ~0.13*angular, so the rect is inflated by the SAME margin
// ApplyDab uses (0.3*angular + 2 texels, expressed in UV) — without it an off-centre bake clips
// the modifier's rim by up to ~13% of its radius (the S1 secondary find). The per-texel eval
// inside the bake decides the actual writes, so the inflation only widens the SET region.
CBTTerrain::SphereEditRegions ModifierFaceRegions(const Mathematics::Vector3& worldPos,
                                                  float32 footprintRadius, float32 planetRadius,
                                                  uint32 virtualDim)
{
    if (planetRadius <= 0.0f)
        return {};
    const float32 kPi = 3.14159265358979323846f;
    const float32 angular = std::min(footprintRadius / planetRadius, kPi);
    CBTTerrain::SphereEditRegions regions =
        CBTTerrain::ClassifySphereCapEdit(worldPos.x, worldPos.y, worldPos.z, angular);
    const float32 texelUV = virtualDim > 1u ? 1.0f / static_cast<float32>(virtualDim - 1u) : 0.0f;
    const float32 marginUV = 0.3f * angular + 2.0f * texelUV;
    for (uint32 i = 0; i < regions.Count; ++i)
    {
        CBTTerrain::SphereFaceUVRect& r = regions.Rects[i];
        r.MinU = std::max(r.MinU - marginUV, 0.0f);
        r.MinV = std::max(r.MinV - marginUV, 0.0f);
        r.MaxU = std::min(r.MaxU + marginUV, 1.0f);
        r.MaxV = std::min(r.MaxV + marginUV, 1.0f);
    }
    return regions;
}

// Union `src`'s face rects into `acc` (per-face merge, bounded by the six cube faces).
void UnionSphereRegions(CBTTerrain::SphereEditRegions& acc, const CBTTerrain::SphereEditRegions& src)
{
    for (uint32 i = 0; i < src.Count; ++i)
    {
        const CBTTerrain::SphereFaceUVRect& r = src.Rects[i];
        bool merged = false;
        for (uint32 j = 0; j < acc.Count; ++j)
            if (acc.Rects[j].Face == r.Face)
            {
                acc.Rects[j].MinU = std::min(acc.Rects[j].MinU, r.MinU);
                acc.Rects[j].MinV = std::min(acc.Rects[j].MinV, r.MinV);
                acc.Rects[j].MaxU = std::max(acc.Rects[j].MaxU, r.MaxU);
                acc.Rects[j].MaxV = std::max(acc.Rects[j].MaxV, r.MaxV);
                merged = true;
                break;
            }
        if (!merged && acc.Count < acc.Rects.size())
            acc.Rects[acc.Count++] = r;
    }
}

} // anonymous namespace

bool TerrainModifierSystem::ShouldGather(ECS::World& world, TerrainService& terrainService)
{
    // Consume the asset-evict force flag before any early-out so it can't
    // stick across a gating toggle. A hot-reloaded mask/heightmap changes no
    // ECS state, so no Changed<> scan or lifecycle event can see it.
    const bool forceFromAssetEvict = m_ForceGatherAfterAssetEvict;
    m_ForceGatherAfterAssetEvict = false;

    if (!ChangeGatingEnabled() || !ECS::ChangeFilter::Enabled())
        return true;

    // M14 gate contract: entry-sample before the scans, write back the SAME
    // sample after — never an end-of-run resample (a concurrent writer's
    // stamp could land between and be skipped permanently).
    const uint64 entryVersion = world.GetGlobalSystemVersion();
    const uint64 swapGeneration = world.GetLifecycleSwapGeneration();
    const uint64 resetGeneration = world.GetLifecycleResetGeneration();

    bool anyChanged = forceFromAssetEvict;

    // Structural fallbacks force one full run: the first gated run (gate
    // zero-init would see everything anyway, but the lifecycle baselines are
    // meaningless), a lifecycle window gap > 1 (events were promoted and
    // discarded unseen — the swap-generation guard), and World::Clear (wipes
    // both event windows without per-entity events).
    if (!m_ChangeGateValid)
        anyChanged = true;
    else if (swapGeneration - m_LastLifecycleSwapGeneration > 1
             || resetGeneration != m_LastLifecycleResetGeneration)
        anyChanged = true;

    // Added/Removed lifecycle events for every component the gather reads.
    if (!anyChanged)
    {
        anyChanged = AnyLifecycleChanged(world, ModifierRootComponents{})
                  || AnyLifecycleChanged(world, ModifierEffectComponents{});
    }

    // Changed<> scans over the modifier archetypes. These see component and
    // transform edits only: column versions are per (chunk, column), so a spline
    // control-point edit — which mutates SplineService data and stamps no ECS
    // column at all — is invisible here. The spline edit epoch below is its
    // signal.
    if (!anyChanged)
    {
        anyChanged = AnyRootChunksChanged(world, m_ChangeGate, ModifierRootComponents{})
                  || AnyEffectColumnsChanged(world, m_ChangeGate, ModifierEffectComponents{});
    }

    // Terrain-side signal: config/handle churn, terrain moves, and tiled tile
    // streaming live in TerrainService state and the Terrain component (whose
    // column is write-stamped by extraction every frame, so a Changed<Terrain>
    // scan is no discriminator). The hash is bounded by terrain-entity count —
    // a handful — unlike the modifier gather it gates.
    const uint64 terrainStateHash = ComputeTerrainStateHash(world, terrainService);
    if (terrainStateHash != m_LastSeenTerrainStateHash)
        anyChanged = true;
    m_LastSeenTerrainStateHash = terrainStateHash;

    // Streamed-tile wake: a tile that just streamed in carries the base but no
    // modifiers, and its residency change is deliberately absent from the hash
    // above (that would force a full re-bake of every tile). Wake explicitly so
    // the scoped new-tile bake runs. Only scanned when nothing else already
    // demands a gather.
    if (!anyChanged && AnyTiledTileNeedsModifierBake(world, terrainService))
        anyChanged = true;
    // A terrain that would page waits for its height page overlay (TiledTerrainData::AwaitsPageOverlay).
    if (!anyChanged && AnyTiledTerrainAwaitsPageOverlay(world, terrainService))
        anyChanged = true;

    // Zone-payload edit epoch: a brush stroke mutates the service store and no
    // ECS component, so the Changed<>/lifecycle scans above can't see it. The
    // epoch bumps on any zone-payload edit/create/evict/restore.
    const uint64 zonePayloadEpoch = terrainService.GetZonePayloadEditEpoch();
    if (!m_HasZonePayloadEpochBaseline || zonePayloadEpoch != m_LastSeenZonePayloadEpoch)
        anyChanged = true;
    m_LastSeenZonePayloadEpoch = zonePayloadEpoch;
    m_HasZonePayloadEpochBaseline = true;

    // Spline edit epoch: dragging a control point rewrites SplineService data and
    // no ECS component, so — exactly like a zone brush stroke — nothing above can
    // see it. SplineService bumps the epoch when a spline cache is actually
    // rebuilt, and the schedule runs SplineExtraction before this system so the
    // bump is visible on the same frame as the edit. Any spline edit wakes the
    // gather; the modifier hash then decides whether a bake follows, so splines
    // no modifier references cost one gather, not a bake.
    const uint64 splineEpoch =
        SplineECS::SplineService::TryGet() ? SplineECS::SplineService::Get().GetEditEpoch() : 0;
    if (!m_HasSplineEpochBaseline || splineEpoch != m_LastSeenSplineEpoch)
        anyChanged = true;
    m_LastSeenSplineEpoch = splineEpoch;
    m_HasSplineEpochBaseline = true;

    // Entry-sample write-back either way: if nothing changed, no stamp exists
    // in (gate, entry]; if the gather runs, it reads everything fresh, and
    // stamps landing during the run compare greater next frame. Short-
    // circuited scans are consumed the same way — safe, because a hit means
    // the full gather runs this frame anyway.
    m_ChangeGate.LastRunVersion = entryVersion;
    m_ChangeGateValid = true;
    m_LastLifecycleSwapGeneration = swapGeneration;
    m_LastLifecycleResetGeneration = resetGeneration;

    return anyChanged;
}

uint64 TerrainModifierSystem::HashModifierState(const ResolvedModifier& mod)
{
    uint64 hash = HashModifierGeometry(mod);
    // Fold the zone payload version so a brush stroke (same transform, mutated
    // payload) still trips the combined change hash and bakes. The geometry
    // hash deliberately omits it so the region diff can scope a stroke to its
    // payload sub-rect (design §3.2).
    if (ResolvedModifier::IsZone(mod.ModType))
        hash = HashCombine(hash, mod.DataVersion);
    return hash;
}

namespace
{

// Feeds every input of one modifier's bake to `sink`, in a fixed order: the one
// field list behind both the in-session change hash (HashModifierGeometry) and the
// portable bake key (ModifierBakeKeySink). The sink decides how the three inputs
// that live outside the modifier are folded: the resolved spline, a stamp's mask
// and a zone's payload (the change hash folds their versions, the key their
// content). Numeric fields go through Value, floats through Float (raw bits).
template <typename Sink>
void VisitModifierInputs(const ResolvedModifier& mod, Sink& sink)
{
    sink.Value(static_cast<uint64>(mod.ModType));
    sink.Value(static_cast<uint64>(mod.Shape));
    sink.Float(mod.Position.x);
    sink.Float(mod.Position.y);
    sink.Float(mod.Position.z);
    sink.Float(mod.YawRadians);
    sink.Float(mod.Radius);
    sink.Float(mod.Falloff);
    sink.Float(mod.Priority);
    sink.Float(mod.RectHalfX);
    sink.Float(mod.RectHalfZ);

    // Volume-only shape refinements. Folded for every type; a zone leaves them
    // at their constant defaults.
    sink.Float(mod.FalloffInward);
    sink.Float(mod.Weight);
    sink.Value(static_cast<uint64>(mod.SplineFillInterior ? 1u : 0u));
    // The spacing decides the station polyline a SplinePath volume's whole bake
    // — weight, reference height and bounds alike — is evaluated against, so an
    // edit to it changes the output and must re-bake.
    sink.Float(mod.StationSpacing);
    // Global scope resolves to the same TerrainModifierShape as Rectangle, so
    // without this a rectangle switched to Global (or back) would hash
    // identically and the change gate would never re-bake it.
    sink.Value(static_cast<uint64>(mod.GlobalScope ? 1u : 0u));

    // Spline-region state belongs to the SHAPE, not the modifier type: any
    // modifier whose Shape is Spline gets its footprint from the resolved spline,
    // so an edit to that spline has to move the hash.
    if (mod.ResolvedSpline)
        sink.Spline(*mod.ResolvedSpline);

    // A volume's output is its whole ordered effect stack: every parameter, and
    // the order itself (two effects swapping StackOrder changes the result).
    //
    // A kind whose parameters are not folded below hashes only as its kind and
    // stack order, so editing any of its fields reproduces the previous bake's
    // hash and the change gate never re-bakes — silently, which is the class the
    // GlobalScope fold above was added to close.
    //
    // The assert is on Kind::Count, NOT on the last real enumerator: appending a
    // kind leaves every existing enumerator's value untouched, so asserting one
    // of those would pass for precisely the change it is meant to catch. MSVC's
    // unhandled-enumerator warning (C4062) is off by default, so the switch's
    // missing `default:` is not a guard on its own either.
    static_assert(static_cast<int32>(ResolvedEffect::Kind::Count) == 8,
                  "ResolvedEffect::Kind gained a member. Fold its parameters into the switch "
                  "below (and into the GPU packer), or edits to it will not re-bake.");

    if (mod.ModType == ResolvedModifier::Type::Volume)
    {
        for (const auto& fx : mod.Effects)
        {
            sink.Value(static_cast<uint64>(fx.EffectKind));
            sink.Value(static_cast<uint64>(static_cast<uint32>(fx.StackOrder)));
            switch (fx.EffectKind)
            {
            case ResolvedEffect::Kind::Flatten:
                sink.Float(fx.Flatten.TargetHeight);
                sink.Value(static_cast<uint64>(fx.Flatten.UseVolumeHeight ? 1u : 0u));
                sink.Value(static_cast<uint64>(fx.Flatten.Blend));
                sink.Float(fx.Flatten.BlendSmoothing);
                // Renaming a group re-partitions which effects average together,
                // and the claim mask is read per effect in every blend mode, so
                // both change the bake even where nothing else about the effect
                // moved.
                sink.Value(fx.Flatten.PoolGroup);
                sink.Value(static_cast<uint64>(fx.Flatten.RespectClaims ? 1u : 0u));
                break;
            case ResolvedEffect::Kind::HeightOffset:
                sink.Float(fx.HeightOffset.Offset);
                sink.Value(static_cast<uint64>(fx.HeightOffset.Blend));
                sink.Float(fx.HeightOffset.BlendSmoothing);
                sink.Value(fx.HeightOffset.PoolGroup);
                sink.Value(static_cast<uint64>(fx.HeightOffset.RespectClaims ? 1u : 0u));
                break;
            case ResolvedEffect::Kind::Noise:
                sink.Float(fx.Noise.Frequency);
                sink.Float(fx.Noise.Amplitude);
                sink.Value(static_cast<uint64>(fx.Noise.Octaves));
                sink.Value(static_cast<uint64>(fx.Noise.Seed));
                sink.Float(fx.Noise.Lacunarity);
                sink.Float(fx.Noise.Persistence);
                sink.Value(static_cast<uint64>(fx.Noise.Blend));
                sink.Float(fx.Noise.BlendSmoothing);
                sink.Float(fx.Noise.Erosion.Strength);
                sink.Value(static_cast<uint64>(fx.Noise.Erosion.Octaves));
                sink.Float(fx.Noise.Erosion.Frequency);
                sink.Float(fx.Noise.Erosion.Detail);
                sink.Float(fx.Noise.Erosion.GullyWeight);
                sink.Float(fx.Noise.Erosion.EdgeRounding);
                sink.Float(fx.Noise.Erosion.Fade);
                sink.Value(fx.Noise.PoolGroup);
                sink.Value(static_cast<uint64>(fx.Noise.RespectClaims ? 1u : 0u));
                break;
            case ResolvedEffect::Kind::Stamp:
                sink.Float(fx.Stamp.HeightScale);
                sink.Float(fx.Stamp.Rotation);
                sink.Value(static_cast<uint64>(fx.Stamp.Blend));
                sink.Float(fx.Stamp.BlendSmoothing);
                sink.StampMask(fx.Stamp);
                sink.Value(fx.Stamp.PoolGroup);
                sink.Value(static_cast<uint64>(fx.Stamp.RespectClaims ? 1u : 0u));
                break;
            case ResolvedEffect::Kind::PaintLayer:
                sink.Value(static_cast<uint64>(fx.PaintLayer.LayerIndex));
                sink.Float(fx.PaintLayer.Strength);
                sink.Value(static_cast<uint64>(fx.PaintLayer.Replace));
                sink.Value(static_cast<uint64>(fx.PaintLayer.RespectClaims ? 1u : 0u));
                break;
            case ResolvedEffect::Kind::GroundClaim:
                sink.Float(fx.GroundClaim.Strength);
                break;
            case ResolvedEffect::Kind::Grass:
                sink.Float(fx.Grass.HeightScale);
                sink.Float(fx.Grass.DensityScale);
                break;
            case ResolvedEffect::Kind::Rules:
                // Every authored field of every live row: dragging one band
                // handle must move this hash, or the edit bakes never.
                sink.Value(static_cast<uint64>(fx.Rules.RespectClaims ? 1u : 0u));
                sink.Value(static_cast<uint64>(fx.Rules.RuleCount));
                for (uint32 r = 0; r < fx.Rules.RuleCount; ++r)
                {
                    const Components::TerrainSurfaceRule& rule = fx.Rules.Rules[r];
                    sink.Value(static_cast<uint64>(rule.MaterialSlot));
                    sink.Float(rule.Strength);
                    sink.Value(static_cast<uint64>(rule.Replace));
                    sink.Value(static_cast<uint64>(rule.ConditionCount));
                    for (uint32 c = 0; c < rule.ConditionCount; ++c)
                    {
                        const Components::TerrainRuleCondition& cond = rule.Conditions[c];
                        sink.Value(static_cast<uint64>(cond.Kind));
                        sink.Value(static_cast<uint64>(cond.FalloffCurve));
                        sink.Float(cond.Min);
                        sink.Float(cond.Max);
                        sink.Float(cond.Feather);
                        // Only where they are read — which is also the only
                        // place they are serialized. Folding them for a slope
                        // band would rebake on an edit that cannot change a
                        // texel, and would move the hash across a save/load
                        // round trip that legitimately drops them.
                        if (cond.Kind == Components::TerrainRuleConditionKind::Noise)
                        {
                            sink.Float(cond.NoiseFrequency);
                            sink.Value(static_cast<uint64>(cond.NoiseSeed));
                        }
                    }
                }
                break;
            }
        }
        return;
    }

    switch (mod.ModType)
    {
    case ResolvedModifier::Type::Volume:
        break; // handled above
    case ResolvedModifier::Type::SculptZone:
        sink.Value(static_cast<uint64>(mod.SculptZone.Blend));
        sink.Value(static_cast<uint64>(mod.SculptZone.Width));
        sink.Value(static_cast<uint64>(mod.SculptZone.Height));
        break;
    case ResolvedModifier::Type::PaintZone:
        sink.Value(static_cast<uint64>(mod.PaintZone.LayerIndex));
        sink.Float(mod.PaintZone.Strength);
        sink.Value(static_cast<uint64>(mod.PaintZone.Width));
        sink.Value(static_cast<uint64>(mod.PaintZone.Height));
        break;
    }
    sink.ZonePayload(mod);
}

// The in-session change hash's sink: HashCombine over the values, with the spline,
// the stamp mask and the zone payload folded as their per-process versions.
struct ModifierChangeHashSink
{
    uint64 Hash = 0;
    void Value(uint64 value) { Hash = HashCombine(Hash, value); }
    void Float(float32 value)
    {
        uint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        Value(bits);
    }
    // TotalArcLength is a cheap proxy for the entity transform rotating or scaling
    // the world-space copy (Version only tracks edits to the authored points).
    void Spline(const Spline::SplineData& spline)
    {
        Value(spline.Version);
        Float(spline.TotalArcLength);
    }
    void StampMask(const ModifierStampParams& stamp)
    {
        Value(stamp.MaskGuidHash);
        Value(stamp.MaskVersion);
    }
    // The zone payload version is folded by HashModifierState, not here: the region
    // diff tells a moved zone from a pure brush stroke by this hash.
    void ZonePayload(const ResolvedModifier&) {}
};

// The portable bake key's sink: the same values, with the spline, the stamp mask
// and the zone payload folded as their content, so every process baking the same
// scene computes the same key.
struct ModifierBakeKeySink
{
    TerrainBakeKeyBuilder& Key;
    void Value(uint64 value) { Key.Value(value); }
    void Float(float32 value) { Key.Float(value); }
    void Spline(const Spline::SplineData& spline)
    {
        // Twelve 4-byte fields including the explicit, always-zero _Pad0: the
        // points' bytes are their values, with no compiler padding to vary.
        static_assert(sizeof(Spline::SplineControlPoint) == 72);
        Key.Value(static_cast<uint32>(spline.Type));
        Key.Value(static_cast<uint32>(spline.Closed ? 1u : 0u));
        Key.Value(spline.ArcLengthSamples);
        Key.Value(static_cast<uint64>(spline.Points.size()));
        Key.Bytes(spline.Points.data(), spline.Points.size() * sizeof(Spline::SplineControlPoint));
    }
    void StampMask(const ModifierStampParams& stamp)
    {
        const bool hasMask = stamp.MaskTexels != nullptr;
        Key.Value(hasMask ? stamp.MaskWidth : 0u);
        Key.Value(hasMask ? stamp.MaskHeight : 0u);
        if (hasMask)
            Key.Bytes(stamp.MaskTexels,
                      static_cast<std::size_t>(stamp.MaskWidth) * stamp.MaskHeight * sizeof(float32));
    }
    void ZonePayload(const ResolvedModifier& mod)
    {
        if (mod.ModType == ResolvedModifier::Type::SculptZone)
        {
            const bool loaded = mod.SculptZone.Offsets != nullptr;
            Key.Value(loaded ? mod.SculptZone.Width : 0u);
            Key.Value(loaded ? mod.SculptZone.Height : 0u);
            if (loaded)
                Key.Bytes(mod.SculptZone.Offsets,
                          static_cast<std::size_t>(mod.SculptZone.Width) * mod.SculptZone.Height * sizeof(float32));
        }
        else if (mod.ModType == ResolvedModifier::Type::PaintZone)
        {
            const bool loaded = mod.PaintZone.Mask != nullptr;
            Key.Value(loaded ? mod.PaintZone.Width : 0u);
            Key.Value(loaded ? mod.PaintZone.Height : 0u);
            if (loaded)
                Key.Bytes(mod.PaintZone.Mask, static_cast<std::size_t>(mod.PaintZone.Width) * mod.PaintZone.Height);
        }
    }
};

// The content key of one single terrain's full bake: the terrain's sample grid,
// placement and base source content, then every modifier in bake order.
uint64 ComputeSingleTerrainBakeKey(const std::vector<ResolvedModifier>& modifiers, const TerrainData& data,
                                   Components::TerrainBaseSource baseSource,
                                   const Terrain::HeightfieldData* baseHeightmap,
                                   float32 originX, float32 originY, float32 originZ)
{
    TerrainBakeKeyBuilder key;
    key.Value(data.Heightfield.GetWidth());
    key.Value(data.Heightfield.GetHeight());
    key.Float(data.Config.WorldSizeX);
    key.Float(data.Config.WorldSizeZ);
    key.Float(data.Config.HeightScale);
    key.Float(originX);
    key.Float(originY);
    key.Float(originZ);
    key.Value(static_cast<uint32>(baseSource));
    // Only a heightmap base reads content; a missing or undecodable one bakes
    // flat, which is its own key.
    const bool heightmapBase = baseSource == Components::TerrainBaseSource::HeightmapAsset &&
                               baseHeightmap != nullptr && !baseHeightmap->IsEmpty();
    key.Value(static_cast<uint32>(heightmapBase ? 1u : 0u));
    if (heightmapBase)
    {
        key.Value(baseHeightmap->GetWidth());
        key.Value(baseHeightmap->GetHeight());
        key.Bytes(baseHeightmap->GetRawSamples(), baseHeightmap->GetSampleCount() * sizeof(float32));
    }
    key.Value(static_cast<uint64>(modifiers.size()));
    ModifierBakeKeySink sink{key};
    for (const ResolvedModifier& mod : modifiers)
    {
        key.Value(static_cast<uint32>(mod.Enabled ? 1u : 0u));
        VisitModifierInputs(mod, sink);
    }
    return key.Key();
}

// Loads the terrain's artifact into `data` as a full bake would leave it when it
// holds `key`. False on any miss; a missing file or an earlier state of the terrain
// is the ordinary miss, anything else is logged.
bool ApplyCachedTerrainBake(TerrainData& data, const std::filesystem::path& file, uint64 key)
{
    TerrainBakeArtifact artifact;
    const TerrainBakeCacheStatus status =
        ReadTerrainBake(file, key, data.Heightfield.GetWidth(), data.Heightfield.GetHeight(), artifact);
    if (status != TerrainBakeCacheStatus::Hit)
    {
        if (status != TerrainBakeCacheStatus::Missing && status != TerrainBakeCacheStatus::KeyMismatch)
            Logger::Log::Warning("Terrain bake cache: '{}' rejected ({}); baking instead", file.string(),
                                 TerrainBakeCacheStatusName(status));
        return false;
    }
    std::memcpy(data.Heightfield.GetMutableSamples(), artifact.Heights.data(),
                artifact.Heights.size() * sizeof(float32));
    data.Splatmap = std::move(artifact.Splat);
    data.SplatmapWidth = artifact.Width;
    data.SplatmapHeight = artifact.Height;
    data.SplatBakeMinH = artifact.SplatMinH;
    data.SplatBakeMaxH = artifact.SplatMaxH;
    data.SplatBakeRangeValid = true;
    data.SplatmapFullDirty = true;
    return true;
}

// The full bake now in `data`, as an artifact.
TerrainBakeArtifact CopyTerrainBake(const TerrainData& data)
{
    TerrainBakeArtifact artifact;
    artifact.Width = data.Heightfield.GetWidth();
    artifact.Height = data.Heightfield.GetHeight();
    artifact.Heights.assign(data.Heightfield.GetRawSamples(),
                            data.Heightfield.GetRawSamples() + data.Heightfield.GetSampleCount());
    artifact.Splat = data.Splatmap;
    artifact.SplatMinH = data.SplatBakeMinH;
    artifact.SplatMaxH = data.SplatBakeMaxH;
    return artifact;
}

// Stores the full bake now in `data` under `key`, replacing the terrain's artifact.
void StoreTerrainBake(const TerrainData& data, const std::filesystem::path& file, uint64 key)
{
    const TerrainBakeArtifact artifact = CopyTerrainBake(data);
    if (!WriteTerrainBake(file, key, artifact))
        Logger::Log::Warning("Terrain bake cache: could not write '{}'; the next open bakes again", file.string());
}

} // namespace

uint64 TerrainModifierSystem::HashModifierGeometry(const ResolvedModifier& mod)
{
    ModifierChangeHashSink sink;
    VisitModifierInputs(mod, sink);
    return sink.Hash;
}

void TerrainModifierSystem::ClampRuleCountsToCaps(Components::TerrainSurfaceRulesEffect& block,
                                                 ECS::EntityHandle entity)
{
    bool overflowed = false;

    if (block.RuleCount > Components::kMaxTerrainSurfaceRules)
    {
        if (m_RuleCapOverflowWarned.insert(entity.id).second)
            Logger::Log::Error(
                "Terrain.SurfaceRules entity {} declares {} rules; the cap is {}. The extra rows "
                "are NOT baked. Split the rule set across a second rules modifier.",
                entity.id, block.RuleCount, Components::kMaxTerrainSurfaceRules);
        block.RuleCount = Components::kMaxTerrainSurfaceRules;
        overflowed = true;
    }

    for (uint32 i = 0; i < block.RuleCount; ++i)
    {
        if (block.Rules[i].ConditionCount <= Components::kMaxTerrainRuleConditions)
            continue;
        if (!overflowed && m_RuleCapOverflowWarned.insert(entity.id).second)
            Logger::Log::Error(
                "Terrain.SurfaceRules entity {} rule {} declares {} conditions; the cap is {}. The "
                "extra conditions are NOT evaluated, so the rule covers MORE ground than authored.",
                entity.id, i, block.Rules[i].ConditionCount, Components::kMaxTerrainRuleConditions);
        block.Rules[i].ConditionCount = static_cast<uint8>(Components::kMaxTerrainRuleConditions);
        overflowed = true;
    }

    // Warn once per entity while it stays broken, but forget it the moment the
    // counts are legal again: dedupe that never resets reports the SECOND
    // breakage as clean, and that is the bake the author is looking at.
    if (!overflowed)
        m_RuleCapOverflowWarned.erase(entity.id);
}

// Reading a new component type here means adding it to the canonical lists in
// TerrainModifierComponents.h — ShouldGather wakes the gather from those, so a
// type read here but absent there is a bake input the change gate cannot see.
// Two exceptions signal by other means: Components::Terrain via the terrain
// state hash, and SplineComponent via the spline epoch.
void TerrainModifierSystem::GatherModifiers(ECS::World& world,
                                             TerrainService& terrainService)
{
    // The one destination, by construction rather than by convention: the three
    // clears below free the storage every entry points into, so a gather that left
    // m_AppliedModifiers unrefilled would leave it holding dangling pointers for
    // the next ComposeTiledGroundBlock to read.
    std::vector<ResolvedModifier>& outModifiers = m_AppliedModifiers;
    outModifiers.clear();
    m_GrassModifiers.clear();
    m_TransformedSplines.clear();
    m_ResolvedRuleBlocks.clear();
    m_RoutePolylines.clear();

    auto* splineService = SplineECS::SplineService::TryGet();

    // A spline-shaped volume takes its region from the SplineComponent on the
    // same entity. Returns false when the volume has no region: an entity with
    // no SplineComponent at all is a scene-authoring error (warned once, by
    // entity, so it is a diagnosis rather than a silent zero); an entity whose
    // spline is switched off or merely not built yet is skipped quietly.
    auto resolveShape = [&](ResolvedModifier& mod,
                            const Components::SplineComponent* splineComp,
                            bool splineSwitchedOff,
                            const Components::WorldTransform& xf) -> bool
    {
        if (mod.Shape != Components::TerrainModifierShape::Spline)
            return true;
        if (splineSwitchedOff)
            return false;
        if (!splineComp)
        {
            if (m_SplineShapeUnresolvedWarned.insert(mod.Entity.id).second)
            {
                Logger::Log::Warning(
                    "TerrainModifierSystem: entity {} has a modifier with Shape=Spline but no "
                    "Spline component to define the path — add a Spline component to the entity, "
                    "or pick a Circle/Rectangle shape. The modifier is ignored.",
                    mod.Entity.id);
            }
            return false;
        }
        if (!splineService)
            return false;
        mod.ResolvedSpline = AppendWorldSpline(m_TransformedSplines, *splineService, *splineComp, xf);
        return mod.ResolvedSpline != nullptr;
    };

    // Collect a volume entity's effect components into stack order. An entity
    // holds at most one component of each type, so the stack is a permutation of
    // this fixed set; StackOrder (which the inspector writes from the visual
    // order of the effect sections) is what orders it. stable_sort keeps the
    // declaration order below for effects that share a StackOrder, so an
    // untouched stack is deterministic rather than merely usually-consistent.
    auto GatherVolumeEffects = [this](ECS::World& w, ECS::EntityHandle e,
                                      std::vector<ResolvedEffect>& out)
    {
        out.clear();
        // One slot per effect kind: reserved up front so `add`'s returned
        // reference survives the pushes that follow it. Folded from the canonical
        // list rather than restated, because a literal that fell behind it would
        // reallocate mid-gather and leave every earlier `add` holding a dangling
        // reference — silent corruption, not a compile error.
        out.reserve(kTerrainEffectTypeCount);
        auto add = [&out](ResolvedEffect::Kind kind, int32 stackOrder) -> ResolvedEffect& {
            ResolvedEffect fx{};
            fx.EffectKind = kind;
            fx.StackOrder = stackOrder;
            out.push_back(fx);
            return out.back();
        };

        if (const auto* c = w.GetComponent<Components::TerrainFlattenEffect>(e); c && c->Enabled)
        {
            auto& fx = add(ResolvedEffect::Kind::Flatten, c->StackOrder);
            fx.Flatten.TargetHeight = c->TargetHeight;
            fx.Flatten.UseVolumeHeight = c->UseVolumeHeight;
            fx.Flatten.Blend = c->Blend;
            fx.Flatten.BlendSmoothing = c->BlendSmoothing;
            // Union member: resolved for every flatten, not only the pooled ones,
            // so the storage never carries a neighbour's bytes.
            fx.Flatten.PoolGroup = ResolvePoolGroup(*c);
            fx.Flatten.RespectClaims = c->RespectClaims;
        }
        if (const auto* c = w.GetComponent<Components::TerrainHeightOffsetEffect>(e); c && c->Enabled)
        {
            auto& fx = add(ResolvedEffect::Kind::HeightOffset, c->StackOrder);
            fx.HeightOffset.Offset = c->Offset;
            fx.HeightOffset.Blend = c->Blend;
            fx.HeightOffset.BlendSmoothing = c->BlendSmoothing;
            // Union member: resolved for every effect, not only the pooled ones,
            // so the storage never carries a neighbour's bytes.
            fx.HeightOffset.PoolGroup = ResolvePoolGroup(*c);
            fx.HeightOffset.RespectClaims = c->RespectClaims;
        }
        if (const auto* c = w.GetComponent<Components::TerrainNoiseEffect>(e); c && c->Enabled)
        {
            auto& fx = add(ResolvedEffect::Kind::Noise, c->StackOrder);
            fx.Noise.Frequency = c->Frequency;
            fx.Noise.Amplitude = c->Amplitude;
            // Scene text is untrusted — the parser takes any uint32 and the
            // per-texel octave loops are unbounded. Clamp at this one point,
            // where authored values become bake params, so the change hash, the
            // GPU packing and the CPU bake all agree on the count actually run.
            fx.Noise.Octaves = std::min(c->Octaves, Components::kMaxNoiseOctaves);
            fx.Noise.Seed = c->Seed;
            fx.Noise.Lacunarity = c->Lacunarity;
            fx.Noise.Persistence = c->Persistence;
            fx.Noise.Blend = c->Blend;
            fx.Noise.BlendSmoothing = c->BlendSmoothing;
            fx.Noise.Erosion.Strength = c->ErosionStrength;
            fx.Noise.Erosion.Octaves = std::min(c->ErosionOctaves, Components::kMaxNoiseOctaves);
            fx.Noise.Erosion.Frequency = c->ErosionFrequency;
            fx.Noise.Erosion.Detail = c->ErosionDetail;
            fx.Noise.Erosion.GullyWeight = c->ErosionGullyWeight;
            fx.Noise.Erosion.EdgeRounding = c->ErosionEdgeRounding;
            fx.Noise.Erosion.Fade = c->ErosionFade;
            // Union member: resolved for every effect, not only the pooled ones,
            // so the storage never carries a neighbour's bytes.
            fx.Noise.PoolGroup = ResolvePoolGroup(*c);
            fx.Noise.RespectClaims = c->RespectClaims;
        }
        if (const auto* c = w.GetComponent<Components::TerrainStampEffect>(e); c && c->Enabled)
        {
            auto& fx = add(ResolvedEffect::Kind::Stamp, c->StackOrder);
            fx.Stamp.HeightScale = c->HeightScale;
            fx.Stamp.Rotation = c->Rotation;
            fx.Stamp.Blend = c->Blend;
            fx.Stamp.BlendSmoothing = c->BlendSmoothing;
            fx.Stamp.MaskTexels = nullptr;
            fx.Stamp.MaskWidth = 0;
            fx.Stamp.MaskHeight = 0;
            fx.Stamp.MaskGuidHash = 0;
            fx.Stamp.MaskVersion = 0;
            // Union member: resolved for every effect, not only the pooled ones,
            // so the storage never carries a neighbour's bytes.
            fx.Stamp.PoolGroup = ResolvePoolGroup(*c);
            fx.Stamp.RespectClaims = c->RespectClaims;

            const GUID maskGuid = c->StampAssetGuid.ToGuid();
            if (!maskGuid.IsNull())
            {
                // Synchronous decode at gather, cached by GUID and evicted on
                // hot-reload — masks are small and the gather is change-gated.
                const DecodedStampMask* mask = ResolveStampMask(maskGuid);
                fx.Stamp.MaskGuidHash = static_cast<uint64>(std::hash<GUID>{}(maskGuid));
                fx.Stamp.MaskVersion = mask->ContentVersion;
                if (!mask->LoadFailed)
                {
                    fx.Stamp.MaskTexels = mask->Texels.data();
                    fx.Stamp.MaskWidth = mask->Width;
                    fx.Stamp.MaskHeight = mask->Height;
                }
            }
        }
        if (const auto* c = w.GetComponent<Components::TerrainPaintLayerEffect>(e); c && c->Enabled)
        {
            auto& fx = add(ResolvedEffect::Kind::PaintLayer, c->StackOrder);
            fx.PaintLayer.LayerIndex = c->LayerIndex;
            fx.PaintLayer.Strength = c->Strength;
            fx.PaintLayer.Replace = c->Replace;
            fx.PaintLayer.RespectClaims = c->RespectClaims;
        }
        if (const auto* c = w.GetComponent<Components::TerrainSurfaceRulesEffect>(e); c && c->Enabled)
        {
            auto& fx = add(ResolvedEffect::Kind::Rules, c->StackOrder);
            // The authored block is copied into frame storage rather than
            // pointed at in place: the bake can run after the ECS column has
            // moved, and the copy is the same shape a resolved stamp mask has.
            //
            // Scene text and script are untrusted: the counts are plain integer
            // fields, so clamp them at this one point — where authored values
            // become bake params — and say so, rather than indexing off the end
            // of a fixed array. The loaders reject an over-cap count outright;
            // this is the backstop for every other route in.
            Components::TerrainSurfaceRulesEffect& block = m_ResolvedRuleBlocks.emplace_back(*c);
            ClampRuleCountsToCaps(block, e);
            fx.Rules.Rules = block.Rules;
            fx.Rules.RuleCount = block.RuleCount;
            fx.Rules.RespectClaims = block.RespectClaims;
        }
        if (const auto* c = w.GetComponent<Components::TerrainGroundClaimEffect>(e); c && c->Enabled)
        {
            auto& fx = add(ResolvedEffect::Kind::GroundClaim, c->StackOrder);
            fx.GroundClaim.Strength = c->Strength;
        }
        if (const auto* c = w.GetComponent<Components::TerrainGrassEffect>(e); c && c->Enabled)
        {
            auto& fx = add(ResolvedEffect::Kind::Grass, c->StackOrder);
            fx.Grass.HeightScale = Components::ClampTerrainGrassEffectScale(c->HeightScale);
            fx.Grass.DensityScale = Components::ClampTerrainGrassEffectScale(c->DensityScale);
        }

        std::stable_sort(out.begin(), out.end(),
                         [](const ResolvedEffect& a, const ResolvedEffect& b) {
                             return a.StackOrder < b.StackOrder;
                         });
    };

    // ---- Modifier volumes (design §2a) ----
    // One entity owns the region; its effect components stack on top, in
    // StackOrder, and never see the shape. The volume resolves to a SINGLE
    // record so the bake evaluates the shape once per texel and runs the whole
    // stack against it — the spline SDF in particular is O(segments), and N
    // separate records would pay it N times.
    world.Query<ECS::Read<Components::TerrainModifierVolume>,
                ECS::Read<Components::WorldTransform>,
                ECS::Optional<Components::SplineComponent>,
                ECS::Optional<ECS::ComponentDisabled<Components::SplineComponent>>>()
        .Each([&](ECS::EntityHandle e, const Components::TerrainModifierVolume& vol,
                  const Components::WorldTransform& xf,
                  const Components::SplineComponent* splineComp,
                  const ECS::ComponentDisabled<Components::SplineComponent>* splineOff)
        {
            ResolvedModifier mod{};
            mod.Entity = e;
            mod.ModType = ResolvedModifier::Type::Volume;
            mod.Shape = VolumeShapeToModifierShape(vol.Shape);
            mod.SplineFillInterior = vol.Shape != Components::TerrainVolumeShape::SplinePath;
            mod.GlobalScope = vol.Shape == Components::TerrainVolumeShape::Global;
            mod.Priority = vol.Priority;
            mod.Enabled = true;
            mod.Position = Mathematics::Vector3(xf.matrix[12], xf.matrix[13], xf.matrix[14]);
            mod.YawRadians = ExtractYaw(xf);
            mod.Radius = vol.Radius;
            mod.RectHalfX = vol.RectHalfX;
            mod.RectHalfZ = vol.RectHalfZ;
            mod.Falloff = vol.Falloff;
            mod.FalloffInward = vol.FalloffInward;
            mod.Weight = vol.Weight;
            mod.StationSpacing = vol.StationSpacing;

            if (!resolveShape(mod, splineComp, splineOff != nullptr, xf))
                return;

            GatherVolumeEffects(world, e, mod.Effects);

            // A Stamp projects a mask over the volume's footprint, and a global
            // volume has none. Left in, it would sample the mask through the
            // volume's authored rect extents and — because the mask UV clamps to
            // [0,1] — extrude the mask's border texel across the entire world at
            // full weight, with none of the four fields that control it shown in
            // the inspector. Refused here rather than skipped per texel: the
            // gather is the single-threaded place that can warn (the bake's rows
            // run across job workers), the effect then never reaches the hot
            // path, and dropping it moves the geometry hash, so removing the
            // stamp re-bakes exactly as any other effect edit does.
            if (mod.GlobalScope)
            {
                const auto stamp = std::remove_if(
                    mod.Effects.begin(), mod.Effects.end(), [](const ResolvedEffect& fx) {
                        return fx.EffectKind == ResolvedEffect::Kind::Stamp;
                    });
                if (stamp != mod.Effects.end())
                {
                    mod.Effects.erase(stamp, mod.Effects.end());
                    if (m_GlobalStampRefusedWarned.insert(e.id).second)
                        Logger::Log::Warning(
                            "TerrainModifierSystem: entity {} has a Stamp effect on a "
                            "Shape=Global volume. A stamp projects its mask over a footprint and "
                            "a global volume has none, so the stamp is ignored. Give the volume a "
                            "Circle or Rectangle shape, or use a Height Offset / Noise effect for "
                            "a world-wide change.",
                            e.id);
                }
            }

            if (mod.Effects.empty())
                return; // A region with no effects has nothing to bake.

            // A route's grade, resampled by arc length ONCE per gather, not per
            // texel and not per frame. This is the polyline's whole job: it
            // carries the per-station HEIGHT a pooled flatten grades toward. The
            // footprint is the analytic curve for every spline shape, so the
            // polyline is never the volume's ramp or its bounds.
            //
            // SplinePath only: a SplineArea grades toward the region it fills,
            // which has no single route running through it.
            //
            // ResolvedSpline is non-null for every Shape::Spline volume that
            // reaches here (resolveShape returns false otherwise); the deref is
            // guarded where it happens rather than argued.
            if (vol.Shape == Components::TerrainVolumeShape::SplinePath && mod.ResolvedSpline)
            {
                RoutePolyline& polyline = m_RoutePolylines.emplace_back();
                BuildRoutePolyline(*mod.ResolvedSpline, mod.StationSpacing, polyline);
                mod.Route = &polyline;
            }

            ComputeModifierBounds(mod);
            outModifiers.push_back(std::move(mod));
        });

    // ---- Zone modifiers (sculpt/paint payloads, design §3.2) ----
    // Zones are rectangular and transform-aware: the entity's XZ scale
    // multiplies the local extents, so moving/rotating/scaling the entity moves
    // the effect. Mapped onto a Rectangle-shaped ResolvedModifier so the shared
    // ComputeWeight / ComputeModifierBounds machinery applies unchanged; the
    // payload is sampled in zone-local UV during the apply.
    auto setupZoneTransform = [](ResolvedModifier& mod, const Components::WorldTransform& xf,
                                 float32 extentX, float32 extentZ, float32 falloff, float32 priority)
    {
        float32 scaleX = 1.0f, scaleZ = 1.0f;
        ExtractXZScale(xf, scaleX, scaleZ);
        mod.Shape = Components::TerrainModifierShape::Rectangle;
        mod.Position = Mathematics::Vector3(xf.matrix[12], xf.matrix[13], xf.matrix[14]);
        mod.YawRadians = ExtractYaw(xf);
        mod.RectHalfX = extentX * scaleX;
        mod.RectHalfZ = extentZ * scaleZ;
        mod.Falloff = falloff;
        mod.Priority = priority;
        mod.Enabled = true;
    };

    world.Query<ECS::Read<Components::TerrainSculptZone>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle e, const Components::TerrainSculptZone& comp,
                  const Components::WorldTransform& xf)
        {
            ResolvedModifier mod{};
            mod.Entity = e;
            mod.ModType = ResolvedModifier::Type::SculptZone;
            setupZoneTransform(mod, xf, comp.ExtentX, comp.ExtentZ, comp.Falloff, comp.Priority);
            // Union member: set every field explicitly.
            mod.SculptZone.Offsets = nullptr;
            mod.SculptZone.Width = 0;
            mod.SculptZone.Height = 0;
            mod.SculptZone.Blend = comp.Blend;

            const GUID guid = comp.PayloadRef.ToGuid();
            if (!guid.IsNull())
            {
                mod.PayloadGuid = guid;
                const TerrainZonePayload* payload = terrainService.ResolveZonePayload(guid);
                if (payload && payload->IsSculpt() && payload->Width > 0 && payload->Height > 0)
                {
                    mod.SculptZone.Offsets = payload->Offsets.data();
                    mod.SculptZone.Width = payload->Width;
                    mod.SculptZone.Height = payload->Height;
                    mod.DataVersion = payload->DataVersion;
                    ComputeZonePayloadWorldRect(mod, *payload);
                }
            }
            ComputeModifierBounds(mod);
            outModifiers.push_back(mod);
        });

    world.Query<ECS::Read<Components::TerrainPaintZone>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle e, const Components::TerrainPaintZone& comp,
                  const Components::WorldTransform& xf)
        {
            ResolvedModifier mod{};
            mod.Entity = e;
            mod.ModType = ResolvedModifier::Type::PaintZone;
            setupZoneTransform(mod, xf, comp.ExtentX, comp.ExtentZ, comp.Falloff, comp.Priority);
            mod.PaintZone.Mask = nullptr;
            mod.PaintZone.Width = 0;
            mod.PaintZone.Height = 0;
            mod.PaintZone.LayerIndex = comp.LayerIndex;
            mod.PaintZone.Strength = comp.Strength;

            const GUID guid = comp.PayloadRef.ToGuid();
            if (!guid.IsNull())
            {
                mod.PayloadGuid = guid;
                const TerrainZonePayload* payload = terrainService.ResolveZonePayload(guid);
                if (payload && !payload->IsSculpt() && payload->Width > 0 && payload->Height > 0)
                {
                    mod.PaintZone.Mask = payload->Mask.data();
                    mod.PaintZone.Width = payload->Width;
                    mod.PaintZone.Height = payload->Height;
                    mod.DataVersion = payload->DataVersion;
                    ComputeZonePayloadWorldRect(mod, *payload);
                }
            }
            ComputeModifierBounds(mod);
            outModifiers.push_back(mod);
        });

    // Sort by priority (lower priority first). Stable, so modifiers sharing a
    // priority keep gather order instead of whatever an unstable sort happens to
    // produce — cross-volume ordering has to be reproducible bake to bake.
    std::stable_sort(outModifiers.begin(), outModifiers.end(),
                     [](const ResolvedModifier& a, const ResolvedModifier& b) {
                         return a.Priority < b.Priority;
                     });

    // One gather owns the shapes and ordering, but grass has its own derived
    // field and invalidation. Removing it before the existing ground hash/bake
    // keeps grass value edits on mixed volumes from re-cooking ground. Adding
    // or removing a component still follows the existing archetype tie order
    // for equal-priority ground volumes; that structural input is not erased.
    for (auto& mod : outModifiers)
    {
        if (mod.ModType != ResolvedModifier::Type::Volume
            || std::none_of(mod.Effects.begin(), mod.Effects.end(), [](const auto& fx) {
                return fx.EffectKind == ResolvedEffect::Kind::Grass;
            })) continue;
        auto grass = mod;
        std::erase_if(grass.Effects, [](const ResolvedEffect& fx) {
            return fx.EffectKind != ResolvedEffect::Kind::Grass;
        });
        if (!grass.Effects.empty()) m_GrassModifiers.push_back(std::move(grass));
        std::erase_if(mod.Effects, [](const ResolvedEffect& fx) {
            return fx.EffectKind == ResolvedEffect::Kind::Grass;
        });
    }
    std::erase_if(outModifiers, [](const ResolvedModifier& mod) {
        return mod.ModType == ResolvedModifier::Type::Volume && mod.Effects.empty();
    });

    WarnOnClaimsThatCannotMask(outModifiers);
    WarnOnClaimedGroundWithNoPaint(outModifiers);
}

void TerrainModifierSystem::BakeGrassFields(ECS::World& world, TerrainService& service)
{
    const uint64 terrainHash = ComputeTerrainStateHash(world, service);
    // Re-entering/created terrain must reconcile even an empty current stack:
    // a disabled owner may still hold pixels from a region removed meanwhile.
    if (m_GrassModifiers.empty() && m_LastGrassSnapshot.empty()
        && m_HasGrassBaseline && terrainHash == m_LastGrassTerrainHash) return;
    // Spline scenes can gather every frame. Compare the stable ordered stack
    // once before allocating a diff or searching for changed/removed owners.
    // Continue through fields below: streaming may replace/introduce tile data
    // without changing either the authored terrain or modifier stack.
    bool unchanged = m_HasGrassBaseline && m_GrassModifiers.size() == m_LastGrassSnapshot.size();
    for (size_t i = 0; unchanged && i < m_GrassModifiers.size(); ++i)
        unchanged = m_GrassModifiers[i].Entity == m_LastGrassSnapshot[i].Entity
                 && HashModifierState(m_GrassModifiers[i]) == m_LastGrassSnapshot[i].Hash;
    std::vector<GrassBakeSnapshot> next;
    uint64 hash = m_LastGrassHash;
    DirtyUnion dirty;
    bool full = !m_HasGrassBaseline || terrainHash != m_LastGrassTerrainHash;
    if (!unchanged)
    {
        hash = 0;
        next.reserve(m_GrassModifiers.size());
        for (const auto& mod : m_GrassModifiers)
        {
            const uint64 value = HashModifierState(mod);
            hash = HashCombine(hash, value);
            next.push_back({mod.Entity, value, mod.BoundsMinX, mod.BoundsMinZ, mod.BoundsMaxX, mod.BoundsMaxZ});
            const auto old = std::find_if(m_LastGrassSnapshot.begin(), m_LastGrassSnapshot.end(),
                                         [&](const auto& entry) { return entry.Entity == mod.Entity; });
            if (old == m_LastGrassSnapshot.end() || old->Hash != value)
            {
                dirty.Add(mod.BoundsMinX, mod.BoundsMinZ, mod.BoundsMaxX, mod.BoundsMaxZ);
                if (old != m_LastGrassSnapshot.end()) dirty.Add(old->MinX, old->MinZ, old->MaxX, old->MaxZ);
            }
        }
        for (const auto& old : m_LastGrassSnapshot)
            if (std::none_of(next.begin(), next.end(), [&](const auto& entry) { return entry.Entity == old.Entity; }))
                dirty.Add(old.MinX, old.MinZ, old.MaxX, old.MaxZ);
        std::vector<ECS::EntityHandle> oldOrder, newOrder;
        for (const auto& old : m_LastGrassSnapshot)
            if (std::any_of(next.begin(), next.end(), [&](const auto& entry) { return entry.Entity == old.Entity; }))
                oldOrder.push_back(old.Entity);
        for (const auto& current : next)
            if (std::any_of(m_LastGrassSnapshot.begin(), m_LastGrassSnapshot.end(),
                           [&](const auto& entry) { return entry.Entity == current.Entity; }))
                newOrder.push_back(current.Entity);
        // An unrelated archetype removal can swap equal-priority survivors. Its
        // removed footprint does not describe the changed overlap elsewhere.
        full = full || oldOrder != newOrder;
    }
    auto update = [&](TerrainGrassField& field, uint32 w, uint32 h,
                      float32 sizeX, float32 sizeZ, float32 originX, float32 originZ)
    {
        if (w < 2 || h < 2 || sizeX <= 0 || sizeZ <= 0) return;
        const bool entire = full || !field.Initialized || field.Width != w || field.Height != h
                            || field.AppliedHash != m_LastGrassHash;
        if (entire)
            ComposeTerrainGrassField(field, w, h, sizeX, sizeZ, originX, originZ, m_GrassModifiers);
        else if (dirty.Any && dirty.MaxX >= originX && dirty.MinX <= originX + sizeX
                 && dirty.MaxZ >= originZ && dirty.MinZ <= originZ + sizeZ)
        {
            // Clamp world bounds before converting: a global region carries
            // infinities, which must never be converted directly to an integer.
            auto lo = [](float32 value, float32 origin, float32 size, uint32 samples) {
                return std::max(0, static_cast<int32>(std::floor(std::clamp((value-origin)/size, 0.0f, 1.0f)
                                                                * (samples-1))) - 1);
            };
            auto hi = [](float32 value, float32 origin, float32 size, uint32 samples) {
                return std::min(static_cast<int32>(samples-1), static_cast<int32>(std::ceil(
                    std::clamp((value-origin)/size, 0.0f, 1.0f) * (samples-1))) + 1);
            };
            ComposeTerrainGrassField(field, w, h, sizeX, sizeZ, originX, originZ, m_GrassModifiers,
                                     lo(dirty.MinX, originX, sizeX, w), lo(dirty.MinZ, originZ, sizeZ, h),
                                     hi(dirty.MaxX, originX, sizeX, w), hi(dirty.MaxZ, originZ, sizeZ, h));
        }
        field.AppliedHash = hash;
    };
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](const Components::Terrain& terrain, const Components::WorldTransform& xf)
        {
            // TerrainGrass is planar-only; this effect cannot enable planet grass.
            if (terrain.Domain != Components::TerrainDomain::Planar) return;
            if (terrain.TiledTerrainHandle != 0 || terrain.TiledTerrainGeneration != 0)
            {
                auto* tiled = service.GetTiledTerrainData({terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration});
                if (!tiled) return;
                // Extraction publishes the service origin later in the tick.
                // Use the current authored transform, never last frame's cache.
                const float32 ox = xf.matrix[12] - terrain.SizeX * 0.5f;
                const float32 oz = xf.matrix[14] - terrain.SizeZ * 0.5f;
                // The renderer covers whole tiles, including a partial authored
                // edge tile. Its UVs use that extent, not the requested size.
                const auto extent = ComputeTiledRenderExtent(tiled->Config);
                const float32 sizeX = extent.WorldSizeX, sizeZ = extent.WorldSizeZ;
                tiled->GrassRegionsActive = std::any_of(m_GrassModifiers.begin(), m_GrassModifiers.end(), [&](const auto& mod) {
                    return mod.Enabled && mod.Weight > 0 && mod.BoundsMaxX >= ox && mod.BoundsMinX <= ox + sizeX
                        && mod.BoundsMaxZ >= oz && mod.BoundsMinZ <= oz + sizeZ
                        && std::any_of(mod.Effects.begin(), mod.Effects.end(), [](const auto& fx) {
                            return fx.Grass.HeightScale != 1.0f || fx.Grass.DensityScale != 1.0f;
                        });
                });
                if (extent.UnifiedWidth <= kMaxUnifiedTiledResolution && extent.UnifiedHeight <= kMaxUnifiedTiledResolution)
                {
                    // This route already owns full-size unified terrain textures.
                    // Compose controls on the same global lattice, independently of
                    // which height tiles have streamed in.
                    update(tiled->GrassUnifiedField, extent.UnifiedWidth, extent.UnifiedHeight,
                           sizeX, sizeZ, ox, oz);
                    return;
                }
                // Evaluate on global world coordinates, including tiles that have
                // never streamed. Residency cannot erase a suppression region.
                update(tiled->GrassCoarseField, kAtlasCoarseFieldDim, kAtlasCoarseFieldDim, sizeX, sizeZ, ox, oz);
                for (auto& [coord, tile] : tiled->Tiles)
                {
                    if (!tile || tile->LodState == TileLodState::Empty) continue;
                    const float32 size = tiled->Config.TileWorldSize;
                    update(tile->GrassField, tiled->Config.TileConfig.HeightmapWidth, tiled->Config.TileConfig.HeightmapHeight,
                           size, size, ox + coord.X * size, oz + coord.Z * size);
                }
            }
            else if (auto* data = service.GetTerrainData({terrain.TerrainDataHandle, terrain.TerrainDataGeneration}))
                update(data->GrassField, data->Heightfield.GetWidth(), data->Heightfield.GetHeight(),
                       data->Config.WorldSizeX, data->Config.WorldSizeZ,
                       xf.matrix[12] - terrain.SizeX * 0.5f, xf.matrix[14] - terrain.SizeZ * 0.5f);
        });
    if (!unchanged) m_LastGrassSnapshot = std::move(next);
    m_LastGrassHash = hash;
    m_LastGrassTerrainHash = terrainHash;
    m_HasGrassBaseline = true;
}

// A claim-respecting effect reads the claim buffer at the slot it APPLIES in —
// its own, the slot it accumulates in when pooled — so a claim holds ground
// against it only if the claim's own slot comes FIRST. Position in this sorted
// list is the rule exactly — including ties, which the stable sort resolves by
// gather order, the same order the bake loop walks — so the test is "is the
// claim at a later index than this effect", not a priority comparison.
//
// Per EFFECT, not per pool and not per volume: the mask is applied to each
// effect's own weight, so one effect's ordering says nothing about another's,
// and the entity worth naming is the one that ends up unmasked.
//
// Warned here rather than at the bake: the gather is single-threaded and is the
// one place that can name the entity, and a claim that masks nothing is an
// authoring mistake the scene cannot fix for itself.
void TerrainModifierSystem::WarnOnClaimsThatCannotMask(
    const std::vector<ResolvedModifier>& modifiers)
{
    for (std::size_t i = 0; i < modifiers.size(); ++i)
    {
        const ResolvedModifier& mod = modifiers[i];
        if (mod.ModType != ResolvedModifier::Type::Volume)
            continue;

        // The FIRST claim-respecting effect on this volume, whatever it blends
        // with — masking is per effect and blend-independent. One report per
        // entity either way: the fix is the same for every such effect on it, and
        // the warn sets are keyed by entity.
        const ResolvedEffect* respecting = nullptr;
        for (const auto& fx : mod.Effects)
            if (EffectCanRespectClaims(fx.EffectKind) && EffectRespectClaims(fx))
            {
                respecting = &fx;
                break;
            }
        if (!respecting)
            continue;

        const char* const kindName = EffectKindName(respecting->EffectKind);

        // A same-entity claim is legal — a run that owns its ground and claims it
        // afterwards is the intended shape. It only silences the effect when the
        // claim runs FIRST: effects apply in stack order within one volume, so a
        // claim below the effect is already in the buffer when the effect reads
        // it, and the effect holds itself off its own ground.
        const ResolvedEffect* const ownClaim =
            FindEffectOfKind(mod, ResolvedEffect::Kind::GroundClaim);
        if (ownClaim && ownClaim->StackOrder < respecting->StackOrder
            && m_EffectSelfClaimWarned.insert(mod.Entity.id).second)
        {
            Logger::Log::Warning(
                "TerrainModifierSystem: entity {} carries a Ground Claim below a {} that stops "
                "at claimed ground, so it holds its own ground back and shapes nothing. A region "
                "that owns the ground it shapes or paints must not defer to a claim: turn Stops "
                "at Claimed Ground off on it, drag the claim above the effect, or move the claim "
                "to a separate volume.",
                mod.Entity.id, kindName);
        }

        for (std::size_t claimIndex = i + 1; claimIndex < modifiers.size(); ++claimIndex)
        {
            const ResolvedModifier& claim = modifiers[claimIndex];
            if (!ModifierHasKind(claim, ResolvedEffect::Kind::GroundClaim))
                continue;
            if (!ModifiersOverlapXZ(mod, claim))
                continue;

            if (m_ClaimAboveEffectWarned.insert(mod.Entity.id).second)
                Logger::Log::Warning(
                    "TerrainModifierSystem: the {} on entity {} stops at claimed "
                    "ground, but the Ground Claim on entity {} it overlaps has priority {} "
                    "against the priority {} the effect applies at. A claim is only in place for "
                    "effects that apply AFTER it, so this one holds no ground back. Give the claim "
                    "a lower priority than every region that must stop at it.",
                    kindName, mod.Entity.id, claim.Entity.id, claim.Priority, mod.Priority);
            break; // One report per effect; the fix is the same for all of them.
        }
    }
}

// THE LAYER-0 TRAP, and it is the companion the splat claim needs to be safe.
//
// Masking a splat effect off claimed ground preserves the claimant's material
// only if the claimant PAINTS that material. The height pass has a meaningful
// "what is already there" — the base heightfield plus every earlier modifier —
// and the splat pass does not: it starts from an all-zero splatmap every bake,
// and an all-zero texel resolves to channel 0, the FIRST material, by shipped
// convention. So a volume that claims but paints nothing does not preserve
// anything; it cuts a patch of the default material out of the surface around
// it, which reads as a bug in the mask rather than as a missing paint effect.
//
// Reported on the CLAIMANT, because that is the entity that has to change, and
// only when something that applies after it actually stops at it — a claim
// nothing defers to leaves no patch. Once per entity, same lifetime as the sets
// above.
void TerrainModifierSystem::WarnOnClaimedGroundWithNoPaint(
    const std::vector<ResolvedModifier>& modifiers)
{
    const auto anySplatEffectRespectsClaims = [](const ResolvedModifier& mod) {
        return std::any_of(mod.Effects.begin(), mod.Effects.end(), [](const ResolvedEffect& fx) {
            return ResolvedEffect::IsSplat(fx.EffectKind) && EffectRespectClaims(fx);
        });
    };

    for (std::size_t i = 0; i < modifiers.size(); ++i)
    {
        const ResolvedModifier& claimant = modifiers[i];
        if (claimant.ModType != ResolvedModifier::Type::Volume)
            continue;
        if (!ModifierHasKind(claimant, ResolvedEffect::Kind::GroundClaim))
            continue;
        // It paints, so the mask preserves what it painted. This is the shape the
        // feature is FOR, and it must stay silent.
        if (claimant.TouchesSplat())
            continue;

        for (std::size_t j = i + 1; j < modifiers.size(); ++j)
        {
            const ResolvedModifier& deferring = modifiers[j];
            if (!anySplatEffectRespectsClaims(deferring))
                continue;
            if (!ModifiersOverlapXZ(claimant, deferring))
                continue;

            if (m_ClaimWithoutPaintWarned.insert(claimant.Entity.id).second)
                Logger::Log::Warning(
                    "TerrainModifierSystem: entity {} claims ground that a splat effect on "
                    "entity {} stops at, but entity {} paints nothing. The splatmap starts EMPTY "
                    "every bake and an empty texel resolves to material channel 0, so the claimed "
                    "ground will come out as channel 0 — the first material — not as this "
                    "region's. Add a Paint Layer or Surface Rules effect to entity {} that paints "
                    "the material this ground should keep, or turn Stops at Claimed Ground off on "
                    "entity {}.",
                    claimant.Entity.id, deferring.Entity.id, claimant.Entity.id,
                    claimant.Entity.id, deferring.Entity.id);
            break; // One report per claimant; the fix is the same for all of them.
        }
    }
}

float32 ComputeWeight(const ResolvedModifier& mod, float32 worldX, float32 worldZ)
{
    if (mod.GlobalScope)
    {
        // No edge, so no ramp: the shape contributes 1 at every sample and the
        // volume's master strength is the whole of the weight. Falloff and
        // FalloffInward are ignored rather than applied to a boundary that does
        // not exist.
        return mod.Weight;
    }

    const float32 dx = worldX - mod.Position.x;
    const float32 dz = worldZ - mod.Position.z;

    float32 distFromEdge = 0.0f;

    if (mod.Shape == Components::TerrainModifierShape::Spline)
    {
        // A modifier footprint is a top-down mask, so the weight comes from the
        // XZ distance to the spline's ground-plane projection: a road authored
        // 200 m up covers exactly the ground a road at sea level does. Its Y is
        // data (the flatten target), never part of the distance.
        //
        // The FOOTPRINT is the analytic curve for every spline shape, while a
        // pooled flatten's per-station HEIGHT comes from the resampled polyline
        // (ComputeVolumeReferenceHeight). Two evaluators for two different
        // quantities, not two answers to one: the polyline exists to carry a
        // route's grade, and a swept band around it cannot describe the region a
        // closed loop encloses. Keeping the ramp analytic is also what holds the
        // documented invariant that the two spline shapes are identical on an
        // open spline.
        if (!mod.ResolvedSpline)
            return 0.0f;
        // SplineFillInterior chooses "inside this loop" (SplineArea) over "along
        // this loop" (SplinePath); on an open spline the two agree.
        const float32 sd = mod.SplineFillInterior
            ? Spline::SignedDistanceToSplineXZ(*mod.ResolvedSpline, worldX, worldZ)
            : Spline::SignedDistanceToSplineBandXZ(*mod.ResolvedSpline, worldX, worldZ);
        distFromEdge = -sd;
    }
    else if (mod.Shape == Components::TerrainModifierShape::Circle)
    {
        const float32 dist = std::sqrt(dx * dx + dz * dz);
        distFromEdge = mod.Radius - dist;
    }
    else // Rectangle
    {
        // Rotate point into local space of the rectangle.
        const float32 c = std::cos(-mod.YawRadians);
        const float32 s = std::sin(-mod.YawRadians);
        const float32 lx = std::abs(c * dx - s * dz);
        const float32 lz = std::abs(s * dx + c * dz);

        const float32 distX = mod.RectHalfX - lx;
        const float32 distZ = mod.RectHalfZ - lz;
        distFromEdge = std::min(distX, distZ);
    }

    // The volume's master strength multiplies the shape ramp; a zone leaves it
    // at 1, so the multiply is exact there.
    return VolumeShapeWeight(mod, distFromEdge);
}

float32 TerrainModifierSystem::ComputeVolumeReferenceHeight(const ResolvedModifier& mod,
                                                            const RouteSample* routeSample,
                                                            float32 worldX, float32 worldZ)
{
    if (routeSample)
    {
        // The station polyline is the volume's geometry, so the grade the caller
        // already measured against it IS the reference — one object, one answer.
        return routeSample->Height;
    }
    if (mod.Shape == Components::TerrainModifierShape::Spline && mod.ResolvedSpline)
    {
        // The path point above/below this sample — found in XZ, so a sloped road
        // flattens to its own profile instead of being pulled along the curve
        // toward whichever part of it happens to run nearest the y = 0 plane.
        return Spline::FindClosestPointXZ(*mod.ResolvedSpline, worldX, worldZ).Position.y;
    }
    return mod.Position.y;
}

StampSampleBasis TerrainModifierSystem::MakeStampBasis(const ResolvedModifier& mod)
{
    StampSampleBasis basis{};
    if (mod.ModType == ResolvedModifier::Type::Volume)
    {
        for (const auto& fx : mod.Effects)
            if (fx.EffectKind == ResolvedEffect::Kind::Stamp)
            {
                basis.Params = &fx.Stamp;
                break;
            }
    }
    if (!basis.Params)
        return basis;

    basis.HasMask = basis.Params->MaskTexels != nullptr
        && basis.Params->MaskWidth > 0 && basis.Params->MaskHeight > 0;
    if (!basis.HasMask)
        return basis;

    // A global volume has no extents to project a mask through, and the UV clamp
    // in SampleMaskBilinear would smear the mask's border texel over the world.
    // The gather drops Stamp effects from a global volume (with a one-shot
    // warning naming the entity), so this is unreachable through it; the guard
    // keeps the invariant at the point of use, where the consequence is silent.
    if (mod.GlobalScope)
    {
        basis.HasMask = false;
        return basis;
    }

    const float32 rotation = mod.YawRadians + basis.Params->Rotation * kDegToRad;
    basis.Cos = std::cos(-rotation);
    basis.Sin = std::sin(-rotation);
    const bool circle = mod.Shape == Components::TerrainModifierShape::Circle;
    const float32 extentX = (circle ? mod.Radius : mod.RectHalfX) + mod.Falloff;
    const float32 extentZ = (circle ? mod.Radius : mod.RectHalfZ) + mod.Falloff;
    basis.InvExtX = extentX > 0.0f ? 1.0f / extentX : 0.0f;
    basis.InvExtZ = extentZ > 0.0f ? 1.0f / extentZ : 0.0f;
    return basis;
}

bool TerrainModifierSystem::HeightEffectValue(const ResolvedModifier& mod,
                                              const ResolvedEffect& effect,
                                              float32 worldX, float32 worldZ,
                                              float32 worldSizeX, float32 heightScale,
                                              float32 terrainOriginY,
                                              const StampSampleBasis& stampBasis,
                                              const RouteSample* routeSample,
                                              float32& outValueNorm)
{
    switch (effect.EffectKind)
    {
    case ResolvedEffect::Kind::Flatten:
    {
        // Both branches resolve to a world Y — an absolute target, or the volume's
        // own world reference height plus the authored offset — so both come back
        // through the same inverse of the sample -> world map, terrain translation
        // included. The GPU kernel's flatten branch mirrors this expression.
        //
        // routeSample is the pooled path's per-station grade and null everywhere
        // else; ComputeVolumeReferenceHeight falls back to the curve or the entity
        // when it is null, which is what the in-place arm has always passed.
        const float32 targetWorldY = effect.Flatten.UseVolumeHeight
            ? ComputeVolumeReferenceHeight(mod, routeSample, worldX, worldZ)
                  + effect.Flatten.TargetHeight
            : effect.Flatten.TargetHeight;
        outValueNorm = NormalizedHeightForWorldY(targetWorldY, terrainOriginY, heightScale);
        return true;
    }
    case ResolvedEffect::Kind::HeightOffset:
        outValueNorm = effect.HeightOffset.Offset / heightScale;
        return true;
    case ResolvedEffect::Kind::Noise:
    {
        const uint32 seed = effect.Noise.Seed != 0 ? effect.Noise.Seed : 12345u;
        const float32 freq = effect.Noise.Frequency / worldSizeX;
        const float32 amp = effect.Noise.Amplitude / heightScale;
        // Erosion strength 0 keeps the untouched fBM call, so arming and
        // disarming the block is byte-exact rather than merely close.
        outValueNorm = effect.Noise.Erosion.Strength > 0.0f
            ? Erosion::ErodedFBMNoise2D(worldX, worldZ, freq, amp, effect.Noise.Octaves, seed,
                                      effect.Noise.Lacunarity, effect.Noise.Persistence,
                                      effect.Noise.Erosion)
            : Noise::FBMNoise2D(worldX, worldZ, freq, amp, effect.Noise.Octaves, seed,
                         effect.Noise.Lacunarity, effect.Noise.Persistence);
        return true;
    }
    case ResolvedEffect::Kind::Stamp:
    {
        // A stamp needs a footprint to project into; a global volume has none.
        // The gather drops the effect (warning once, by entity), so this is
        // unreachable through it — but the guard has to be HERE as well as in
        // MakeStampBasis, because a basis with no mask falls through to the
        // flat-white branch below, which would turn a refused stamp into a
        // silent world-wide raise.
        //
        // NO CONTRIBUTION, not a zero value: a pooled member contributing weight
        // with value 0 would drag its pool's average toward zero instead of
        // standing aside.
        if (mod.GlobalScope)
            return false;

        // Mask value at this sample. Grayscale masks use the R channel; no mask
        // (or a failed decode) is flat white — the documented flat raise/lower.
        float32 maskValue = 1.0f;
        if (stampBasis.HasMask)
        {
            const float32 dx = worldX - mod.Position.x;
            const float32 dz = worldZ - mod.Position.z;
            const float32 lx = stampBasis.Cos * dx - stampBasis.Sin * dz;
            const float32 lz = stampBasis.Sin * dx + stampBasis.Cos * dz;
            maskValue = SampleMaskBilinear(
                stampBasis.Params->MaskTexels, stampBasis.Params->MaskWidth,
                stampBasis.Params->MaskHeight,
                0.5f + 0.5f * lx * stampBasis.InvExtX,
                0.5f + 0.5f * lz * stampBasis.InvExtZ);
        }
        outValueNorm = effect.Stamp.HeightScale * maskValue / heightScale;
        return true;
    }
    case ResolvedEffect::Kind::PaintLayer:
    case ResolvedEffect::Kind::Rules:
    case ResolvedEffect::Kind::GroundClaim:
        return false; // splat-only, or ownership without height
    default:
        return false;
    }
}

void TerrainModifierSystem::ApplyHeightEffectSample(const ResolvedModifier& mod,
                                                    const ResolvedEffect& effect,
                                                    float32& currentHeight, float32 weight,
                                                    float32 worldX, float32 worldZ,
                                                    float32 worldSizeX, float32 heightScale,
                                                    float32 terrainOriginY,
                                                    const StampSampleBasis& stampBasis)
{
    // A pooled effect writes ONCE, at its pool's slot; writing here as well would
    // apply it twice. Guarded at the point of use, where the mistake would be
    // silent — BlendHeight's fall-through arm bakes an unhandled mode as Add, so
    // a double-applied route would read as an odd raise rather than a wrong
    // branch.
    if (IsPooled(effect))
    {
        assert(false && "a pooled effect writes through its pool, never in place");
        return;
    }

    // The in-place arm never has a route sample: the per-station grade is read
    // only where a pooled flatten accumulates.
    float32 valueNorm = 0.0f;
    if (!HeightEffectValue(mod, effect, worldX, worldZ, worldSizeX, heightScale, terrainOriginY,
                           stampBasis, nullptr, valueNorm))
        return;

    // Every effect drives the same algebra; only the value it contributes differs.
    // BlendSmoothing is metres of height, so it normalizes by the same height scale.
    currentHeight = BlendHeight(currentHeight, valueNorm, weight, EffectBlend(effect),
                                EffectBlendSmoothing(effect) / heightScale);
}

void TerrainModifierSystem::ApplyHeightModifiers(Terrain::HeightfieldData& heightfield,
                                                  float32 worldSizeX, float32 worldSizeZ,
                                                  float32 heightScale,
                                                  float32 terrainOriginX,
                                                  float32 terrainOriginY,
                                                  float32 terrainOriginZ,
                                                  const std::vector<ResolvedModifier>& modifiers,
                                                  int32 regionMinX, int32 regionMinZ,
                                                  int32 regionMaxX, int32 regionMaxZ)
{
    const uint32 w = heightfield.GetWidth();
    const uint32 h = heightfield.GetHeight();
    if (w == 0 || h == 0)
        return;

    const float32 spacingX = worldSizeX / static_cast<float32>(w - 1);
    const float32 spacingZ = worldSizeZ / static_cast<float32>(h - 1);

    float32* samples = heightfield.GetMutableSamples();

    // ---- Cross-modifier scratch ---------------------------------------------
    //
    // The loop below is modifier-major (outer loop over modifiers, inner over
    // that modifier's own AABB), which is what makes a region bake cheap. The two
    // composition operators are the only things in the stack that need to see
    // ACROSS modifiers, so they are the only things that need scratch: a claim
    // has to outlive the modifier that wrote it, and a group's members have to
    // accumulate before any of them applies.
    //
    // Both are local to this call. ApplyHeightModifiers runs per tile and tiles
    // are baked across job workers, so anything held on the system would be
    // shared mutable state between them.
    const int32 scratchMinX = std::max(0, regionMinX);
    const int32 scratchMinZ = std::max(0, regionMinZ);
    const int32 scratchMaxX = std::min(static_cast<int32>(w - 1), regionMaxX);
    const int32 scratchMaxZ = std::min(static_cast<int32>(h - 1), regionMaxZ);
    const bool scratchRegionEmpty = scratchMinX > scratchMaxX || scratchMinZ > scratchMaxZ;

    // Ownership per sample of the bake region, accumulated by MAX as claiming
    // volumes apply and read by every later height effect that respects claims,
    // whatever it blends with. Allocated only when a claim is actually authored,
    // so a scene without one pays nothing and takes the byte-identical path it
    // always did.
    std::vector<float32> claim;
    const bool anyClaim = !scratchRegionEmpty
        && std::any_of(modifiers.begin(), modifiers.end(), [](const ResolvedModifier& m) {
               return m.ModType == ResolvedModifier::Type::Volume
                   && std::any_of(m.Effects.begin(), m.Effects.end(), [](const ResolvedEffect& fx) {
                          return fx.EffectKind == ResolvedEffect::Kind::GroundClaim;
                      });
           });
    const int32 scratchWidth = scratchMaxX - scratchMinX + 1;
    if (anyClaim)
        claim.assign(static_cast<std::size_t>(scratchWidth)
                         * static_cast<std::size_t>(scratchMaxZ - scratchMinZ + 1),
                     0.0f);

    const auto scratchIndex = [&](int32 sx, int32 sz) {
        return static_cast<std::size_t>(sz - scratchMinZ) * static_cast<std::size_t>(scratchWidth)
             + static_cast<std::size_t>(sx - scratchMinX);
    };

    // One accumulator per POOL: the weighted value sum, the weight sum, and the
    // CLAIM-MASKED weight sum, over the union of the pool's member footprints.
    // Every pooled effect has one — an unnamed member joins the default pool, and
    // a lone one is a pool of one whose accumulator holds a single term.
    //
    // A pool is keyed by (Kind, Group), not by the group name alone. A pool
    // averages ONE quantity, and a flatten's is an absolute candidate height
    // while a height offset's, a noise's and a stamp's are displacements, so a
    // Noise pool named "dunes" and a Flatten pool named "dunes" must not merge.
    //
    // One path, not a fast path beside it. A pool of one with no claim reduces
    // algebraically to the in-place operator ((w*v)/w = v, blend = min(w,1)) —
    // the lerp for a height-valued kind, the plain Add for a displacement-valued
    // one — and the reduction is asserted rather than implemented twice: two
    // implementations of one operator is what let a claim invert the blend on
    // exactly one of them in slice 1.
    struct HeightPoolAccum
    {
        ResolvedEffect::Kind Kind = ResolvedEffect::Kind::Flatten;
        StringId Group = 0;
        // Selects the flush arm: lerp the ground toward the average, or add it.
        bool ValueIsHeight = true;
        int32 MinX = 0, MinZ = 0, MaxX = 0, MaxZ = 0;
        int32 Width = 0;
        std::size_t LastMember = 0;
        // Tie-break when two pools share a last member, which a volume carrying
        // two pooled effects of different kinds produces: they apply in the stack
        // order of the effects that own them, exactly as in-place effects do.
        int32 LastMemberStackOrder = 0;
        bool HasRect = false;
        bool Applied = false;
        std::vector<float32> Acc;
        std::vector<float32> Wsum;
        // sum of w * (member respects claims ? 1 - claim : 1). The masking is per
        // MEMBER, so an owning run grades its ground while the path beside it
        // stops at the claim; the coverage clamp still reads the UNMASKED Wsum,
        // so three routes over a half-owned texel still move it half way.
        std::vector<float32> MaskedWsum;
    };
    // A handful of groups at most, so a flat vector with a linear find beats a
    // hash map on both lookup cost and allocation.
    std::vector<HeightPoolAccum> groups;

    if (!scratchRegionEmpty)
    {
        for (std::size_t i = 0; i < modifiers.size(); ++i)
        {
            const ResolvedModifier& mod = modifiers[i];
            if (mod.ModType != ResolvedModifier::Type::Volume)
                continue;

            const int32 minX = std::max(scratchMinX,
                ModifierSampleIndex(mod.BoundsMinX, terrainOriginX, spacingX));
            const int32 minZ = std::max(scratchMinZ,
                ModifierSampleIndex(mod.BoundsMinZ, terrainOriginZ, spacingZ));
            const int32 maxX = std::min(scratchMaxX,
                ModifierSampleIndex(mod.BoundsMaxX, terrainOriginX, spacingX) + 1);
            const int32 maxZ = std::min(scratchMaxZ,
                ModifierSampleIndex(mod.BoundsMaxZ, terrainOriginZ, spacingZ) + 1);

            // One component of a type per entity, so a volume contributes at most
            // one member to any one pool — but it can carry a pooled effect of
            // each poolable kind, and those join four different pools.
            for (const ResolvedEffect& fx : mod.Effects)
            {
                if (!IsPooled(fx))
                    continue;

                const StringId group = EffectPoolGroup(fx);
                auto it = std::find_if(groups.begin(), groups.end(),
                                       [&](const HeightPoolAccum& g) {
                                           return g.Kind == fx.EffectKind && g.Group == group;
                                       });
                if (it == groups.end())
                {
                    HeightPoolAccum fresh{};
                    fresh.Kind = fx.EffectKind;
                    fresh.Group = group;
                    fresh.ValueIsHeight = PooledValueIsHeight(fx.EffectKind);
                    groups.push_back(std::move(fresh));
                    it = groups.end() - 1;
                }
                // A member whose footprint misses this bake region contributes no
                // rect — unioning its inverted box in would grow the accumulator
                // to cover ground no member actually reaches.
                if (minX <= maxX && minZ <= maxZ)
                {
                    if (!it->HasRect)
                    {
                        it->MinX = minX; it->MinZ = minZ; it->MaxX = maxX; it->MaxZ = maxZ;
                        it->HasRect = true;
                    }
                    else
                    {
                        it->MinX = std::min(it->MinX, minX);
                        it->MinZ = std::min(it->MinZ, minZ);
                        it->MaxX = std::max(it->MaxX, maxX);
                        it->MaxZ = std::max(it->MaxZ, maxZ);
                    }
                }
                // The group occupies ONE slot in the priority order, at its
                // highest-priority member — which, after the stable sort by
                // Priority, is simply the last member seen.
                it->LastMember = i;
                it->LastMemberStackOrder = fx.StackOrder;
            }
        }

        // Flush order. The main loop applies every pool whose last member is
        // behind it, walking `groups` in vector order, so ordering the vector by
        // (last member, that member's stack order) IS the apply order: pools
        // sharing a last member then compose in the stack order of the effects
        // that own them, exactly as two in-place effects on one volume do.
        std::stable_sort(groups.begin(), groups.end(),
                         [](const HeightPoolAccum& a, const HeightPoolAccum& b) {
                             if (a.LastMember != b.LastMember)
                                 return a.LastMember < b.LastMember;
                             return a.LastMemberStackOrder < b.LastMemberStackOrder;
                         });

        for (HeightPoolAccum& group : groups)
        {
            if (!group.HasRect)
                continue;
            group.Width = group.MaxX - group.MinX + 1;
            const std::size_t count = static_cast<std::size_t>(group.Width)
                * static_cast<std::size_t>(group.MaxZ - group.MinZ + 1);
            group.Acc.assign(count, 0.0f);
            group.Wsum.assign(count, 0.0f);
            group.MaskedWsum.assign(count, 0.0f);
        }
    }

    // A pool's ONE slot in the priority order: every member has contributed, so
    // the accumulated (sum w*value, sum w) becomes a single weighted-average
    // value applied with a single blend.
    //
    // Two routes overlapping at full strength land on the average of their
    // grades; what the stack gives without this is that the later one overwrites
    // the earlier and leaves a step where its blend band ends.
    //
    // The blend is min(wsum, 1) * (maskedWsum / wsum): the CLAMP reads the
    // unmasked coverage — the weights are per-member coverage, and three routes
    // overlapping do not make the ground three times more strongly theirs — while
    // the ratio is the share of that coverage whose owner asked to be let through.
    // A pool with no claim-respecting member has maskedWsum == wsum, so the ratio
    // is exactly 1 and the expression reduces to the unmasked blend.
    //
    // Two arms, one for each thing a pooled value can BE. A height-valued pool
    // (the flatten) lerps the ground toward the average, which is what levelling
    // means. A displacement-valued pool (height offset, noise, stamp) ADDS the
    // average, so two overlapping members contribute their mean displacement
    // rather than their sum — the crossfade, not the pile-up. Lerping toward a
    // displacement would drag the ground to the displacement's magnitude as if it
    // were an altitude.
    const auto applyHeightPool = [&](HeightPoolAccum& group) {
        group.Applied = true;
        if (group.Width <= 0)
            return;
        for (int32 sz = group.MinZ; sz <= group.MaxZ; ++sz)
        {
            for (int32 sx = group.MinX; sx <= group.MaxX; ++sx)
            {
                const std::size_t gi = static_cast<std::size_t>(sz - group.MinZ)
                        * static_cast<std::size_t>(group.Width)
                    + static_cast<std::size_t>(sx - group.MinX);
                const float32 wsum = group.Wsum[gi];
                if (wsum <= 0.0f)
                    continue;

                const float32 blend = std::min(wsum, 1.0f) * (group.MaskedWsum[gi] / wsum);
                if (blend <= 0.0f)
                    continue;

                const float32 value = group.Acc[gi] / wsum;
                float32& currentHeight = samples[static_cast<size_t>(sz) * w + sx];
                if (group.ValueIsHeight)
                    currentHeight += (value - currentHeight) * blend;
                else
                    currentHeight += value * blend;
            }
        }
    };

    for (std::size_t modIndex = 0; modIndex < modifiers.size(); ++modIndex)
    {
        // Pools flush at the TOP of the iteration after their last member, not
        // at the bottom of that member's own. The member's footprint can miss
        // this bake region while an earlier member's does not, and the loop below
        // skips such a modifier outright — the pool would then accumulate and
        // never apply. Nothing runs between the two positions, so the order is
        // the same one and this one is reachable.
        for (HeightPoolAccum& group : groups)
            if (!group.Applied && group.LastMember < modIndex)
                applyHeightPool(group);

        const auto& mod = modifiers[modIndex];
        if (mod.IsSplatOnlyModifier())
            continue; // No height contribution — ApplySplatModifiers handles it.

        // Convert modifier AABB to heightfield sample range, clamped to the
        // bake region. During a region bake only region samples were reset to
        // the base — writing outside would double-apply onto baked heights.
        const int32 minSX = std::max({0, regionMinX,
                                      ModifierSampleIndex(mod.BoundsMinX, terrainOriginX, spacingX)});
        const int32 minSZ = std::max({0, regionMinZ,
                                      ModifierSampleIndex(mod.BoundsMinZ, terrainOriginZ, spacingZ)});
        const int32 maxSX = std::min({static_cast<int32>(w - 1), regionMaxX,
                                      ModifierSampleIndex(mod.BoundsMaxX, terrainOriginX, spacingX) + 1});
        const int32 maxSZ = std::min({static_cast<int32>(h - 1), regionMaxZ,
                                      ModifierSampleIndex(mod.BoundsMaxZ, terrainOriginZ, spacingZ) + 1});

        if (minSX > maxSX || minSZ > maxSZ)
            continue;

        // Stamp mask sampling basis, once per modifier (a modifier carries at
        // most one stamp: one component of a type per entity).
        const StampSampleBasis stampBasis = MakeStampBasis(mod);

        // Sculpt-zone payload UV basis: inverse rotation into zone-local space,
        // then map [-RectHalf, +RectHalf] onto [0,1] mask UV. Unlike the stamp,
        // the payload spans the rect exactly (falloff is NOT added to the
        // extent) — the falloff skirt samples the clamped edge texels.
        const bool zoneHasPayload = mod.ModType == ResolvedModifier::Type::SculptZone
            && mod.SculptZone.Offsets != nullptr
            && mod.SculptZone.Width > 0 && mod.SculptZone.Height > 0;
        float32 zoneCos = 1.0f;
        float32 zoneSin = 0.0f;
        float32 zoneInvExtX = 0.0f;
        float32 zoneInvExtZ = 0.0f;
        if (zoneHasPayload)
        {
            zoneCos = std::cos(-mod.YawRadians);
            zoneSin = std::sin(-mod.YawRadians);
            zoneInvExtX = mod.RectHalfX > 0.0f ? 1.0f / mod.RectHalfX : 0.0f;
            zoneInvExtZ = mod.RectHalfZ > 0.0f ? 1.0f / mod.RectHalfZ : 0.0f;
        }

        // Pool state, hoisted: the accumulator each poolable kind in this volume's
        // stack feeds, indexed by kind so the texel loop resolves it with one
        // indexed load instead of a search. A volume carries at most one effect of
        // each kind, so one slot per kind is exact.
        //
        // Non-null for every pooled effect present: the pool's rect for this
        // member is built from the SAME clamped expressions as the texel bounds
        // below, so a member that has texels to walk contributed a rect, and a
        // pool with a rect has a positive Width.
        HeightPoolAccum* poolForKind[static_cast<std::size_t>(ResolvedEffect::Kind::Count)] = {};
        if (mod.ModType == ResolvedModifier::Type::Volume)
        {
            for (const ResolvedEffect& fx : mod.Effects)
            {
                if (!IsPooled(fx))
                    continue;
                const StringId group = EffectPoolGroup(fx);
                const auto it = std::find_if(groups.begin(), groups.end(),
                                             [&](const HeightPoolAccum& g) {
                                                 return g.Kind == fx.EffectKind && g.Group == group;
                                             });
                if (it != groups.end() && it->Width > 0)
                    poolForKind[static_cast<std::size_t>(fx.EffectKind)] = &*it;
                // Hoisted out of the texel loop, where it would cost a branch per
                // sample.
                assert(poolForKind[static_cast<std::size_t>(fx.EffectKind)] != nullptr
                       && "a pooled effect reaching the texel loop must have a pool accumulator");
            }
        }

        // The per-station route grade is the FLATTEN's alone — no other poolable
        // kind grades along a route — so the polyline scan is conditioned on this,
        // not on pooling in general.
        const ResolvedEffect* const pooledFlatten = FindPooledFlatten(mod);

        for (int32 sz = minSZ; sz <= maxSZ; ++sz)
        {
            const float32 worldZ = terrainOriginZ + static_cast<float32>(sz) * spacingZ;
            for (int32 sx = minSX; sx <= maxSX; ++sx)
            {
                const float32 worldX = terrainOriginX + static_cast<float32>(sx) * spacingX;

                // The FOOTPRINT is the analytic curve for every shape, so the
                // weight always comes from ComputeWeight. The polyline is scanned
                // only for what only it can answer — the route grade a POOLED
                // flatten grades toward — which is why the scan is conditioned on
                // the pool rather than on the route existing. Weighting a spline
                // volume by the polyline instead would make a SplinePath volume
                // disagree with a SplineArea one over the same open spline, which
                // is pinned bytewise.
                const float32 weight = ComputeWeight(mod, worldX, worldZ);
                if (weight <= 0.0f)
                    continue;

                RouteSample routeSample{};
                const bool wantsRoute = pooledFlatten != nullptr && mod.Route != nullptr;
                if (wantsRoute)
                    routeSample = ClosestStationXZ(*mod.Route, worldX, worldZ);
                const RouteSample* const routeRef = wantsRoute ? &routeSample : nullptr;

                const size_t idx = static_cast<size_t>(sz) * w + sx;
                float32& currentHeight = samples[idx];

                if (mod.ModType == ResolvedModifier::Type::Volume)
                {
                    // The volume owns the region; its effects run in stack order
                    // over the one weight it produced, and never see the shape.
                    for (const auto& fx : mod.Effects)
                    {
                        if (ResolvedEffect::IsSplat(fx.EffectKind))
                            continue;

                        if (fx.EffectKind == ResolvedEffect::Kind::GroundClaim)
                        {
                            // Ownership only — no height is written here, which is
                            // what makes a claim safe to lay over finished ground.
                            // The splat pass folds the same operator on its own
                            // grid.
                            if (!claim.empty())
                                AccumulateGroundClaim(claim[scratchIndex(sx, sz)],
                                                      fx.GroundClaim.Strength, weight);
                            continue;
                        }

                        // The claim is read HERE, at the slot this effect applies
                        // in — the slot it accumulates in when pooled — never at a
                        // pool's flush: the mask is this effect's own answer to
                        // ownership, so a claim that lands later holds nothing back
                        // from it.
                        //
                        // The blend mode does not enter it. Ownership is about WHO
                        // holds the ground, and the operator a region happens to
                        // compose with says nothing about that; masking the WEIGHT
                        // is the one expression both arms need, because a pooled
                        // member's masked weight is its share of the pool and
                        // BlendHeight at weight 0 returns the ground unchanged in
                        // every operator. An effect that does not respect claims,
                        // or a bake with none authored, multiplies by exactly 1.0f
                        // and is bit-identical to the unmasked path.
                        const float32 claimMask = (EffectRespectClaims(fx) && !claim.empty())
                            ? 1.0f - claim[scratchIndex(sx, sz)]
                            : 1.0f;

                        if (IsPooled(fx))
                        {
                            // The same value the in-place arm would blend — one
                            // implementation, so a pooled noise and an unpooled one
                            // cannot evaluate different fields. routeRef is the
                            // per-station grade and is read by the flatten alone.
                            float32 valueNorm = 0.0f;
                            if (!HeightEffectValue(mod, fx, worldX, worldZ, worldSizeX,
                                                   heightScale, terrainOriginY, stampBasis,
                                                   routeRef, valueNorm))
                                continue; // contributes nothing here, not zero

                            // Accumulate now, apply once at the pool's slot. No
                            // in-place arm beside this one: a pooled effect writes
                            // no height at its own slot, whether it shares its pool
                            // with twelve regions or with none.
                            HeightPoolAccum* const pool =
                                poolForKind[static_cast<std::size_t>(fx.EffectKind)];
                            const std::size_t gi =
                                static_cast<std::size_t>(sz - pool->MinZ)
                                    * static_cast<std::size_t>(pool->Width)
                                + static_cast<std::size_t>(sx - pool->MinX);
                            pool->Acc[gi] += weight * valueNorm;
                            pool->Wsum[gi] += weight;
                            // The coverage clamp reads the UNMASKED Wsum: three
                            // routes over a half-owned texel still move it half way.
                            pool->MaskedWsum[gi] += weight * claimMask;
                            continue;
                        }

                        ApplyHeightEffectSample(mod, fx, currentHeight, weight * claimMask,
                                                worldX, worldZ, worldSizeX, heightScale,
                                                terrainOriginY, stampBasis);
                    }
                    continue;
                }

                switch (mod.ModType)
                {
                case ResolvedModifier::Type::SculptZone:
                {
                    if (!zoneHasPayload)
                        break;
                    // Inverse-transform the world sample into zone-local, map to
                    // payload UV, sample the R32F offset (world units), blend.
                    const float32 dx = worldX - mod.Position.x;
                    const float32 dz = worldZ - mod.Position.z;
                    const float32 lx = zoneCos * dx - zoneSin * dz;
                    const float32 lz = zoneSin * dx + zoneCos * dz;
                    const float32 offsetWorld = SampleMaskBilinear(
                        mod.SculptZone.Offsets, mod.SculptZone.Width, mod.SculptZone.Height,
                        0.5f + 0.5f * lx * zoneInvExtX,
                        0.5f + 0.5f * lz * zoneInvExtZ);
                    const float32 offsetNorm = offsetWorld / heightScale;
                    if (mod.SculptZone.Blend == Components::TerrainModifierBlend::Set)
                        currentHeight = currentHeight + (offsetNorm - currentHeight) * weight;
                    else
                        currentHeight += offsetNorm * weight;
                    break;
                }
                default:
                    break;
                }
            }
        }
    }

    // Every pool whose last member was the final modifier in the stack.
    for (HeightPoolAccum& group : groups)
        if (!group.Applied)
            applyHeightPool(group);
}

bool PackSurfaceRulesForGpuSplat(const std::vector<ResolvedModifier>& mods,
                                 std::vector<SurfaceRuleGpu>& outRules,
                                 std::vector<SurfaceRuleConditionGpu>& outConditions)
{
    outRules.clear();
    outConditions.clear();

    for (const auto& mod : mods)
    {
        if (!mod.TouchesSplat())
            continue;

        // Every splat writer that is not a volume is paint — a PaintLayer/PaintZone component or
        // a spline painting its path — and the kernel has no mask sampler, no zone payload and no
        // spline SDF to reproduce any of them.
        if (mod.ModType != ResolvedModifier::Type::Volume)
        {
            outRules.clear();
            outConditions.clear();
            return false;
        }
        // A global volume resolves to Rectangle and is handled by scope, never by footprint; any
        // other spline-shaped volume needs the SDF the kernel does not have.
        if (!mod.GlobalScope && mod.Shape == Components::TerrainModifierShape::Spline)
        {
            outRules.clear();
            outConditions.clear();
            return false;
        }

        for (const auto& fx : mod.Effects)
        {
            if (fx.EffectKind == ResolvedEffect::Kind::PaintLayer)
            {
                outRules.clear();
                outConditions.clear();
                return false;
            }
            if (fx.EffectKind != ResolvedEffect::Kind::Rules)
                continue; // a height effect on the same volume; the splat pass ignores it
            // Claim masking is a CPU-side accumulate ACROSS the modifier stack —
            // ownership folded in at each claiming volume's own slot — and the
            // kernel evaluates one rule row per texel with no such buffer to
            // read. Refused rather than packed, mirroring the height packer's
            // refusal of a pool: a packed claim-respecting block would paint
            // straight through claimed ground on the GPU and stop at it on the
            // CPU, and the settle re-splat would then disagree with the frame
            // the author was looking at.
            //
            // Checked ahead of the empty-row early-out below, so the refusal does
            // not depend on whether this particular block happens to carry rows.
            if (fx.Rules.RespectClaims)
            {
                outRules.clear();
                outConditions.clear();
                return false;
            }
            if (fx.Rules.Rules == nullptr || fx.Rules.RuleCount == 0)
                continue; // mirrors applyRules' own early-out for an empty row set

            // Counts are already capped at the gather (ClampRuleCountsToCaps, which is where the
            // author hears about it); the mins here are a backstop that agrees whenever it did.
            const uint32 ruleCount =
                std::min<uint32>(fx.Rules.RuleCount, Components::kMaxTerrainSurfaceRules);
            for (uint32 r = 0; r < ruleCount; ++r)
            {
                const Components::TerrainSurfaceRule& rule = fx.Rules.Rules[r];

                SurfaceRuleGpu g{};
                // The same clamp applyRules' clampLayer applies. Silent here because the CPU
                // re-splat at settle runs the same rows through the warning path.
                g.MaterialSlot = std::min<uint32>(rule.MaterialSlot,
                                                  Terrain::kMaxTerrainMaterialLayers - 1u);
                g.Strength = rule.Strength;
                g.Replace = rule.Replace ? 1u : 0u;
                g.ConditionCount = std::min<uint32>(rule.ConditionCount,
                                                    Components::kMaxTerrainRuleConditions);
                g.ConditionBase = static_cast<uint32>(outConditions.size());
                g.Shape = static_cast<uint32>(
                    mod.GlobalScope
                        ? SurfaceRuleGpuShape::Global
                        : (mod.Shape == Components::TerrainModifierShape::Circle
                               ? SurfaceRuleGpuShape::Circle
                               : SurfaceRuleGpuShape::Rectangle));
                g.CenterX = mod.Position.x;
                g.CenterZ = mod.Position.z;
                g.Yaw = mod.YawRadians;
                g.Radius = mod.Radius;
                g.RectHalfX = mod.RectHalfX;
                g.RectHalfZ = mod.RectHalfZ;
                g.Falloff = mod.Falloff;
                g.FalloffInward = mod.FalloffInward;
                g.VolumeWeight = mod.Weight;

                for (uint32 c = 0; c < g.ConditionCount; ++c)
                {
                    const Components::TerrainRuleCondition& cond = rule.Conditions[c];
                    SurfaceRuleConditionGpu cg{};
                    cg.Kind = static_cast<uint32>(cond.Kind);
                    cg.Curve = static_cast<uint32>(cond.FalloffCurve);
                    cg.Min = cond.Min;
                    cg.Max = cond.Max;
                    cg.Feather = cond.Feather;
                    cg.NoiseFrequency = cond.NoiseFrequency;
                    cg.NoiseSeed = cond.NoiseSeed;
                    outConditions.push_back(cg);
                }
                outRules.push_back(g);
            }
        }
    }
    return true;
}

void TerrainModifierSystem::ApplySplatModifiers(std::vector<uint8>& splatmap,
                                                 uint32 splatmapWidth, uint32 splatmapHeight,
                                                 const Terrain::HeightfieldData& heightfield,
                                                 float32 worldSizeX, float32 worldSizeZ,
                                                 float32 terrainOriginX,
                                                 float32 terrainOriginZ,
                                                 float32 heightScale,
                                                 float32 splatMinH, float32 splatMaxH,
                                                 const std::vector<ResolvedModifier>& modifiers,
                                                 int32 regionMinX, int32 regionMinZ,
                                                 int32 regionMaxX, int32 regionMaxZ)
{
    if (splatmap.empty())
        return;

    const uint32 w = splatmapWidth;
    const uint32 h = splatmapHeight;
    if (w == 0 || h == 0)
        return;

    const float32 spacingX = worldSizeX / static_cast<float32>(w - 1);
    const float32 spacingZ = worldSizeZ / static_cast<float32>(h - 1);

    // Surface rules measure the heightfield at the splat's own indices, which is
    // exact only while the two grids are the same size — as every generator
    // makes them. Paint needs none of this, so a mismatch disables rules alone
    // rather than dropping the whole pass.
    const bool rulesAddressable =
        heightfield.GetWidth() == w && heightfield.GetHeight() == h;
    const float32 heightRange = std::max(splatMaxH - splatMinH, 0.001f);

    // ---- Ground claims, on the SPLAT's own grid --------------------------------
    //
    // Evaluated here rather than borrowed from the height pass: the claim field is
    // a function of (modifier set, world XZ), and the splat grid and the
    // heightfield grid are not guaranteed to be the same size — the rules path
    // already checks exactly that and disables rules alone when they differ.
    //
    // Local to this call, like the height pass's: ApplySplatModifiers runs per
    // tile and tiles bake across job workers, so anything held on the system
    // would be shared mutable state between them. Allocated only when a claim is
    // actually authored, so a scene without one pays nothing.
    const int32 claimMinX = std::max(0, regionMinX);
    const int32 claimMinZ = std::max(0, regionMinZ);
    const int32 claimMaxX = std::min(static_cast<int32>(w - 1), regionMaxX);
    const int32 claimMaxZ = std::min(static_cast<int32>(h - 1), regionMaxZ);
    const bool claimRegionEmpty = claimMinX > claimMaxX || claimMinZ > claimMaxZ;

    std::vector<float32> claim;
    if (!claimRegionEmpty
        && std::any_of(modifiers.begin(), modifiers.end(), [](const ResolvedModifier& m) {
               return m.ModType == ResolvedModifier::Type::Volume
                   && ModifierHasKind(m, ResolvedEffect::Kind::GroundClaim);
           }))
    {
        claim.assign(static_cast<std::size_t>(claimMaxX - claimMinX + 1)
                         * static_cast<std::size_t>(claimMaxZ - claimMinZ + 1),
                     0.0f);
    }
    const int32 claimWidth = claimMaxX - claimMinX + 1;
    const auto claimIndex = [&](int32 sx, int32 sz) {
        return static_cast<std::size_t>(sz - claimMinZ) * static_cast<std::size_t>(claimWidth)
             + static_cast<std::size_t>(sx - claimMinX);
    };
    // 1 where nothing is owned, or where this effect does not defer. Exactly
    // 1.0f, so an effect that ignores claims multiplies by a value that changes
    // no bits and composites byte-identically to the unmasked path.
    const auto claimMaskAt = [&](bool respectClaims, int32 sx, int32 sz) {
        return (respectClaims && !claim.empty()) ? 1.0f - claim[claimIndex(sx, sz)] : 1.0f;
    };

    // Out of range clamps to the last channel and says so. Skipping instead would drop the
    // WHOLE modifier, so a hand-edited or scripted scene got a silently inert paint stroke
    // with nothing in the log to explain it.
    auto clampLayer = [&](uint32 layerIdx, const char* what) {
        if (layerIdx < Terrain::kMaxTerrainMaterialLayers)
            return layerIdx;
        if (m_LayerClampWarnings.First(layerIdx))
        {
            Logger::Log::Warning(
                "Terrain.Paint layer index {} is out of range (max {}); clamping. Check the "
                "{} setting.",
                layerIdx, Terrain::kMaxTerrainMaterialLayers - 1u, what);
        }
        return Terrain::kMaxTerrainMaterialLayers - 1u;
    };

    for (const auto& mod : modifiers)
    {
        const bool isPaintZone = (mod.ModType == ResolvedModifier::Type::PaintZone);
        const bool isVolume = (mod.ModType == ResolvedModifier::Type::Volume);
        // A claim-only volume writes no splat and would otherwise be skipped
        // here — but its ownership has to reach the buffer at ITS slot, or a
        // claim would only ever hold back effects on volumes that happen to
        // paint as well.
        const bool volumeClaims =
            isVolume && !claim.empty() && ModifierHasKind(mod, ResolvedEffect::Kind::GroundClaim);
        if (!isPaintZone && !(isVolume && (mod.TouchesSplat() || volumeClaims)))
            continue;

        // Paint-zone mask basis: inverse rotation into zone-local, map the rect
        // onto [0,1] mask UV (same convention as the sculpt payload).
        const bool zoneHasMask = isPaintZone && mod.PaintZone.Mask != nullptr
            && mod.PaintZone.Width > 0 && mod.PaintZone.Height > 0;
        float32 zoneCos = 1.0f;
        float32 zoneSin = 0.0f;
        float32 zoneInvExtX = 0.0f;
        float32 zoneInvExtZ = 0.0f;
        if (zoneHasMask)
        {
            zoneCos = std::cos(-mod.YawRadians);
            zoneSin = std::sin(-mod.YawRadians);
            zoneInvExtX = mod.RectHalfX > 0.0f ? 1.0f / mod.RectHalfX : 0.0f;
            zoneInvExtZ = mod.RectHalfZ > 0.0f ? 1.0f / mod.RectHalfZ : 0.0f;
        }
        // A paint zone with no resident mask paints nothing (unlike stamps,
        // there is no flat-white fallback — an empty mask means "not authored").
        if (isPaintZone && !zoneHasMask)
            continue;

        // Clamp to the bake region: only region texels were regenerated from
        // the fresh heightfield — painting outside would double-apply.
        const int32 minSX = std::max({0, regionMinX,
                                      ModifierSampleIndex(mod.BoundsMinX, terrainOriginX, spacingX)});
        const int32 minSZ = std::max({0, regionMinZ,
                                      ModifierSampleIndex(mod.BoundsMinZ, terrainOriginZ, spacingZ)});
        const int32 maxSX = std::min({static_cast<int32>(w - 1), regionMaxX,
                                      ModifierSampleIndex(mod.BoundsMaxX, terrainOriginX, spacingX) + 1});
        const int32 maxSZ = std::min({static_cast<int32>(h - 1), regionMaxZ,
                                      ModifierSampleIndex(mod.BoundsMaxZ, terrainOriginZ, spacingZ) + 1});

        if (minSX > maxSX || minSZ > maxSZ)
            continue;

        // Fold this volume's ownership into the claim buffer at its own slot, so
        // effects that apply AFTER it stop at it and effects before it do not.
        auto accumulateClaim = [&](float32 strength) {
            for (int32 sz = minSZ; sz <= maxSZ; ++sz)
            {
                const float32 worldZ = terrainOriginZ + static_cast<float32>(sz) * spacingZ;
                for (int32 sx = minSX; sx <= maxSX; ++sx)
                {
                    const float32 worldX = terrainOriginX + static_cast<float32>(sx) * spacingX;
                    const float32 weight = ComputeWeight(mod, worldX, worldZ);
                    if (weight <= 0.0f)
                        continue;
                    AccumulateGroundClaim(claim[claimIndex(sx, sz)], strength, weight);
                }
            }
        };

        // One paint write over the modifier's clamped region.
        auto applyPaint = [&](uint32 layerIdx, float32 strength, bool replace,
                              bool respectClaims) {
            for (int32 sz = minSZ; sz <= maxSZ; ++sz)
            {
                const float32 worldZ = terrainOriginZ + static_cast<float32>(sz) * spacingZ;
                for (int32 sx = minSX; sx <= maxSX; ++sx)
                {
                    const float32 worldX = terrainOriginX + static_cast<float32>(sx) * spacingX;
                    // Claimed ground keeps what the claimant painted. Masked to
                    // zero, the `weight <= 0` early-out below skips the composite
                    // and the texel is left exactly as it was — and that skip is
                    // the mechanism, not an optimization: the additive arm
                    // RENORMALIZES, so compositing zero weight would still
                    // re-quantize a texel whose channels do not divide 255
                    // exactly, and a region bake replays the whole stack.
                    float32 weight = ComputeWeight(mod, worldX, worldZ) * strength
                                   * claimMaskAt(respectClaims, sx, sz);
                    if (zoneHasMask)
                    {
                        const float32 dx = worldX - mod.Position.x;
                        const float32 dz = worldZ - mod.Position.z;
                        const float32 lx = zoneCos * dx - zoneSin * dz;
                        const float32 lz = zoneSin * dx + zoneCos * dz;
                        weight *= SampleMaskR8Bilinear(
                            mod.PaintZone.Mask, mod.PaintZone.Width, mod.PaintZone.Height,
                            0.5f + 0.5f * lx * zoneInvExtX,
                            0.5f + 0.5f * lz * zoneInvExtZ);
                    }
                    if (weight <= 0.0f)
                        continue;

                    CompositeSplatTexel(&splatmap[(static_cast<size_t>(sz) * w + sx) * 4],
                                        layerIdx, weight, replace);
                }
            }
        };

        // One rules effect over the same region: each texel measured once, then
        // every row composited in authored order.
        auto applyRules = [&](const ModifierSurfaceRulesParams& rules) {
            if (rules.RuleCount == 0)
                return;
            if (!rulesAddressable)
            {
                if (m_UnaddressableRulesWarnings.First(HashCombine(HashCombine(HashCombine(w, h),
                                                                          heightfield.GetWidth()),
                                                              heightfield.GetHeight())))
                {
                    Logger::Log::Warning(
                        "Terrain.SurfaceRules splatmap is {}x{} but the heightfield is {}x{}; the "
                        "rules cannot measure slope or height and are SKIPPED for this bake.",
                        w, h, heightfield.GetWidth(), heightfield.GetHeight());
                }
                return;
            }

            for (int32 sz = minSZ; sz <= maxSZ; ++sz)
            {
                const float32 worldZ = terrainOriginZ + static_cast<float32>(sz) * spacingZ;
                for (int32 sx = minSX; sx <= maxSX; ++sx)
                {
                    const float32 worldX = terrainOriginX + static_cast<float32>(sx) * spacingX;
                    // Masked once here rather than per row: every row's weight is
                    // this volume weight times the row's own, so scaling it is
                    // exactly `weight *= 1 - claim` for each of them — and a
                    // fully-claimed texel costs nothing, because the early-out
                    // below catches it before any row is evaluated.
                    const float32 volumeWeight = ComputeWeight(mod, worldX, worldZ)
                                               * claimMaskAt(rules.RespectClaims, sx, sz);
                    if (volumeWeight <= 0.0f)
                        continue;

                    const auto normal = heightfield.ComputeNormal(sx, sz, spacingX, spacingZ);
                    const float32 sampleH = heightfield.GetSample(static_cast<uint32>(sx),
                                                                  static_cast<uint32>(sz));

                    const TerrainRuleSample sample =
                        MakeTerrainRuleSample(normal.x, normal.y, normal.z, sampleH, heightScale,
                                              splatMinH, heightRange, worldX, worldZ);

                    uint8* pixel = &splatmap[(static_cast<size_t>(sz) * w + sx) * 4];
                    for (uint32 r = 0; r < rules.RuleCount; ++r)
                    {
                        const Components::TerrainSurfaceRule& rule = rules.Rules[r];
                        const float32 weight =
                            EvaluateTerrainSurfaceRuleWeight(rule, sample, SurfaceRuleNoiseSample)
                            * volumeWeight;
                        if (weight <= 0.0f)
                            continue;
                        CompositeSplatTexel(pixel, clampLayer(rule.MaterialSlot, "rule's Material"),
                                            weight, rule.Replace);
                    }
                }
            }
        };

        if (isVolume)
        {
            // Stack order is the whole point: a rules row and a paint stroke on
            // one volume composite in the order the inspector shows them — and a
            // claim on the same volume takes effect for the effects stacked
            // above it, exactly as it does on the height side.
            for (const auto& fx : mod.Effects)
            {
                if (fx.EffectKind == ResolvedEffect::Kind::GroundClaim)
                {
                    if (!claim.empty())
                        accumulateClaim(fx.GroundClaim.Strength);
                }
                else if (fx.EffectKind == ResolvedEffect::Kind::PaintLayer)
                    applyPaint(clampLayer(fx.PaintLayer.LayerIndex, "paint modifier's Layer"),
                               fx.PaintLayer.Strength, fx.PaintLayer.Replace,
                               fx.PaintLayer.RespectClaims);
                else if (fx.EffectKind == ResolvedEffect::Kind::Rules)
                    applyRules(fx.Rules);
            }
        }
        else // PaintZone
        {
            // Zones accumulate + renormalize (PaintLayer math).
            applyPaint(clampLayer(mod.PaintZone.LayerIndex, "paint zone's Layer"),
                       mod.PaintZone.Strength, false, /*respectClaims=*/false);
        }
    }
}

void TerrainModifierSystem::ComposeTileHeights(Terrain::HeightfieldData& field,
                                               const TiledTerrainData& tiled,
                                               float32 tileWorldOriginX,
                                               float32 tileWorldOriginZ,
                                               float32 heightScale, float32 terrainOriginY,
                                               const std::vector<ResolvedModifier>& modifiers,
                                               int32 minX, int32 minZ, int32 maxX, int32 maxZ)
{
    const float32 tileWorldSize = tiled.Config.TileWorldSize;
    FillTiledBaseRegion(field, tiled.Config, tiled.WorldOriginX, tiled.WorldOriginZ,
                        tileWorldOriginX, tileWorldOriginZ, tileWorldSize, tileWorldSize,
                        minX, minZ, maxX, maxZ);
    ApplyHeightModifiers(field, tileWorldSize, tileWorldSize, heightScale, tileWorldOriginX,
                         terrainOriginY, tileWorldOriginZ, modifiers, minX, minZ, maxX, maxZ);
}

bool TerrainModifierSystem::ComposeTiledGroundBlock(const TiledTerrainData& tiled,
                                                    float32 heightScale, float32 terrainOriginY,
                                                    int64 firstLatticeX, int64 firstLatticeZ,
                                                    uint32 countX, uint32 countZ,
                                                    std::vector<float32>& outNormalized) const
{
    return ComposeLatticeBlock(tiled, heightScale, terrainOriginY, m_AppliedModifiers, firstLatticeX, firstLatticeZ,
                               countX, countZ, outNormalized);
}

bool TerrainModifierSystem::ComposeLatticeBlock(const TiledTerrainData& tiled, float32 heightScale,
                                                float32 terrainOriginY,
                                                const std::vector<ResolvedModifier>& modifiers,
                                                int64 firstLatticeX, int64 firstLatticeZ, uint32 countX,
                                                uint32 countZ, std::vector<float32>& outNormalized)
{
    const uint32 tileW = tiled.Config.TileConfig.HeightmapWidth;
    const uint32 tileH = tiled.Config.TileConfig.HeightmapHeight;
    const float32 tileSize = tiled.Config.TileWorldSize;
    if (tileW < 2u || tileH < 2u || !(tileSize > 0.0f) || countX == 0u || countZ == 0u)
        return false;

    // Adjacent tiles SHARE their edge sample, so the global lattice advances by
    // (dim - 1) per tile: global index g sits on tile g / interior at in-tile
    // index g % interior. A g exactly on a seam resolves to the lower tile's last
    // sample or the upper tile's first; both name the same world position, and the
    // base and the modifier stack both read world position, so they agree.
    const int64 interiorX = static_cast<int64>(tileW) - 1;
    const int64 interiorZ = static_cast<int64>(tileH) - 1;
    const auto floorDiv = [](int64 a, int64 b) -> int64
    {
        return a >= 0 ? a / b : -(((-a) + b - 1) / b);
    };

    outNormalized.assign(static_cast<std::size_t>(countX) * countZ, 0.0f);

    const int64 lastLatticeX = firstLatticeX + static_cast<int64>(countX) - 1;
    const int64 lastLatticeZ = firstLatticeZ + static_cast<int64>(countZ) - 1;
    // Start one tile LOW on each axis: a block beginning exactly on a seam divides
    // to the upper tile, but that sample is also the lower tile's last column — and
    // when the upper tile is off the grid (a block starting on the terrain's far
    // edge) the lower one is the only tile that holds it. The sub-rect clamp below
    // rejects any tile that turns out not to overlap, so the extra step costs
    // nothing but removes the edge case.
    const int64 firstTileX = floorDiv(firstLatticeX, interiorX) - 1;
    const int64 lastTileX = floorDiv(lastLatticeX, interiorX);
    const int64 firstTileZ = floorDiv(firstLatticeZ, interiorZ) - 1;
    const int64 lastTileZ = floorDiv(lastLatticeZ, interiorZ);

    // One scratch tile, reused. The composition needs the tile's FULL dimensions
    // even when only a sub-rect is filled: both steps derive their spacing from
    // tileSize / (dim - 1), so a smaller grid would sample a different lattice.
    Terrain::HeightfieldData scratch;
    scratch.Resize(tileW, tileH, 0.0f);

    for (int64 tz = firstTileZ; tz <= lastTileZ; ++tz)
    {
        if (tz < 0 || tz >= static_cast<int64>(tiled.Config.TilesPerAxisZ))
            continue;
        for (int64 tx = firstTileX; tx <= lastTileX; ++tx)
        {
            if (tx < 0 || tx >= static_cast<int64>(tiled.Config.TilesPerAxisX))
                continue;

            // The block's slice of this tile, in the tile's own sample indices.
            const int64 tileFirstGlobalX = tx * interiorX;
            const int64 tileFirstGlobalZ = tz * interiorZ;
            const int32 minIx = static_cast<int32>(
                std::max<int64>(0, firstLatticeX - tileFirstGlobalX));
            const int32 maxIx = static_cast<int32>(
                std::min<int64>(interiorX, lastLatticeX - tileFirstGlobalX));
            const int32 minIz = static_cast<int32>(
                std::max<int64>(0, firstLatticeZ - tileFirstGlobalZ));
            const int32 maxIz = static_cast<int32>(
                std::min<int64>(interiorZ, lastLatticeZ - tileFirstGlobalZ));
            if (minIx > maxIx || minIz > maxIz)
                continue;

            const float32 tileOriginX =
                tiled.WorldOriginX + static_cast<float32>(tx) * tileSize;
            const float32 tileOriginZ =
                tiled.WorldOriginZ + static_cast<float32>(tz) * tileSize;

            // The same composition the bakes run, because it is literally the same
            // function. Clamping to the sub-rect is exact: both steps inside it
            // resolve per world sample, so a clamped pass equals a full one within
            // its rect — which is also what lets BakeTiledRegion re-bake a dirty
            // rect and leave the rest of the tile standing.
            ComposeTileHeights(scratch, tiled, tileOriginX, tileOriginZ, heightScale,
                               terrainOriginY, modifiers, minIx, minIz, maxIx, maxIz);

            for (int32 iz = minIz; iz <= maxIz; ++iz)
            {
                const int64 outZ = tileFirstGlobalZ + iz - firstLatticeZ;
                for (int32 ix = minIx; ix <= maxIx; ++ix)
                {
                    const int64 outX = tileFirstGlobalX + ix - firstLatticeX;
                    outNormalized[static_cast<std::size_t>(outZ) * countX +
                                  static_cast<std::size_t>(outX)] =
                        scratch.GetSample(static_cast<uint32>(ix), static_cast<uint32>(iz));
                }
            }
        }
    }
    return true;
}

void TerrainModifierSystem::BakeTiledFull(TiledTerrainData& tiled, float32 heightScale,
                                          float32 terrainOriginY,
                                          const std::vector<ResolvedModifier>& modifiers,
                                          std::vector<TileCoord>& outBakedTiles, bool gpuEvalSkip)
{
    const float32 tileSize = tiled.Config.TileWorldSize;

    // GPU eval-skip (slice-1c): the GPU produces every tile's height into the atlas; the CPU
    // heightfield stays stale until the settle readback refreshes it, so skip ALL CPU height
    // work (fill / modifiers / range / splat / quadtree) and only record the tiles to dispatch.
    if (gpuEvalSkip)
    {
        for (auto& [coord, tilePtr] : tiled.Tiles)
        {
            if (!tilePtr || tilePtr->LodState != TileLodState::Full) continue;
            if (tilePtr->Heightfield.GetWidth() < 2 || tilePtr->Heightfield.GetHeight() < 2) continue;
            RecordGpuBakeTile(tiled, *tilePtr, coord, tileSize, 0, 0,
                              static_cast<int32>(tilePtr->Heightfield.GetWidth()) - 1,
                              static_cast<int32>(tilePtr->Heightfield.GetHeight()) - 1);
            outBakedTiles.push_back(coord);
        }
        return;
    }

    // Scheduler dependencies keep this tile set stable throughout the caller's modifier-access
    // scope and both joins. Workers own distinct tiles; shared range/publication state stays here.
    std::vector<std::pair<TileCoord, TerrainTileData*>> fullTiles;
    uint64 totalTexels = 0;
    for (auto& [coord, tilePtr] : tiled.Tiles)
    {
        if (!tilePtr || tilePtr->LodState != TileLodState::Full) continue;
        fullTiles.emplace_back(coord, tilePtr.get());
        totalTexels += tilePtr->Heightfield.GetSampleCount();
    }
    const bool parallel = ModifierParallelBakeEnabled() && m_JobPool != nullptr &&
                          fullTiles.size() > 1u && totalTexels >= kParallelBakeMinTexels;
    auto runTiles = [&](auto&& tileFn) {
        if (!parallel)
        {
            for (const auto& [coord, tile] : fullTiles)
                tileFn(*tile);
            return;
        }
        // Match the region bake's fork/join: Wait helps drain jobs, including when this system
        // is already running on a worker inside a scheduler wave. ParallelFor cannot join there.
        JobSystem::JobCounter counter;
        for (const auto& [coord, tile] : fullTiles)
            m_JobPool->Run([&tileFn, tile]() { tileFn(*tile); }, counter);
        m_JobPool->Wait(counter);
    };

    // Pass 1: regenerate the base + apply height modifiers on every Full tile.
    const auto heightStart = std::chrono::steady_clock::now();
    runTiles([&](TerrainTileData& tile) {
        ComposeTileHeights(tile.Heightfield, tiled, tile.WorldOriginX, tile.WorldOriginZ,
                           heightScale, terrainOriginY, modifiers,
                           0, 0, kRegionUnbounded, kRegionUnbounded);
        // Keep the per-tile cached range current so the O(tiles) global aggregate
        // below (ComputeResidentGlobalHeightRange) never re-scans all tile samples.
        RefreshTileHeightRange(tile);
    });
    const auto heightEnd = std::chrono::steady_clock::now();

    // Global height range for consistent splat generation across tiles.
    float32 globalMinH, globalMaxH;
    ComputeResidentGlobalHeightRange(tiled, globalMinH, globalMaxH);

    // Pass 2: all heights and the shared normalization range are final before any splat worker
    // samples them. Each splat vector and its dimensions belong to one tile exclusively.
    const auto splatStart = std::chrono::steady_clock::now();
    runTiles([&](TerrainTileData& tile) {
        ResetSplatmap(
            tile.Heightfield, tile.Splatmap, tile.SplatmapWidth, tile.SplatmapHeight);
        ApplySplatModifiers(tile.Splatmap, tile.SplatmapWidth, tile.SplatmapHeight,
                            tile.Heightfield, tileSize, tileSize,
                            tile.WorldOriginX, tile.WorldOriginZ,
                            heightScale, globalMinH, globalMaxH,
                            modifiers, 0, 0, kRegionUnbounded, kRegionUnbounded);
    });
    const auto splatEnd = std::chrono::steady_clock::now();
    for (const auto& [coord, tile] : fullTiles)
    {
        tile->MarkFullDirty();
        tile->SplatmapDirty = true;
        tile->ModifiersApplied = true;
        outBakedTiles.push_back(coord);
    }
    if (BakeTimingEnabled() && !fullTiles.empty())
        Logger::Log::Info("Terrain.BakeTiming full heightMs={} splatMs={} tiles={} parallel={}",
            std::chrono::duration<double, std::milli>(heightEnd - heightStart).count(),
            std::chrono::duration<double, std::milli>(splatEnd - splatStart).count(),
            fullTiles.size(), parallel ? 1 : 0);

    tiled.SplatBakeMinH = globalMinH;
    tiled.SplatBakeMaxH = globalMaxH;
    tiled.SplatBakeRangeValid = true;
    tiled.CachedGlobalMinH = globalMinH;
    tiled.CachedGlobalMaxH = globalMaxH;

    ++tiled.Revision;
    tiled.QuadtreeDirty = true;
    tiled.GlobalHeightRangeDirty = true;
}

// Re-bake EVERY resident Full tile's splatmap against (globalMinH, globalMaxH) and commit
// that range. Used for the first bake (no committed range) and for the deferred
// stroke-settle flush. Whole-terrain cost (reset + rule evaluation per texel), so it must
// run at most once per settle — an active stroke defers to it via SplatResplatPending.
void TerrainModifierSystem::RenormalizeAllTileSplats(TiledTerrainData& tiled, float32 tileSize,
                                     const std::vector<ResolvedModifier>& modifiers,
                                     float32 globalMinH, float32 globalMaxH,
                                     std::vector<TileCoord>& outBakedTiles)
{
    GE_CPU_PROFILE_SCOPE("Terrain.SplatRenormalize");
    for (auto& [coord, tilePtr] : tiled.Tiles)
    {
        if (!tilePtr || tilePtr->LodState != TileLodState::Full) continue;
        ResetSplatmap(
            tilePtr->Heightfield,
            tilePtr->Splatmap, tilePtr->SplatmapWidth, tilePtr->SplatmapHeight);
        ApplySplatModifiers(tilePtr->Splatmap, tilePtr->SplatmapWidth, tilePtr->SplatmapHeight,
                            tilePtr->Heightfield, tileSize, tileSize,
                            tilePtr->WorldOriginX, tilePtr->WorldOriginZ,
                            tiled.Config.HeightScale, globalMinH, globalMaxH,
                            modifiers, 0, 0, kRegionUnbounded, kRegionUnbounded);
        tilePtr->SplatmapDirty = true;
        outBakedTiles.push_back(coord);
    }
    tiled.SplatBakeMinH = globalMinH;
    tiled.SplatBakeMaxH = globalMaxH;
    tiled.SplatBakeRangeValid = true;
}

std::size_t TerrainModifierSystem::RenormalizeTileSplatRows(
    TerrainTileData& tile, float32 tileSize, float32 heightScale,
    const std::vector<ResolvedModifier>& modifiers,
    float32 globalMinH, float32 globalMaxH, int32 rowStart, int32 rowEnd)
{
    const int32 w = static_cast<int32>(tile.Heightfield.GetWidth());
    const int32 h = static_cast<int32>(tile.Heightfield.GetHeight());
    if (w < 2 || h < 2)
        return 0;
    // Ensure the splatmap is sized to the heightfield. A tile that became resident without
    // a baked splat (a stream-in no modifier touched) needs one sized here; every band
    // resets then re-bakes its rows, so the settled splat is byte-identical to
    // RenormalizeAllTileSplats' whole-tile path (which also sizes then resets). Cheap size
    // check per band; the assign fires at most once.
    const std::size_t need = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
    if (tile.Splatmap.size() != need || tile.SplatmapWidth != static_cast<uint32>(w) ||
        tile.SplatmapHeight != static_cast<uint32>(h))
    {
        tile.Splatmap.assign(need, 0);
        tile.SplatmapWidth = static_cast<uint32>(w);
        tile.SplatmapHeight = static_cast<uint32>(h);
    }
    rowStart = std::max(rowStart, 0);
    rowEnd = std::min(rowEnd, h - 1);
    if (rowStart > rowEnd)
        return 0;
    // Region variant re-bakes only splat rows [rowStart,rowEnd]; the splatmap is
    // already sized (the tile was splat-baked before the deferral). Reading heights
    // at ±1 across a row boundary is safe — heights are frozen during settle.
    ResetSplatmapRegion(
        tile.Heightfield, tile.Splatmap,
        0, rowStart, w - 1, rowEnd);
    ApplySplatModifiers(tile.Splatmap, tile.SplatmapWidth, tile.SplatmapHeight,
                        tile.Heightfield, tileSize, tileSize,
                        tile.WorldOriginX, tile.WorldOriginZ,
                        heightScale, globalMinH, globalMaxH,
                        modifiers, 0, rowStart, w - 1, rowEnd);
    const std::size_t texels = static_cast<std::size_t>(w) *
                               static_cast<std::size_t>(rowEnd - rowStart + 1);
    g_SplatRenormalizeTexelCount.fetch_add(texels, std::memory_order_relaxed);
    return texels;
}

bool TerrainModifierSystem::RunBakeRowBands(
    std::span<const BakeBandRegion> regions,
    const std::function<void(std::size_t, int32, int32, int32, int32)>& rowFn)
{
    uint64 totalTexels = 0;
    for (const BakeBandRegion& r : regions)
        totalTexels += static_cast<uint64>(r.MaxX - r.MinX + 1) * static_cast<uint64>(r.MaxZ - r.MinZ + 1);
    const bool parallel =
        ModifierParallelBakeEnabled() && m_JobPool != nullptr && totalTexels >= kParallelBakeMinTexels;
    if (!parallel)
    {
        for (std::size_t i = 0; i < regions.size(); ++i)
            rowFn(i, regions[i].MinX, regions[i].MaxX, regions[i].MinZ, regions[i].MaxZ);
        m_LastBakeBandCount = 0;
        return false;
    }
    // Flatten every region into fixed-height row bands so the pool stays busy even
    // when there are few regions. The per-sample noise, modifier and splat evaluators
    // are pure functions of world position, so a band split is byte-identical to one
    // whole-region call. Bands are launched through the JobCounter fork-join (Run +
    // Wait), NOT ParallelFor: the wave scheduler runs a multi-system wave's members on
    // pool workers, and this bake is one of them. Wait(counter) participates in
    // draining the bands (the joining worker executes them itself), so it is safe to
    // join from a worker, where ParallelFor's barrier is not.
    struct Band { std::size_t Region; int32 MinX, MaxX, Z0, Z1; };
    std::vector<Band> bands;
    for (std::size_t i = 0; i < regions.size(); ++i)
        for (int32 z = regions[i].MinZ; z <= regions[i].MaxZ; z += kBakeBandRows)
            bands.push_back(Band{i, regions[i].MinX, regions[i].MaxX, z,
                                 std::min(z + kBakeBandRows - 1, regions[i].MaxZ)});
    JobSystem::JobCounter counter;
    for (const Band& band : bands)
        m_JobPool->Run([&rowFn, band]() { rowFn(band.Region, band.MinX, band.MaxX, band.Z0, band.Z1); },
                       counter);
    m_JobPool->Wait(counter);
    m_LastBakeBandCount = static_cast<uint32>(bands.size());
    return true;
}

bool TerrainModifierSystem::BakeSingleTerrainHeights(TerrainData& data, Components::TerrainBaseSource baseSource,
                                                     const Terrain::HeightfieldData* baseHeightmap, float32 originX,
                                                     float32 originY, float32 originZ,
                                                     const std::vector<ResolvedModifier>& modifiers, int32 minX,
                                                     int32 minZ, int32 maxX, int32 maxZ)
{
    const BakeBandRegion rect{minX, minZ, maxX, maxZ};
    return RunBakeRowBands({&rect, 1}, [&](std::size_t, int32 x0, int32 x1, int32 z0, int32 z1) {
        FillHeightfieldBaseRegion(data.Heightfield, baseSource, baseHeightmap, x0, z0, x1, z1);
        ApplyHeightModifiers(data.Heightfield, data.Config.WorldSizeX, data.Config.WorldSizeZ,
                             data.Config.HeightScale, originX, originY, originZ, modifiers, x0, z0, x1, z1);
    });
}

void TerrainModifierSystem::BakeSingleTerrainSplat(TerrainData& data, float32 originX, float32 originZ,
                                                   float32 minH, float32 maxH, bool resetRect,
                                                   const std::vector<ResolvedModifier>& modifiers, int32 minX,
                                                   int32 minZ, int32 maxX, int32 maxZ)
{
    const BakeBandRegion rect{minX, minZ, maxX, maxZ};
    RunBakeRowBands({&rect, 1}, [&](std::size_t, int32 x0, int32 x1, int32 z0, int32 z1) {
        if (resetRect)
            ResetSplatmapRegion(data.Heightfield, data.Splatmap, x0, z0, x1, z1);
        ApplySplatModifiers(data.Splatmap, data.SplatmapWidth, data.SplatmapHeight, data.Heightfield,
                            data.Config.WorldSizeX, data.Config.WorldSizeZ, originX, originZ,
                            data.Config.HeightScale, minH, maxH, modifiers, x0, z0, x1, z1);
    });
}

void TerrainModifierSystem::BakeTiledRegion(TiledTerrainData& tiled, float32 heightScale,
                                            float32 terrainOriginY,
                                            const std::vector<ResolvedModifier>& modifiers,
                                            const DirtyUnion& heightDirty, const DirtyUnion& splatDirty,
                                            std::vector<TileCoord>& outBakedTiles, bool gpuEvalSkip)
{
    const float32 tileSize = tiled.Config.TileWorldSize;

    // Clamp a world dirty rect to a tile's local sample rect [min,max]
    // (inclusive), padded by `pad`. False when the tile doesn't intersect it.
    auto tileSampleRect = [&](const TerrainTileData& tile, const DirtyUnion& d, int32 pad,
                              int32& outMinX, int32& outMinZ, int32& outMaxX, int32& outMaxZ) -> bool
    {
        if (!d.Any) return false;
        const uint32 w = tile.Heightfield.GetWidth();
        const uint32 h = tile.Heightfield.GetHeight();
        if (w < 2 || h < 2) return false;
        const float32 originX = tile.WorldOriginX;
        const float32 originZ = tile.WorldOriginZ;
        if (d.MaxX < originX || d.MinX > originX + tileSize ||
            d.MaxZ < originZ || d.MinZ > originZ + tileSize)
            return false; // disjoint
        const float32 spacingX = tileSize / static_cast<float32>(w - 1);
        const float32 spacingZ = tileSize / static_cast<float32>(h - 1);
        outMinX = std::max(0, ModifierSampleIndex(d.MinX, originX, spacingX) - pad);
        outMinZ = std::max(0, ModifierSampleIndex(d.MinZ, originZ, spacingZ) - pad);
        outMaxX = std::min(static_cast<int32>(w - 1),
                           ModifierSampleIndex(d.MaxX, originX, spacingX) + 1 + pad);
        outMaxZ = std::min(static_cast<int32>(h - 1),
                           ModifierSampleIndex(d.MaxZ, originZ, spacingZ) + 1 + pad);
        return outMinX <= outMaxX && outMinZ <= outMaxZ;
    };

    // The height and splat passes run each tile's sample rect through RunBakeRowBands:
    // row bands fanned across workers for a wide edit, whole and serial for a dab.
    struct BakeRegionRect { TerrainTileData* Tile; TileCoord Coord; int32 MinX, MinZ, MaxX, MaxZ; };
    bool bakeWasParallel = false;
    auto runRegionBands = [this, &bakeWasParallel](const std::vector<BakeRegionRect>& regions,
                                                   auto&& rowFn) {
        std::vector<BakeBandRegion> rects;
        rects.reserve(regions.size());
        for (const auto& r : regions)
            rects.push_back(BakeBandRegion{r.MinX, r.MinZ, r.MaxX, r.MaxZ});
        bakeWasParallel |= RunBakeRowBands(rects,
            [&regions, &rowFn](std::size_t i, int32 minX, int32 maxX, int32 z0, int32 z1) {
                rowFn(*regions[i].Tile, minX, maxX, z0, z1);
            });
    };

    // Pass 1: region-rebake heights on tiles intersecting the height dirty rect.
    // Reset the region to the base, then apply ALL modifiers clamped to it —
    // untouched samples keep their previous bake, which matches a full bake
    // everywhere because the base + unchanged modifiers are deterministic.
    double heightPassMs = 0.0; // GE_TERRAIN_BAKE_TIMING attribution
    if (heightDirty.Any)
    {
        GE_CPU_PROFILE_SCOPE("Terrain.RegionHeight");
        const auto tHeight0 = std::chrono::steady_clock::now();
        // Collect the tiles the height dirty rect intersects. GPU-eval-skip tiles are
        // recorded here and excluded from the CPU per-texel pass below.
        std::vector<BakeRegionRect> heightRegions;
        for (auto& [coord, tilePtr] : tiled.Tiles)
        {
            if (!tilePtr || tilePtr->LodState != TileLodState::Full) continue;
            int32 minX, minZ, maxX, maxZ;
            if (!tileSampleRect(*tilePtr, heightDirty, 1, minX, minZ, maxX, maxZ))
                continue;
            if (gpuEvalSkip)
            {
                // GPU produces the height for this region into the atlas; the CPU heightfield
                // stays stale until the settle readback. Record + skip the per-texel eval.
                RecordGpuBakeTile(tiled, *tilePtr, coord, tileSize, minX, minZ, maxX, maxZ);
                outBakedTiles.push_back(coord);
                continue;
            }
            heightRegions.push_back(BakeRegionRect{tilePtr.get(), coord, minX, minZ, maxX, maxZ});
        }

        // Reset each region to the base then re-apply the modifier stack. Fanned
        // across workers for a wide edit; whole-and-serial for a dab.
        runRegionBands(heightRegions,
            [&](TerrainTileData& tp, int32 minX, int32 maxX, int32 z0, int32 z1) {
                ComposeTileHeights(tp.Heightfield, tiled, tp.WorldOriginX, tp.WorldOriginZ,
                                   heightScale, terrainOriginY, modifiers, minX, z0, maxX, z1);
            });

        // Per-tile range refresh + dirty bookkeeping: cheap, and they touch shared
        // state (the quadtree dirty list, the baked-tiles signal), so keep them
        // serial after the parallel height pass has produced the final samples.
        for (const auto& r : heightRegions)
        {
            // A region height edit can move this tile's min/max in either direction,
            // so refresh its cached range. Incremental: only the block-grid cells the
            // edited rect overlaps are rescanned (O(region)), not the whole tile —
            // byte-identical to a full scan but off the per-dab O(tile-samples) hitch.
            RefreshTileHeightRangeRegion(*r.Tile, r.MinX, r.MinZ, r.MaxX, r.MaxZ);
            r.Tile->MarkRegionDirty(r.MinX, r.MinZ, r.MaxX + 1, r.MaxZ + 1);
            outBakedTiles.push_back(r.Coord);
            // The tile's heights moved, so its global-quadtree node min/max are
            // stale. Record it for an incremental patch instead of forcing a full
            // rebuild of every resident tile (FinalizeGlobalQuadtreeUpdates).
            tiled.DirtyQuadtreeTiles.push_back(r.Coord);
        }
        heightPassMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tHeight0).count();
    }

    // GPU eval-skip: heights are stale on the CPU until the settle readback, so the splat
    // (height-range + slope) and the quadtree/global-range bookkeeping cannot run yet — the
    // readback-adopt reschedules them exactly like a CPU bake would (via MarkRegionDirty +
    // SplatResplatPending). Skip the rest of the CPU bake here.
    if (gpuEvalSkip)
        return;

    // Height-based splat layers depend on the GLOBAL range across all tiles; a
    // region splat regen is exact only while that range is unchanged. Only a
    // height edit can move the range, so rescan solely for those (matching the
    // single-terrain path); a paint-only edit leaves heights byte-identical, so
    // the cached range is exactly what a rescan would produce — reuse it and
    // skip the full-terrain sample scan.
    float32 globalMinH = tiled.SplatBakeMinH;
    float32 globalMaxH = tiled.SplatBakeMaxH;
    // On the FIRST bake the committed range is not valid yet, so SplatBakeMin/MaxH are the
    // uninitialized [0,0] sentinel. That range IS the domain every HeightNormalized rule
    // condition normalizes against, and a degenerate one is not merely imprecise: the
    // heightRange clamp floors it at 0.001, so every texel above the terrain's minimum
    // reads as a normalized height far past 1.0 and any altitude-capped row (the default
    // snow row) claims the whole terrain. Recompute the real global range whenever the
    // committed one can't be trusted (first bake) or a height edit moved it.
    if (heightDirty.Any || !tiled.SplatBakeRangeValid)
        ComputeResidentGlobalHeightRange(tiled, globalMinH, globalMaxH);

    const bool rangeStable = tiled.SplatBakeRangeValid &&
                             tiled.SplatBakeMinH == globalMinH &&
                             tiled.SplatBakeMaxH == globalMaxH;

    // Region-splat the tiles intersecting the splat dirty rect, normalized against
    // (minH, maxH). Shared by the stable-range path and the deferred stroke path.
    double splatPassMs = 0.0; // GE_TERRAIN_BAKE_TIMING attribution
    auto regionSplat = [&](float32 minH, float32 maxH) {
        if (!splatDirty.Any)
            return;
        GE_CPU_PROFILE_SCOPE("Terrain.RegionSplat");
        const auto tSplat0 = std::chrono::steady_clock::now();
        std::vector<BakeRegionRect> splatRegions;
        for (auto& [coord, tilePtr] : tiled.Tiles)
        {
            if (!tilePtr || tilePtr->LodState != TileLodState::Full) continue;
            const uint32 w = tilePtr->Heightfield.GetWidth();
            if (tilePtr->SplatmapWidth != w || tilePtr->Splatmap.empty())
                continue; // never splat-baked; the full bake owns first generation
            int32 minX, minZ, maxX, maxZ;
            if (!tileSampleRect(*tilePtr, splatDirty, 2, minX, minZ, maxX, maxZ))
                continue;
            splatRegions.push_back(BakeRegionRect{tilePtr.get(), coord, minX, minZ, maxX, maxZ});
        }
        // The rules' slope term reads the (already-final) heights of the adjacent band, so
        // splitting the splat re-bake into row bands is byte-identical to one call.
        runRegionBands(splatRegions,
            [&](TerrainTileData& tp, int32 minX, int32 maxX, int32 z0, int32 z1) {
                ResetSplatmapRegion(
                    tp.Heightfield, tp.Splatmap,
                    minX, z0, maxX, z1);
                ApplySplatModifiers(tp.Splatmap, tp.SplatmapWidth, tp.SplatmapHeight,
                                    tp.Heightfield, tileSize, tileSize,
                                    tp.WorldOriginX, tp.WorldOriginZ,
                                    heightScale, minH, maxH,
                                    modifiers, minX, z0, maxX, z1);
            });
        for (const auto& r : splatRegions)
        {
            r.Tile->SplatmapDirty = true;
            outBakedTiles.push_back(r.Coord);
        }
        splatPassMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tSplat0).count();
    };

    if (rangeStable)
    {
        // Committed range unchanged: region splat on the touched tiles is bit-exact.
        regionSplat(globalMinH, globalMaxH);
    }
    else if (tiled.SplatBakeRangeValid && !DeferSplatResplatEnabled())
    {
        // Kill switch (GE_TERRAIN_DEFER_RESPLAT=0): pre-#526 behavior — the range
        // shifted, so renormalize every resident tile against the new range right
        // now. Correct but O(all tiles) per dab; used only to A/B the deferral.
        RenormalizeAllTileSplats(tiled, tileSize, modifiers, globalMinH, globalMaxH, outBakedTiles);
    }
    else if (tiled.SplatBakeRangeValid)
    {
        // The global range shifted DURING an active stroke (a raise/lower dab moved
        // the resident min/max). Renormalizing every resident tile here would cost
        // O(all tiles) procedural-splat regen PER DAB — the dominant brush-stroke
        // hitch (~150 ms/tile in Debug). Defer it: region-splat only the touched
        // tiles against the STILL-COMMITTED range so they track their new heights and
        // stay consistent with the not-yet-renormalized untouched tiles, and flag the
        // terrain. FlushDeferredSplatResplat runs the one full renormalize against the
        // FINAL range once the stroke settles -> settled splat == full bake.
        regionSplat(tiled.SplatBakeMinH, tiled.SplatBakeMaxH);
        tiled.SplatResplatPending = true;
        tiled.SplatResplatIdleFrames = 0;
        m_AnyResplatPending = true;
    }
    else
    {
        // First bake / no committed range yet: full renormalize now (one-time cost,
        // not on the edit hot path).
        RenormalizeAllTileSplats(tiled, tileSize, modifiers, globalMinH, globalMaxH, outBakedTiles);
    }

    // Only a height edit moves the global range, the quadtree node min/max, or
    // the collider heights — a paint-only edit touches none of these, so it must
    // not bump Revision / QuadtreeDirty / GlobalHeightRangeDirty (which would
    // force a needless global quadtree rebuild each paint-drag frame). Mirrors
    // the single-terrain path, which rebuilds the quadtree only for heights.
    if (heightDirty.Any)
    {
        tiled.CachedGlobalMinH = globalMinH;
        tiled.CachedGlobalMaxH = globalMaxH;
        ++tiled.Revision;
        // Default: patch only the edited tiles (recorded above) into the global
        // quadtree. The full O(all-samples) rebuild is reserved for structural
        // changes (tile load/unload, first bake) and the kill-switch path.
        if (!IncrementalQuadtreeEnabled())
            tiled.QuadtreeDirty = true;
        tiled.GlobalHeightRangeDirty = true;
    }

    // De-duplicate tiles that were both height- and splat-baked (one line per
    // tile in the runtime signal).
    std::sort(outBakedTiles.begin(), outBakedTiles.end());
    outBakedTiles.erase(std::unique(outBakedTiles.begin(), outBakedTiles.end()),
                        outBakedTiles.end());

    if (BakeTimingEnabled() && (heightPassMs > 0.0 || splatPassMs > 0.0))
        Logger::Log::Info("Terrain.BakeTiming region heightMs={} splatMs={} tiles={} parallel={} "
                          "pool={} worker={}",
                          heightPassMs, splatPassMs, outBakedTiles.size(),
                          bakeWasParallel ? 1 : 0, m_JobPool != nullptr ? 1 : 0,
                          m_JobPool ? static_cast<int64>(m_JobPool->GetCurrentWorkerId()) : -1);
}

void TerrainModifierSystem::BakeTiledStreamedTiles(TiledTerrainData& tiled, float32 heightScale,
                                                   float32 terrainOriginY,
                                                   const std::vector<ResolvedModifier>& modifiers,
                                                   std::vector<TileCoord>& outBakedTiles)
{
    const float32 tileSize = tiled.Config.TileWorldSize;

    // Does any modifier's world AABB overlap this tile's footprint? A tile that no
    // modifier touches needs NO bake — its streamed base + job splat are already
    // final. Marking it applied without touching data is the zero-work path that
    // keeps modifier-free (and far-from-edit) streaming cheap: no whole-tile base
    // re-fill, no version bump, no re-upload.
    auto anyModifierIntersects = [&](const TerrainTileData& t, bool heightWritersOnly) -> bool {
        const float32 x0 = t.WorldOriginX, x1 = t.WorldOriginX + tileSize;
        const float32 z0 = t.WorldOriginZ, z1 = t.WorldOriginZ + tileSize;
        for (const auto& mod : modifiers)
            if ((!heightWritersOnly || !mod.IsSplatOnlyModifier()) &&
                mod.BoundsMaxX >= x0 && mod.BoundsMinX <= x1 &&
                mod.BoundsMaxZ >= z0 && mod.BoundsMinZ <= z1)
                return true;
        return false;
    };

    // RETAINED PENDING ANALYSIS — the force-bake below is provably value-identical for the
    // tiles it currently forces, and is kept only because the seam it was added for (round-6 #3)
    // has not been re-derived under the rules bake.
    //
    // What it was: streaming jobs used to bake an arrival's splat against a CachedGlobal range
    // snapshotted at job-submit, so an edit that widened the committed range after that snapshot
    // left the arrival on a stale range — a permanent tile-boundary seam against its resplatted
    // neighbour. Jobs no longer capture a range at all: they size the splat and leave it unbaked,
    // so that specific staleness cannot occur.
    //
    // Why it is still a no-op rather than wrong: this predicate only ADDS tiles that
    // anyModifierIntersects already rejected. No modifier reaches them, so their bake resets to
    // zero and composites nothing — the same bytes they already hold. The cost is a wasted bake
    // (plus a height re-fill and version bump), not a wrong result.
    //
    // What still needs deriving before it can go: the committed range is residency-dependent, and
    // it is the domain every HeightNormalized row normalizes against. Two tiles evaluated against
    // different committed ranges resolve identical border heights to different row weights — the
    // same seam CLASS, reached through the rules rather than through a captured snapshot. Removing
    // this needs that case measured, not assumed absent.
    const bool committedRangeOffBand =
        tiled.SplatBakeRangeValid &&
        (tiled.SplatBakeMinH < 0.0f || tiled.SplatBakeMaxH > kTileNoiseAmplitude);

    // COARSE tiles: composite the rules over the coarse splat at coarse resolution, once.
    //
    // Every full-detail splat path below gates on LodState == Full, and material now comes
    // only from authored rows — so a coarse tile left unbaked renders the unbaked base
    // (channel 0) across the whole streaming edge and then POPS to the ruled surface on
    // upgrade. That is a visible distance artifact, so the rules run here too, on the tile's
    // own coarse grid: the rule sample reads slope and height off the coarse heightfield,
    // which is exactly what the coarse LOD is showing.
    //
    // Cheap by construction — coarse resolution, once per arrival (CoarseSplatBaked), and
    // ONLY while the tile is coarse. ModifiersApplied is deliberately NOT set: the full bake
    // must still run when this tile upgrades.
    //
    // Deferred until the committed range is VALID: that range is the domain every
    // HeightNormalized row normalizes against, and the [0,0] sentinel would collapse it
    // onto the 0.001 heightRange clamp and hand the whole tile to any altitude-capped row.
    // A tile that arrives before the first bake commits a range simply bakes on a later tick.
    for (auto& [coarseCoord, coarsePtr] : tiled.Tiles)
    {
        if (!tiled.SplatBakeRangeValid)
            break;
        if (!coarsePtr || coarsePtr->LodState != TileLodState::Coarse || coarsePtr->CoarseSplatBaked)
            continue;
        const uint32 cw = coarsePtr->Heightfield.GetWidth();
        const uint32 ch = coarsePtr->Heightfield.GetHeight();
        if (cw < 2 || ch < 2)
            continue;
        ResetSplatmap(coarsePtr->Heightfield, coarsePtr->Splatmap,
                      coarsePtr->SplatmapWidth, coarsePtr->SplatmapHeight);
        ApplySplatModifiers(coarsePtr->Splatmap, coarsePtr->SplatmapWidth, coarsePtr->SplatmapHeight,
                            coarsePtr->Heightfield, tileSize, tileSize,
                            coarsePtr->WorldOriginX, coarsePtr->WorldOriginZ,
                            heightScale, tiled.SplatBakeMinH, tiled.SplatBakeMaxH,
                            modifiers, 0, 0, kRegionUnbounded, kRegionUnbounded);
        coarsePtr->SplatmapDirty = true;
        coarsePtr->CoarseSplatBaked = true;
    }

    // Collect streamed-in tiles (Full, modifiers not yet applied). Tiles no modifier touches
    // (and on-band terrains) are marked applied here and cost nothing further.
    std::vector<TileCoord> bakeTiles;
    for (auto& [coord, tilePtr] : tiled.Tiles)
    {
        if (!tilePtr || tilePtr->LodState != TileLodState::Full || tilePtr->ModifiersApplied)
            continue;
        if (anyModifierIntersects(*tilePtr, false) || committedRangeOffBand)
            bakeTiles.push_back(coord);
        else
            // No modifier reaches this tile, so the UNBAKED base is already its final splat:
            // material comes only from authored rows, and none of them cover it. Marking it
            // applied costs nothing and skips a bake that would write the same zeros back.
            // (A Shape::Global rules volume has infinite bounds, so on a terrain carrying the
            // default rows this branch does not fire — every tile intersects and bakes.)
            tilePtr->ModifiersApplied = true;
    }
    if (bakeTiles.empty())
        return;

    // Pass 1: the base + height modifiers on the arrivals a HEIGHT-writing modifier reaches,
    // then refresh each one's cached range. Re-filling the deterministic base makes this
    // bit-identical to how BakeTiledFull treats it. An arrival only splat writers reach (the
    // scene's global surface rules reach every tile) still holds exactly that base, as the
    // streaming job filled it with the same FillTiledBaseRegion, so its heights, their cached
    // range and their dirty state are left alone: a re-fill would rewrite the same samples and
    // re-trigger the height upload, the normal regen and the quadtree sync for nothing.
    std::vector<uint8> heightsComposed(bakeTiles.size(), 0u);
    bool anyHeightsComposed = false;
    for (std::size_t i = 0; i < bakeTiles.size(); ++i)
    {
        auto* t = tiled.Tiles[bakeTiles[i]].get();
        if (!anyModifierIntersects(*t, true))
            continue;
        ComposeTileHeights(t->Heightfield, tiled, t->WorldOriginX, t->WorldOriginZ,
                           heightScale, terrainOriginY, modifiers,
                           0, 0, kRegionUnbounded, kRegionUnbounded);
        RefreshTileHeightRange(*t);
        heightsComposed[i] = 1u;
        anyHeightsComposed = true;
    }

    // Splat over the CURRENT global range across all resident tiles (O(tiles)
    // over cached per-tile min/max — matches BakeTiledFull's normalization when
    // the other tiles' cached ranges are current, which they are). Other tiles
    // keep their existing splat here: a stream-in must not regen or version-bump
    // any tile but the ones that arrived (the E6-review#3 churn contract). When the
    // arrivals push the aggregate past the committed splat range, the already-
    // resident tiles are left at the OLD range while the new tile normalizes against
    // the wider one — a hard normalization seam at the new tile's border. That is
    // reconciled below by scheduling the deferred whole-terrain renormalize, so the
    // settle brings every resident tile onto the one final range (it must not depend
    // on a later authored edit, which under heavy atlas eviction may never come —
    // the persistent snow/grass tile-boundary seam).
    float32 globalMinH, globalMaxH;
    ComputeResidentGlobalHeightRange(tiled, globalMinH, globalMaxH);

    // The arrivals' splat runs in row bands across the job pool (RunBakeRowBands): a whole
    // tile is a million texels, and the rules' slope term reads only the arrival's own heights,
    // final by now, so the bands are byte-identical to one call per tile.
    // The tiles are resolved here, on the calling thread: the bands only index this vector.
    std::vector<TerrainTileData*> splatTiles;
    std::vector<BakeBandRegion> splatRects;
    splatTiles.reserve(bakeTiles.size());
    splatRects.reserve(bakeTiles.size());
    for (const TileCoord coord : bakeTiles)
    {
        auto* t = tiled.Tiles[coord].get();
        splatTiles.push_back(t);
        ResetSplatmap(t->Heightfield, t->Splatmap, t->SplatmapWidth, t->SplatmapHeight);
        splatRects.push_back(BakeBandRegion{0, 0, static_cast<int32>(t->SplatmapWidth) - 1,
                                            static_cast<int32>(t->SplatmapHeight) - 1});
    }
    RunBakeRowBands(splatRects, [&](std::size_t i, int32 minX, int32 maxX, int32 z0, int32 z1) {
        TerrainTileData* t = splatTiles[i];
        ApplySplatModifiers(t->Splatmap, t->SplatmapWidth, t->SplatmapHeight,
                            t->Heightfield, tileSize, tileSize,
                            t->WorldOriginX, t->WorldOriginZ,
                            heightScale, globalMinH, globalMaxH,
                            modifiers, minX, z0, maxX, z1);
    });

    static const bool kTileStreamDebug = std::getenv("GE_TERRAIN_TILE_DEBUG") != nullptr;
    for (std::size_t i = 0; i < bakeTiles.size(); ++i)
    {
        const TileCoord coord = bakeTiles[i];
        auto* t = tiled.Tiles[coord].get();
        if (heightsComposed[i] != 0u)
            t->MarkFullDirty();
        t->SplatmapDirty = true;
        t->ModifiersApplied = true;
        outBakedTiles.push_back(coord);

        if (kTileStreamDebug)
            Logger::Log::Info("Terrain.TileStream in tile=({},{}) bake=region", coord.X, coord.Z);
    }

    // An edited streamed tile can extend the global height range; keep the
    // quadtree/global aggregate in sync WITHOUT re-baking any existing tile (the
    // same bookkeeping the streaming integration does on load). Revision tracks
    // tile-content changes, so bump it here too (RevisionTracksTileMutations).
    tiled.CachedGlobalMinH = std::min(tiled.CachedGlobalMinH, globalMinH);
    tiled.CachedGlobalMaxH = std::max(tiled.CachedGlobalMaxH, globalMaxH);
    if (anyHeightsComposed)
        tiled.QuadtreeDirty = true;
    ++tiled.Revision;

    // The arrivals extended the aggregate beyond the range every already-resident
    // tile was splat-normalized against (SplatBakeMinH/MaxH). Schedule the deferred
    // whole-terrain renormalize so the settle re-normalizes ALL resident tiles onto
    // the one final range (== a full bake) and the tile-boundary seam heals without
    // waiting for the next authored edit. No SplatResplatIdleFrames reset: the
    // settle countdown stays churn-robust so continuous streaming can't starve it.
    if (tiled.SplatBakeRangeValid &&
        (globalMinH < tiled.SplatBakeMinH || globalMaxH > tiled.SplatBakeMaxH))
    {
        tiled.SplatResplatPending = true;
        m_AnyResplatPending = true;
    }
}

void TerrainModifierSystem::FlushDeferredSplatResplat(ECS::World& world, TerrainService& terrainService)
{
    // A stroke defers its whole-terrain splat renormalize (SplatResplatPending) to keep
    // editing real-time. Settle it after a couple of quiet frames so a momentary
    // mid-stroke pause doesn't trigger a premature full re-splat the next dab re-defers.
    constexpr uint32 kSplatResplatSettleFrames = 2;
    const bool spreadSettle = SpreadSettleEnabled();

    // The retained stack, not a local. This flush runs ahead of — and independently
    // of — the change gate, so a gather into a local here would clear the arenas
    // and then return through a closed gate that never refills m_AppliedModifiers.
    const std::vector<ResolvedModifier>& modifiers = m_AppliedModifiers;
    bool gatheredModifiers = false;
    bool stillPending = false;

    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle /*entity*/, const Components::Terrain& terrain,
                  const Components::WorldTransform& worldXf)
        {
            if (terrain.TiledTerrainHandle == 0 && terrain.TiledTerrainGeneration == 0)
            {
                // ---- Single terrain settle ----
                TerrainHandle handle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration};
                auto* data = terrainService.GetTerrainData(handle);
                if (!data || !data->SplatResplatPending)
                    return;

                if (++data->SplatResplatIdleFrames < kSplatResplatSettleFrames)
                {
                    stillPending = true; // wait one more quiet frame
                    return;
                }

                // Full re-bake against the FINAL heights: ResetSplatmapAndCommitRange
                // re-caches the exact range and marks the whole splat for upload, then the
                // rules and paint modifiers composite on top — byte-identical to a fresh
                // full bake.
                if (!gatheredModifiers)
                {
                    GatherModifiers(world, terrainService);
                    gatheredModifiers = true;
                }
                const float32 originX = worldXf.matrix[12] - terrain.SizeX * 0.5f;
                const float32 originZ = worldXf.matrix[14] - terrain.SizeZ * 0.5f;
                data->ResetSplatmapAndCommitRange();
                ApplySplatModifiers(data->Splatmap, data->SplatmapWidth, data->SplatmapHeight,
                                    data->Heightfield,
                                    data->Config.WorldSizeX, data->Config.WorldSizeZ,
                                    originX, originZ, data->Config.HeightScale,
                                    data->SplatBakeMinH, data->SplatBakeMaxH, modifiers,
                                    0, 0, kRegionUnbounded, kRegionUnbounded);
                data->SplatmapDirty = true;
                data->SplatResplatPending = false;
                data->SplatResplatIdleFrames = 0;
                return;
            }
            TiledTerrainHandle tiledHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration};
            auto* tiled = terrainService.GetTiledTerrainData(tiledHandle);
            if (!tiled || !tiled->SplatResplatPending)
                return;

            if (++tiled->SplatResplatIdleFrames < kSplatResplatSettleFrames)
            {
                // A fresh range-shifting dab restarted the countdown. Abort any spread
                // in progress so it re-freezes against the FINAL range once quiet again
                // (a spread half-done against a now-stale range would not settle to a
                // full bake). Some tiles keep the old frozen range mid-stroke — an
                // acceptable splat preview lag, healed by the completed spread at settle.
                tiled->SplatRenormalizeActive = false;
                stillPending = true; // wait one more quiet frame
                return;
            }

            if (!gatheredModifiers)
            {
                GatherModifiers(world, terrainService);
                gatheredModifiers = true;
            }

            if (!spreadSettle)
            {
                // Kill switch (GE_TERRAIN_SPREAD_SETTLE=0): one-shot flush — renormalize
                // EVERY resident tile against the final range in this single frame (the
                // pre-fix ~1.4s stroke-release spike; kept only to A/B the spread).
                float32 globalMinH = tiled->SplatBakeMinH;
                float32 globalMaxH = tiled->SplatBakeMaxH;
                ComputeResidentGlobalHeightRange(*tiled, globalMinH, globalMaxH);
                std::vector<TileCoord> baked;
                RenormalizeAllTileSplats(*tiled, tiled->Config.TileWorldSize, modifiers,
                                         globalMinH, globalMaxH, baked);
                tiled->SplatResplatPending = false;
                tiled->SplatResplatIdleFrames = 0;
                tiled->SplatRenormalizeActive = false;
                return;
            }

            GE_CPU_PROFILE_SCOPE("Terrain.SplatRenormalize");

            // First settled frame of this spread: snapshot the resident-Full tiles and
            // FREEZE the final global range, so every row band across every frame
            // normalizes against one range — the fully-spread result is byte-identical
            // to a one-shot RenormalizeAllTileSplats (each texel visited exactly once).
            if (!tiled->SplatRenormalizeActive)
            {
                float32 globalMinH = tiled->SplatBakeMinH;
                float32 globalMaxH = tiled->SplatBakeMaxH;
                ComputeResidentGlobalHeightRange(*tiled, globalMinH, globalMaxH);
                tiled->SplatRenormalizeMinH = globalMinH;
                tiled->SplatRenormalizeMaxH = globalMaxH;
                tiled->SplatRenormalizeQueue.clear();
                // Every resident Full tile with a valid heightfield — matching the one-shot
                // RenormalizeAllTileSplats, which also sizes+generates the splat for any Full
                // tile (a stream-in that arrived without a cooked splat included).
                for (auto& [coord, tilePtr] : tiled->Tiles)
                    if (tilePtr && tilePtr->LodState == TileLodState::Full &&
                        tilePtr->Heightfield.GetWidth() >= 2 && tilePtr->Heightfield.GetHeight() >= 2)
                        tiled->SplatRenormalizeQueue.push_back(coord);
                std::sort(tiled->SplatRenormalizeQueue.begin(), tiled->SplatRenormalizeQueue.end());
                tiled->SplatRenormalizeTileCursor = 0;
                tiled->SplatRenormalizeRowCursor = 0;
                tiled->SplatRenormalizeActive = true;
            }

            // Renormalize row bands until this frame's texel budget is spent. A tile is
            // marked SplatmapDirty (-> one whole-tile upload) only once its LAST row is
            // done, so the GPU never sees a half-renormalized tile.
            const float32 tileSize = tiled->Config.TileWorldSize;
            std::size_t texelsThisFrame = 0;
            while (texelsThisFrame < kSplatRenormalizeTexelsPerFrame &&
                   tiled->SplatRenormalizeTileCursor < tiled->SplatRenormalizeQueue.size())
            {
                const TileCoord coord = tiled->SplatRenormalizeQueue[tiled->SplatRenormalizeTileCursor];
                auto it = tiled->Tiles.find(coord);
                if (it == tiled->Tiles.end() || !it->second ||
                    it->second->LodState != TileLodState::Full)
                {
                    // Evicted / changed LOD since the snapshot: a later authored edit or
                    // stream-in re-bakes it. Skip to the next queued tile.
                    ++tiled->SplatRenormalizeTileCursor;
                    tiled->SplatRenormalizeRowCursor = 0;
                    continue;
                }
                TerrainTileData& tp = *it->second;
                const int32 rows = static_cast<int32>(tp.Heightfield.GetHeight());
                const int32 width = static_cast<int32>(tp.Heightfield.GetWidth());
                if (rows < 2 || width < 2)
                {
                    ++tiled->SplatRenormalizeTileCursor;
                    tiled->SplatRenormalizeRowCursor = 0;
                    continue;
                }
                const std::size_t budgetLeft = kSplatRenormalizeTexelsPerFrame - texelsThisFrame;
                const int32 rowsThisStep = std::max<int32>(
                    1, static_cast<int32>(budgetLeft / static_cast<std::size_t>(width)));
                const int32 rowStart = tiled->SplatRenormalizeRowCursor;
                const int32 rowEnd = std::min(rows - 1, rowStart + rowsThisStep - 1);
                texelsThisFrame += RenormalizeTileSplatRows(tp, tileSize,
                                                            tiled->Config.HeightScale, modifiers,
                                                            tiled->SplatRenormalizeMinH,
                                                            tiled->SplatRenormalizeMaxH,
                                                            rowStart, rowEnd);
                if (rowEnd >= rows - 1)
                {
                    tp.SplatmapDirty = true; // tile complete: one whole-tile re-upload
                    ++tiled->SplatRenormalizeTileCursor;
                    tiled->SplatRenormalizeRowCursor = 0;
                }
                else
                {
                    tiled->SplatRenormalizeRowCursor = rowEnd + 1;
                }
            }

            if (tiled->SplatRenormalizeTileCursor >= tiled->SplatRenormalizeQueue.size())
            {
                // Spread complete: commit the frozen range (mid-stroke region splats
                // read SplatBakeMinH/MaxH, so it must move only once every tile is on the
                // new range) and clear the deferral.
                tiled->SplatBakeMinH = tiled->SplatRenormalizeMinH;
                tiled->SplatBakeMaxH = tiled->SplatRenormalizeMaxH;
                tiled->SplatBakeRangeValid = true;
                tiled->SplatResplatPending = false;
                tiled->SplatResplatIdleFrames = 0;
                tiled->SplatRenormalizeActive = false;
                tiled->SplatRenormalizeQueue.clear();
            }
            else
            {
                stillPending = true; // more row bands to spread next frame
            }
        });

    m_AnyResplatPending = stillPending;
}


void TerrainModifierSystem::BakeSphereModifiers(TerrainService& terrainService, float32 planetRadius,
                                                const Components::TerrainPlanetRelief& relief,
                                                const std::vector<ResolvedModifier>& modifiers,
                                                const std::vector<uint64>& modifierGeometryHashes,
                                                bool fullBake)
{
    // Size the sculpt page store's virtual resolution from the planet radius before any write, so
    // the modifier bake (and every reader) iterate the same radius-scaled dim. Idempotent + frozen
    // after the first edit — cheap to call each bake.
    terrainService.ConfigurePlanetSculpt(planetRadius);
    const uint32 virtualDim = terrainService.GetPlanetSculptGeometry().VirtualDim;

    // Resolve every sphere-supported modifier onto the sphere (sorted by priority already), and
    // flag any unsupported type for the one-shot honest warning. With the analytic flag on (S2),
    // closed-form-primitive modifiers — circular flattens today — are PARTITIONED OUT of the store
    // bake and published as analytic placements instead: the store's modifier layer then never
    // contains them (no double-apply by construction), and the sampling chokepoints evaluate the
    // exact shape at any radius. Rect flattens and every payload-carrying type (noise, stamp,
    // sculpt zone) stay store-baked. Overflow beyond the bounded set falls back to the bake
    // (shape quality degrades to the store ceiling for the excess — warned once, never dropped).
    const bool analyticOn = AnalyticModifiersEnabled();
    std::vector<SphereModifierPlacement> placements;
    placements.reserve(modifiers.size());
    std::array<CBTTerrain::SphereAnalyticFlatten, CBTTerrain::kMaxSphereAnalyticModifiers>
        analytic{};
    uint32 analyticCount = 0u;
    bool anyUnsupported = false;
    bool analyticOverflow = false;
    for (const auto& mod : modifiers)
    {
        // Pooling has no meaning on a planet: the pool accumulates across its
        // members over a planar bake region and applies once, and the sphere
        // bakes cube-face rects with no such region. Named here because this is
        // the one place that has the entity, and because an author who reaches
        // for Average on a planet needs to be told which operator to use instead.
        if (HasPooledEffect(mod) && m_SpherePooledEffectWarned.insert(mod.Entity.id).second)
        {
            const char* kindName = "effect";
            for (const auto& fx : mod.Effects)
                if (IsPooled(fx))
                {
                    kindName = EffectKindName(fx.EffectKind);
                    break;
                }
            Logger::Log::Warning(
                "TerrainModifierSystem: entity {} has a {} effect whose blend is Average on a "
                "spherical terrain. Pooled averaging is planar-only — a planet has no bake region "
                "for a pool to accumulate into — so the effect is ignored there. Use Set, Min or "
                "Max on a planet (Add for a height offset, noise or stamp).",
                mod.Entity.id, kindName);
        }

        if (!IsSphereSupportedModifier(mod))
        {
            anyUnsupported = true; // paint / spline type, or a spline or global shape: no sphere path yet
            continue;
        }
        // A volume contributes one placement per sphere-bakeable effect; every
        // other modifier contributes exactly one.
        std::vector<SphereModifierPlacement> modPlacements;
        AppendSpherePlacements(mod, planetRadius, modPlacements, anyUnsupported);
        for (SphereModifierPlacement& p : modPlacements)
        {
            // The analytic path is a closed-form circular flatten with the plain
            // shape falloff; an inward feather or a master weight has no analytic
            // twin, so those bake through the store instead. As with the GPU
            // height bake, the closed form carries only ShapeFalloffWeight's
            // outward-only branch, so refusing the inward feather here is what
            // keeps the analytic eval and the store bake the same function.
            const bool analyticEligible = analyticOn &&
                                          p.Type == SpherePlacementKind::Flatten &&
                                          p.Shape == Components::TerrainModifierShape::Circle &&
                                          p.FalloffInward <= 0.0f && p.MasterWeight == 1.0f;
            if (analyticEligible && analyticCount < CBTTerrain::kMaxSphereAnalyticModifiers)
            {
                // Copy the EXACT placement frame (N/E1/E2 from MakeSphereFrame) so the analytic
                // eval and a store bake of the same modifier are the same closed form to the bit.
                CBTTerrain::SphereAnalyticFlatten& f = analytic[analyticCount++];
                f.N = p.N;
                f.E1 = p.E1;
                f.E2 = p.E2;
                f.Radius = p.Radius;
                f.Falloff = p.Falloff;
                f.TargetRadius = p.TargetRadius;
                f.PlanetRadius = p.PlanetRadius;
                f.Valid = true;
                continue; // published analytically — excluded from the store bake below
            }
            if (analyticEligible)
                analyticOverflow = true; // over the bounded set — falls back to the store bake
            placements.push_back(std::move(p));
        }
    }

    // Publish the placement set (empty when the flag is off or no eligible modifier exists — the
    // dark-ship). An unchanged publish is a no-op; a changed one advances the combined sculpt
    // version so upload / Classify / physics gates re-arm even before the region bake below runs.
    terrainService.SetPlanetAnalyticModifiers(analytic.data(), analyticCount);

    if (anyUnsupported && !m_SphereUnsupportedWarned)
    {
        m_SphereUnsupportedWarned = true;
        Logger::Log::Warning(
            "TerrainModifierSystem: a spherical terrain carries modifiers not yet baked onto "
            "planets (Noise, Flatten, Stamp and Sculpt zones bake with a circle or rectangle "
            "shape; Paint and Spline modifiers, and any volume with Shape=Spline Path/Area or "
            "Shape=Global, do not yet); those modifiers are ignored on the sphere.");
    }
    if (analyticOverflow && !m_SphereAnalyticOverflowWarned)
    {
        m_SphereAnalyticOverflowWarned = true;
        Logger::Log::Warning(
            "TerrainModifierSystem: more than {} analytic-eligible flatten modifiers on the "
            "planet; the excess bakes into the sculpt store instead (shape accuracy limited by "
            "the store's texel budget there).",
            CBTTerrain::kMaxSphereAnalyticModifiers);
    }

    // The dirty (face, UV rect) regions to re-derive. A full bake re-derives every face; a region
    // bake unions each changed modifier's old ∪ new footprint (the sphere analogue of the planar
    // old∪new bounds diff), classifying a world footprint to cube-face rects exactly like a #488 dab.
    CBTTerrain::SphereEditRegions dirty;
    if (fullBake)
    {
        // Bound the full re-bake to the CURRENT modifiers' footprints; the clear-all inside
        // BakePlanetModifierRegions(fullBake=true) resets any removed/moved modifier's vacated
        // pages. Iterating whole faces at a radius-scaled virtual dim would be tens of millions of
        // texels — a whole-face region set is a scale trap on a large planet.
        for (const auto& mod : modifiers)
        {
            if (!IsSphereSupportedModifier(mod))
                continue;
            UnionSphereRegions(
                dirty, ModifierFaceRegions(mod.Position, ModifierFootprintRadius(mod), planetRadius,
                                           virtualDim));
        }
    }
    else
    {
        std::vector<bool> matched(m_LastBakeSnapshot.size(), false);
        for (size_t i = 0; i < modifiers.size(); ++i)
        {
            const auto& mod = modifiers[i];
            if (!IsSphereSupportedModifier(mod))
                continue;
            const ModifierBakeSnapshot* prev = nullptr;
            for (size_t j = 0; j < m_LastBakeSnapshot.size(); ++j)
                if (!matched[j] && m_LastBakeSnapshot[j].Entity == mod.Entity &&
                    m_LastBakeSnapshot[j].ModType == mod.ModType)
                {
                    matched[j] = true;
                    prev = &m_LastBakeSnapshot[j];
                    break;
                }
            const float32 fpR = ModifierFootprintRadius(mod);
            if (!prev)
            {
                // Added: dirty where it is.
                UnionSphereRegions(
                    dirty, ModifierFaceRegions(mod.Position, fpR, planetRadius, virtualDim));
            }
            else if (prev->GeometryHash != modifierGeometryHashes[i] ||
                     prev->PayloadVersion != mod.DataVersion)
            {
                // Moved / re-parameterized / brush-stroked: dirty where it was AND where it is.
                UnionSphereRegions(dirty, ModifierFaceRegions(prev->WorldPos, prev->FootprintRadius,
                                                              planetRadius, virtualDim));
                UnionSphereRegions(
                    dirty, ModifierFaceRegions(mod.Position, fpR, planetRadius, virtualDim));
            }
        }
        for (size_t j = 0; j < m_LastBakeSnapshot.size(); ++j)
        {
            if (matched[j] || !m_LastBakeSnapshot[j].SphereSupported)
                continue;
            // Removed: dirty where it was.
            UnionSphereRegions(dirty, ModifierFaceRegions(m_LastBakeSnapshot[j].WorldPos,
                                                          m_LastBakeSnapshot[j].FootprintRadius,
                                                          planetRadius, virtualDim));
        }
    }

    // A region bake with an empty diff is quiescent (idle planets bake nothing). A full bake still
    // runs on an empty footprint set so the clear-all resets any removed modifier's vacated pages.
    if (!fullBake && dirty.Count == 0u)
        return;

    // Flatten cancels the base relief per texel, so it needs the closed-form relief at each
    // direction; the other types don't, so skip the noise eval unless a flatten is present.
    bool anyFlatten = false;
    for (const SphereModifierPlacement& p : placements)
        if (p.Type == SpherePlacementKind::Flatten)
        {
            anyFlatten = true;
            break;
        }

    const CBTTerrain::SphereEditRegions baked = terrainService.BakePlanetModifierRegions(
        dirty,
        [&placements, &relief, anyFlatten](float32 dx, float32 dy, float32 dz) {
            const float32 reliefAtDir =
                anyFlatten ? CBTTerrain::PlanetRelief(dx, dy, dz, relief.Amplitude, relief.Frequency,
                                                      relief.Octaves)
                           : 0.0f;
            float32 offset = 0.0f;
            for (const SphereModifierPlacement& p : placements)
                offset += EvaluateSpherePlacement(p, dx, dy, dz, reliefAtDir);
            return offset;
        },
        fullBake);

    static const bool kEditDebug = std::getenv("GE_TERRAIN_ZONE_DEBUG") != nullptr;
    if (kEditDebug && baked.Count > 0u)
    {
        std::string faces;
        for (uint32 i = 0; i < baked.Count; ++i)
        {
            if (i != 0)
                faces += ',';
            faces += std::to_string(baked.Rects[i].Face);
        }
        Logger::Log::Info("Planet.ModifierBake faces=[{}] mods={} full={}", faces, placements.size(),
                          fullBake ? 1 : 0);
    }
}

TerrainModifierSystem::TerrainModifierSystem() = default;

TerrainModifierSystem::~TerrainModifierSystem()
{
    if (m_BakeStoreWrites && m_BakeStoreWrites->IsValid())
        m_BakeStoreWrites->Wait();
}

// A save's copy of each stored terrain's bake. Whichever of the store job and the next
// Update claims it first takes it; the other waits for that copy (the job before it
// writes, Update before it bakes over the data in place).
struct BakeStoreSnapshot
{
    struct Store
    {
        std::filesystem::path File;
        uint64 Key = 0;
        std::shared_ptr<const TerrainData> Data;
    };
    std::vector<Store> Stores;
    std::vector<TerrainBakeArtifact> Artifacts; // one per store, once taken
    std::atomic<bool> Claimed{false};
    std::promise<void> TakenPromise;
    std::shared_future<void> Taken = TakenPromise.get_future().share();

    // Copies every store's bake if this caller claims the copy; otherwise waits for the
    // claimant's. Returns once Artifacts is complete.
    void Take()
    {
        if (Claimed.exchange(true, std::memory_order_acq_rel))
        {
            Taken.wait();
            return;
        }
        Artifacts.reserve(Stores.size());
        for (Store& store : Stores)
        {
            Artifacts.push_back(CopyTerrainBake(*store.Data));
            store.Data.reset();
        }
        TakenPromise.set_value();
    }
};

void TerrainModifierSystem::BakeAwaitedPageOverlays(ECS::World& world, TerrainService& terrainService,
                                                    const std::vector<ResolvedModifier>& modifiers)
{
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle /*entity*/, const Components::Terrain& terrain,
                  const Components::WorldTransform& worldXf)
        {
            if (terrain.TiledTerrainHandle == 0 && terrain.TiledTerrainGeneration == 0)
                return;
            auto* tiled = terrainService.GetTiledTerrainData(
                TiledTerrainHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration});
            if (tiled && tiled->AwaitsPageOverlay())
                BakeHeightPageOverlay(*tiled, terrain.HeightScale, worldXf.matrix[13], modifiers, false, DirtyUnion{});
        });
}

void TerrainModifierSystem::TakePendingBakeStoreSnapshot()
{
    if (!m_PendingBakeStoreSnapshot)
        return;
    m_PendingBakeStoreSnapshot->Take();
    m_PendingBakeStoreSnapshot.reset();
}

void TerrainModifierSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    UpdateBakes(world);
    if (m_BakeStoreRequested)
        StoreRequestedBakes(world);
}

void TerrainModifierSystem::RequestBakeCacheStore(ECS::World& world, const GUID& previousScene, const GUID& savedScene)
{
    world.Query<ECS::Write<Components::Terrain>>().Each(
        [&](Components::Terrain& terrain)
        {
            if (terrain.BakeOriginScene.IsNull() || terrain.BakeOriginScene == previousScene)
                terrain.BakeOriginScene = savedScene;
        });
    m_BakeStoreScene = savedScene;
    m_BakeStoreRequested = true;
}

void TerrainModifierSystem::StoreRequestedBakes(ECS::World& world)
{
    auto* terrainService = TerrainService::TryGet();
    if (!terrainService)
        return;
    const TerrainBakeCacheConfig& bakeCache = terrainService->GetBakeCache();
    if (!bakeCache.Writable)
    {
        m_BakeStoreRequested = false;
        return;
    }
    // A deferred geometry bake, a pending splat renormalize or an interactive edit means
    // the in-memory result is a preview, not the full bake of the current inputs: wait
    // for the update that settles it.
    if (m_GeometryBakePending || m_AnyResplatPending || terrainService->IsInteractiveModifierEdit())
        return;
    // The previous store's writes still running: take the snapshot on a later update
    // rather than wait for them here.
    if (m_BakeStoreWrites && m_BakeStoreWrites->IsValid() && !m_BakeStoreWrites->IsDone())
        return;

    // Settled: a region bake's result equals a full bake of the same inputs whenever no
    // renormalize is pending (TerrainRegionBake.RegionBakeMatchesFullBake,
    // SingleDiscreteRangeShiftRegionBakeMatchesFullBake and
    // SingleInteractiveRangeShiftDefersThenSettlesToFullBake pin it), so the field
    // is stored as it is, under the key of the stack it was baked from.
    //
    // The update keys each terrain (the key reads the modifier stack and its payloads,
    // which the editor changes on this thread) and shares its data. The job copies the
    // bake, which is megabytes per terrain, then writes what changed and prunes; the next
    // update copies it instead when the job has not started (BakeStoreSnapshot).
    auto snapshot = std::make_shared<BakeStoreSnapshot>();
    std::vector<std::filesystem::path> live;
    terrainService->BeginModifierAccess();
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle entity, const Components::Terrain& terrain, const Components::WorldTransform& worldXf)
        {
            if (terrain.Domain != Components::TerrainDomain::Planar || terrain.TiledTerrainHandle != 0 ||
                terrain.TiledTerrainGeneration != 0 || terrain.BakeOriginScene != m_BakeStoreScene)
                return;
            const auto* sceneTag = world.GetComponent<Components::SceneEntityTag>(entity);
            const std::filesystem::path file = TerrainBakeFile(
                bakeCache.Directory, terrain.BakeOriginScene, sceneTag ? sceneTag->View() : std::string_view{});
            const TerrainHandle handle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration};
            const auto* data = terrainService->GetTerrainData(handle);
            if (file.empty() || !data || m_AppliedModifiers.empty() || data->Splatmap.empty())
                return;
            live.push_back(file);
            std::shared_ptr<const Terrain::HeightfieldData> baseHeightmap;
            if (terrain.BaseSource == Components::TerrainBaseSource::HeightmapAsset && !terrain.TerrainAssetGuid.IsNull())
                baseHeightmap = terrainService->ResolveHeightmapAsset(terrain.TerrainAssetGuid.ToGuid());
            const uint64 key = ComputeSingleTerrainBakeKey(
                m_AppliedModifiers, *data, terrain.BaseSource, baseHeightmap.get(),
                worldXf.matrix[12] - terrain.SizeX * 0.5f, worldXf.matrix[13], worldXf.matrix[14] - terrain.SizeZ * 0.5f);
            snapshot->Stores.push_back({file, key, terrainService->ShareTerrainData(handle)});
        });
    terrainService->EndModifierAccess();
    m_BakeStoreRequested = false;

    m_PendingBakeStoreSnapshot = snapshot;
    auto storeAndPrune = [snapshot, live = std::move(live), directory = bakeCache.Directory,
                          scene = m_BakeStoreScene]() {
        snapshot->Take();
        for (size_t i = 0; i < snapshot->Stores.size(); ++i)
        {
            const BakeStoreSnapshot::Store& store = snapshot->Stores[i];
            if (ReadTerrainBakeKey(store.File) == store.Key)
                continue; // already the current state: a save with no edit writes nothing
            if (!WriteTerrainBake(store.File, store.Key, snapshot->Artifacts[i]))
                Logger::Log::Warning("Terrain bake cache: could not write '{}' at save; the next open bakes again",
                                     store.File.string());
        }
        PruneTerrainBakeScene(directory, scene, live);
    };
    // A job of the process's one bake-store channel: the writes wait on the disk, so they
    // hold no compute worker, and a world's store is scheduled behind any other world's
    // (the cap schedules; it is not a lock).
    if (m_JobPool)
        m_BakeStoreWrites = std::make_unique<JobSystem::TaskHandle>(
            terrainService->BakeStoreChannel(*m_JobPool).Submit(std::move(storeAndPrune)));
    else
        storeAndPrune();
}

void TerrainModifierSystem::UpdateBakes(ECS::World& world)
{
    GE_CPU_PROFILE_SCOPE("Terrain.Modifier");
    auto* terrainService = TerrainService::TryGet();
    if (!terrainService)
        return;

    // Capture the World's pool for the large-region bake fan-out. Null in contexts
    // with no pool (headless tests) → the bake runs serially.
    m_JobPool = world.GetJobSystem();

    // Every splat pass of this update (any terrain, tile or row band) shares one
    // warning per distinct problem; the next update reports what it meets again.
    m_LayerClampWarnings.Reset();
    m_UnaddressableRulesWarnings.Reset();

    // Hot-reload lane: evict decoded mask/heightmap caches for assets whose
    // reload events were enqueued (possibly on the watcher thread) since last
    // frame, before the gate decides whether to look.
    DrainAssetInvalidations(*terrainService);

    terrainService->BeginModifierAccess();

    // A GPU-bake settle readback (slice-1c) refreshed a heightfield last frame and set the
    // terrain's SplatResplatPending — arm the deferred renormalize so it runs the SAME #526
    // path as a CPU bake (regenerate splat from the readback-refreshed heights).
    if (terrainService->TakeGpuReadbackSplatPending())
        m_AnyResplatPending = true;

    // Settle any deferred whole-terrain splat renormalize a stroke left owed. Runs
    // every frame a terrain carries the flag (rare): counts frames since the last
    // range-shifting dab and, once quiet, runs the one full renormalize against the
    // final range (settled == full bake). A dab this frame re-defers below, resetting
    // the count, so it never fires mid-stroke. Independent of the change gate, so it
    // still settles when ShouldGather stays true (e.g. spline-modifier scenes).
    if (m_AnyResplatPending)
    {
        TakePendingBakeStoreSnapshot();
        FlushDeferredSplatResplat(world, *terrainService);
    }

    // Interactive geometry-drag coalescing (see the header). `interactiveDrag` is
    // set while the editor drives a modifier/zone transform gizmo; the deferred
    // re-bake is finished by the settle bake the moment it clears.
    const bool coalesceEnabled = ModifierCoalesceEnabled();
    const bool interactiveDrag = coalesceEnabled && terrainService->IsInteractiveModifierEdit();
    // A drag that just ended (flag cleared) with a deferred bake owed must run its
    // settle bake now even though the change gate is quiet — no new ECS stamp lands
    // on the frame the mouse is released.
    const bool forceSettleBake = coalesceEnabled && m_GeometryBakePending && !interactiveDrag;

    // Drag rising edge: reset the live-preview throttle so each drag starts its
    // cadence fresh — the baseline clock is re-taken on the first bake-worthy
    // frame, the adaptive interval returns to base, and the per-drag preview
    // counter zeroes. Runs before any early return so the edge is never missed.
    if (interactiveDrag && !m_WasInteractiveDrag)
    {
        m_HasPreviewBakeBaseline = false;
        if (!m_PreviewIntervalPinned)
            m_PreviewIntervalMs = kBasePreviewIntervalMs;
        m_DragPreviewBakeCount = 0;
    }
    m_WasInteractiveDrag = interactiveDrag;

    // Change gate: on frames where no modifier (or terrain) input could have
    // changed, skip the gather + content hash entirely — the previous bake is
    // still correct. The downstream hash remains the authority on whether a
    // bake actually runs; the gate only decides whether it is worth looking.
    if (!ShouldGather(world, *terrainService) && !forceSettleBake)
    {
        terrainService->EndModifierAccess();
        return;
    }

    g_GatherCount.fetch_add(1, std::memory_order_relaxed);
    // ComposeTiledGroundBlock answers off-camera ground queries from this stack, so
    // it must describe the state this bake is about to apply.
    GatherModifiers(world, *terrainService);
    // A terrain whose height a modifier may touch pages only once its height page overlay holds
    // the gathered set (TerrainHeightIsPaged).
    terrainService->SetHeightModifiersPresent(!m_AppliedModifiers.empty());
    BakeGrassFields(world, *terrainService);
    const std::vector<ResolvedModifier>& modifiers = m_AppliedModifiers;

    // Hash per-modifier state; fold into the combined change-detection hash.
    // The geometry hash (no zone payload version) is recorded in the snapshot so
    // the region diff can tell a moved/param edit (dirty old ∪ new bounds) from
    // a pure brush stroke (dirty the payload sub-rect) — design §3.2.
    std::vector<uint64> modifierHashes;
    std::vector<uint64> modifierGeometryHashes;
    modifierHashes.reserve(modifiers.size());
    modifierGeometryHashes.reserve(modifiers.size());
    uint64 hash = 0;
    for (const auto& mod : modifiers)
    {
        const uint64 modHash = HashModifierState(mod);
        modifierHashes.push_back(modHash);
        modifierGeometryHashes.push_back(HashModifierGeometry(mod));
        hash = HashCombine(hash, modHash);
    }

    const uint64 terrainStateHash = ComputeTerrainStateHash(world, *terrainService);
    // An empty modifier list keeps the 0 sentinel so startup with no modifiers
    // never bakes; the first Update after the last modifier is removed bakes
    // once (base reset through the normal apply path below).
    hash = modifiers.empty() ? 0 : HashCombine(hash, terrainStateHash);

    // Tiles that streamed in since the last bake need their modifiers applied
    // even when neither the modifier set nor the terrain config changed (tile
    // residency is intentionally not in either hash — see ComputeTerrainStateHash).
    // Their bake is scoped to the new tiles (BakeTiledStreamedTiles) so the rest
    // of the terrain is never re-baked or version-bumped during streaming flight.
    const bool newTilesNeedBake = AnyTiledTileNeedsModifierBake(world, *terrainService);

    // Modifier-free base re-fill: with the hash pinned to the 0 sentinel, a
    // base-source delta (heightmap hot-reload evict, BaseSource/GUID edit)
    // would otherwise no-op — the natural authoring state for pure-heightmap
    // terrains. Compare the component-level base inputs against their own
    // baseline instead. The baseline seeds on the first gather WITHOUT baking
    // and re-baselines post-bake alongside the modifier hash (#405 idle-loop
    // discipline); it never folds handles, so extraction creating terrain
    // data doesn't trip it.
    const uint64 baseInputHash = ComputeBaseInputHash(world, *terrainService);
    const bool baseRefillNeeded = m_HasBaseInputBaseline
        && modifiers.empty()
        && baseInputHash != m_LastBaseInputHash;
    if (!m_HasBaseInputBaseline)
    {
        m_LastBaseInputHash = baseInputHash;
        m_HasBaseInputBaseline = true;
    }

    if (hash == m_LastModifierHash && !baseRefillNeeded && !newTilesNeedBake)
    {
        // A coalesced drag whose net change is nil (e.g. dragged back to the last
        // baked position, or a settle frame after a discrete edit already baked)
        // needs no bake — drop the pending flag so the gate can go quiet again.
        m_GeometryBakePending = false;
        // A terrain that started to page since the last bake (its store opened) still needs
        // the overlay of the unchanged modifier set.
        BakeAwaitedPageOverlays(world, *terrainService, modifiers);
        terrainService->EndModifierAccess();
        return; // Nothing changed.
    }
    // A bake follows and writes the terrain data in place: a save's copy is taken first.
    TakePendingBakeStoreSnapshot();

    // A PURE streamed-tile wake: neither the modifier set nor the terrain config
    // changed, we are here only to bake tiles that streamed in. It must NEVER take
    // the full-bake path (which re-bakes + version-bumps every resident tile — the
    // E6-review#3 churn, and also the redundant whole-tile re-fill in a modifier-
    // free streaming scene). The scoped BakeTiledStreamedTiles handles it, applying
    // modifiers only to tiles that actually intersect one and leaving the rest
    // (including all tiles in a modifier-free scene) as their already-final
    // streamed base.
    const bool streamedTileWakeOnly =
        newTilesNeedBake && hash == m_LastModifierHash && !baseRefillNeeded;

    // Decide bake scope. A region re-bake is exact only when the terrain set
    // itself is unchanged and a previous bake exists to diff against: the
    // dirty region is the union of old AND new bounds of every modifier whose
    // state changed (a moved stamp dirties both where it was and where it is;
    // the base is deterministic per sample, so re-baking just the region
    // reproduces a full bake exactly).
    //
    // Height and splat dirt are tracked separately: PaintLayer modifiers only
    // touch the splatmap, and a paint-only change must NOT re-bake heights or
    // bump HeightfieldVersion (which would re-cook the physics collider every
    // paint-drag frame). Every height change also dirties the splat (slope-
    // and height-based weights follow the heights).
    bool fullBake = !streamedTileWakeOnly;
    DirtyUnion heightDirty;
    DirtyUnion splatDirty;
    // Classify what drove this frame's diff: a geometry change (a modifier/zone
    // moved, rotated, scaled, a param edit, or was added/removed) vs a pure
    // payload edit (a brush stroke). Only pure-geometry drags are coalesced.
    bool anyGeometryChange = false;
    bool anyPayloadChange = false;
    auto addDirtyBounds = [&](bool splatOnly,
                              float32 minX, float32 minZ, float32 maxX, float32 maxZ) {
        splatDirty.Add(minX, minZ, maxX, maxZ);
        // Splat-only modifiers (PaintLayer, PaintZone, a paint-only volume)
        // never touch heights.
        if (!splatOnly)
            heightDirty.Add(minX, minZ, maxX, maxZ);
    };

    if (!streamedTileWakeOnly && m_HasBaselineBake && terrainStateHash == m_LastTerrainStateHash)
    {
        fullBake = false;

        // Match current modifiers against the last bake by (entity, type).
        std::vector<bool> matched(m_LastBakeSnapshot.size(), false);
        for (size_t i = 0; i < modifiers.size(); ++i)
        {
            const auto& mod = modifiers[i];
            const ModifierBakeSnapshot* prev = nullptr;
            for (size_t j = 0; j < m_LastBakeSnapshot.size(); ++j)
            {
                const auto& snap = m_LastBakeSnapshot[j];
                if (!matched[j] && snap.Entity == mod.Entity && snap.ModType == mod.ModType)
                {
                    matched[j] = true;
                    prev = &snap;
                    break;
                }
            }

            if (!prev)
            {
                // Added modifier: dirty its bounds.
                anyGeometryChange = true;
                addDirtyBounds(mod.IsSplatOnlyModifier(), mod.BoundsMinX, mod.BoundsMinZ, mod.BoundsMaxX, mod.BoundsMaxZ);
            }
            else if (prev->GeometryHash != modifierGeometryHashes[i])
            {
                // Moved / rotated / scaled / re-parameterized: dirty where it
                // was AND where it is (a zone's move rides this exactly like the
                // existing modifiers — old ∪ new bounds).
                anyGeometryChange = true;
                addDirtyBounds(mod.IsSplatOnlyModifier(), prev->BoundsMinX, prev->BoundsMinZ, prev->BoundsMaxX, prev->BoundsMaxZ);
                addDirtyBounds(mod.IsSplatOnlyModifier(), mod.BoundsMinX, mod.BoundsMinZ, mod.BoundsMaxX, mod.BoundsMaxZ);
            }
            else if (prev->PayloadVersion != mod.DataVersion)
            {
                anyPayloadChange = true;
                // Pure brush stroke (§3.2 payload-dirty-rect extension): the
                // transform is identical, only the payload changed. Dirty the
                // stroke's world footprint, not the whole zone — otherwise a
                // full-terrain zone would re-bake its entire footprint per dab.
                if (mod.HasPayloadDirty)
                    addDirtyBounds(mod.IsSplatOnlyModifier(), mod.PayloadDirtyMinX, mod.PayloadDirtyMinZ,
                                   mod.PayloadDirtyMaxX, mod.PayloadDirtyMaxZ);
                else
                    // Version moved with no recorded sub-rect (fresh load /
                    // Play-restore / hot-reload): fall back to the full bounds.
                    addDirtyBounds(mod.IsSplatOnlyModifier(), mod.BoundsMinX, mod.BoundsMinZ, mod.BoundsMaxX, mod.BoundsMaxZ);
            }
        }
        for (size_t j = 0; j < m_LastBakeSnapshot.size(); ++j)
        {
            if (matched[j])
                continue;
            // Removed modifier: dirty where it was.
            anyGeometryChange = true;
            const auto& snap = m_LastBakeSnapshot[j];
            addDirtyBounds(snap.SplatOnly, snap.BoundsMinX, snap.BoundsMinZ, snap.BoundsMaxX, snap.BoundsMaxZ);
        }

        // Combined hash changed but no per-modifier difference was found
        // (fold collision): fall back to a full bake rather than skip work.
        // But when we are here ONLY to bake streamed-in tiles (the modifier hash
        // is unchanged), keep the region path — the stream-in must stay scoped to
        // the new tiles (BakeTiledStreamedTiles), not trigger a full re-bake.
        if (!splatDirty.Any && hash != m_LastModifierHash)
            fullBake = true;
    }

    // Throttled live preview of a pure-geometry drag: while the editor signals an
    // interactive transform drag, re-bake the affected old∪new region at a bounded
    // wall-clock cadence instead of every frame, so the modifier's effect updates
    // live as it is placed. The mouse-up settle bake (forced above the moment the
    // drag ends) lands the final position byte-identically. Never coalesce a frame
    // that also edited a payload (a stroke needs live, region-scoped feedback), a
    // full / streaming bake (those must land promptly), or the settle bake itself.
    bool previewBakeThisFrame = false;
    if (interactiveDrag && anyGeometryChange && !anyPayloadChange && !forceSettleBake
        && !fullBake && !streamedTileWakeOnly && !newTilesNeedBake)
    {
        m_GeometryBakePending = true;
        const auto now = PreviewNow();
        if (!m_HasPreviewBakeBaseline)
        {
            // First bake-worthy frame of the drag: start the cadence clock and
            // defer once, so grabbing a gizmo never hitches on frame one (and the
            // synchronous #530 settle oracle, whose whole drag spans far less than
            // one interval, still observes no mid-drag re-bake).
            m_PreviewBakeBaseline = now;
            m_HasPreviewBakeBaseline = true;
            terrainService->EndModifierAccess();
            return;
        }
        const float32 elapsedMs =
            std::chrono::duration<float32, std::milli>(now - m_PreviewBakeBaseline).count();
        if (elapsedMs < m_PreviewIntervalMs)
        {
            terrainService->EndModifierAccess();
            return; // Throttled: the previous preview stays on screen this frame.
        }
        // Interval elapsed — bake a live preview this frame. The bake below updates
        // the snapshot to this position; the settle diff then spans last-preview ∪
        // final, still an exact region bake, so the released result is unchanged.
        previewBakeThisFrame = true;
    }

    // Bake-cost timer for the adaptive backoff (used only for a preview bake).
    const auto previewBakeStart = PreviewNow();

    // Everything past here re-bakes. Ground moves on a full bake or on region dirt
    // that carries height; a splat-only pass leaves every height byte-identical, so
    // it must not bump this and make composed-ground consumers rebuild for nothing.
    if (fullBake || heightDirty.Any)
        ++m_AppliedGroundRevision;

    // Apply modifiers to all active terrains.
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity,
                  const Components::Terrain& terrain,
                  const Components::WorldTransform& worldXf)
        {
            const float32 centerX = worldXf.matrix[12];
            const float32 centerZ = worldXf.matrix[14];
            const float32 originX = centerX - terrain.SizeX * 0.5f;
            const float32 originZ = centerZ - terrain.SizeZ * 0.5f;
            // The terrain's own world Y: heightfield samples are read back as
            // `normalized * HeightScale + originY`, so world-authored effect
            // targets have to be measured against it.
            const float32 originY = worldXf.matrix[13];

            // ---- Spherical (planet) path ----
            // A planet has no planar heightfield (the cube-sphere is displaced procedurally), so
            // the modifier stack bakes into TerrainService's sphere sculpt atlas instead of a
            // heightfield. The planet is centred at the entity origin (== world origin per C7), so
            // this ignores the planar SizeX/Z origin above.
            if (terrain.Domain == Components::TerrainDomain::Spherical)
            {
                // Flatten cancels the base relief per texel, so it must read the SAME closed-form
                // params the renderer/collider use (via the entity's TerrainPlanetRelief, or the
                // default when absent — the pre-relief-component surface).
                const auto* reliefPtr = world.GetComponent<Components::TerrainPlanetRelief>(entity);
                const Components::TerrainPlanetRelief relief =
                    reliefPtr ? *reliefPtr : Components::TerrainPlanetRelief{};
                BakeSphereModifiers(*terrainService, terrain.PlanetRadius, relief, modifiers,
                                    modifierGeometryHashes, fullBake);
                return;
            }

            // ---- Tiled terrain path ----
            if ((terrain.TiledTerrainHandle != 0 || terrain.TiledTerrainGeneration != 0))
            {
                TiledTerrainHandle tiledHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration};
                auto* tiled = terrainService->GetTiledTerrainData(tiledHandle);
                if (!tiled)
                    return;

                // Region-scoped tiled bake (E6): only tiles intersecting an
                // edit's dirty rects re-bake, each at region cost. A full bake
                // (first bake, tile streamed in, terrain-set change) still walks
                // every resident tile. Both are bit-exact — the world-space base
                // and region-clamped modifiers are deterministic per sample.
                //
                // GPU height bake (slice-1c, GE_TERRAIN_GPU_BAKE): when the flag is on, this
                // terrain is whole-resident (extraction set AtlasGpuBakeEligible last frame), and
                // every height modifier is a slice-1 GPU type, SKIP the CPU per-texel eval — the
                // bake functions only record the affected tiles into GpuBakeBatch, the extraction
                // system dispatches them, and the stroke-settle readback refreshes the CPU store.
                //
                // The kernel composes the tile noise as its base (BaseFreq/BaseAmp/... below), so a
                // terrain with any other base keeps the CPU bake, which fills from that base.
                std::vector<ModifierGpu> gpuMods;
                const bool gpuEvalSkip =
                    IsGpuHeightBakeEnabled() && tiled->AtlasGpuBakeEligible &&
                    tiled->Config.Base.Source == Components::TerrainBaseSource::ProceduralNoise &&
                    PackModifiersForGpuBake(modifiers, gpuMods);
                tiled->GpuBakeBatch.Clear();

                std::vector<TileCoord> bakedTiles;
                if (fullBake)
                {
                    BakeTiledFull(*tiled, terrain.HeightScale, originY, modifiers, bakedTiles,
                                  gpuEvalSkip);
                }
                else
                {
                    BakeTiledRegion(*tiled, terrain.HeightScale, originY, modifiers,
                                    heightDirty, splatDirty, bakedTiles, gpuEvalSkip);
                    // Streamed-in tiles always take the CPU bake (their atlas slot may not be
                    // assigned yet); the GPU eval-skip covers only the edit's dirty region.
                    BakeTiledStreamedTiles(*tiled, terrain.HeightScale, originY, modifiers,
                                           bakedTiles);
                    // A streamed tile can also intersect a modifier's dirty rect
                    // in the same frame; keep one line per tile in the signal.
                    std::sort(bakedTiles.begin(), bakedTiles.end());
                    bakedTiles.erase(std::unique(bakedTiles.begin(), bakedTiles.end()),
                                     bakedTiles.end());
                }
                // The height pages carry the same modifiers as the tiles (Q1 (a)): what they add
                // to the base pages, over the whole terrain, not only its resident tiles.
                if (fullBake || heightDirty.Any || tiled->AwaitsPageOverlay())
                    BakeHeightPageOverlay(*tiled, terrain.HeightScale, originY, modifiers, fullBake, heightDirty);

                // Falsifiable runtime signal (GE_TERRAIN_TILE_DEBUG): one line
                // per edit frame naming the tiles baked + the world dirty rect.
                // A region bake that touched no tile (edit outside the terrain)
                // and every idle frame (gated out before reaching here) stay
                // silent — the same discipline as CBT.Edit.
                static const bool kTileBakeDebug = std::getenv("GE_TERRAIN_TILE_DEBUG") != nullptr;
                if (kTileBakeDebug && !bakedTiles.empty())
                {
                    // Signed (x,z) pairs — tile coords can be negative, so a
                    // linear Z*width+X id would print garbage for those.
                    std::string ids;
                    for (size_t i = 0; i < bakedTiles.size(); ++i)
                    {
                        if (i != 0) ids += ',';
                        ids += '(' + std::to_string(bakedTiles[i].X) + ','
                             + std::to_string(bakedTiles[i].Z) + ')';
                    }
                    if (heightDirty.Any || splatDirty.Any)
                    {
                        const DirtyUnion& r = heightDirty.Any ? heightDirty : splatDirty;
                        Logger::Log::Info("Terrain.TileBake tiles=[{}] region=({},{},{},{})",
                                          ids, r.MinX, r.MinZ, r.MaxX, r.MaxZ);
                    }
                    else
                    {
                        Logger::Log::Info("Terrain.TileBake tiles=[{}] region=full", ids);
                    }
                }

                // GPU height bake (slice-1b, GE_TERRAIN_GPU_BAKE): the bake above recorded each
                // baked tile's rect into GpuBakeBatch.Tiles (when the flag + this-frame eligibility
                // held). If every height modifier is a slice-1 GPU type, pack the modifier rows so
                // the extraction system re-runs the SAME height evaluation on the GPU (write-through
                // into the atlas slots). The CPU bake above stays authoritative for physics / normal
                // / splat; the GPU produces the sampled atlas height.
                if (gpuEvalSkip && !tiled->GpuBakeBatch.Tiles.empty())
                {
                    tiled->GpuBakeBatch.Modifiers = std::move(gpuMods);
                    tiled->GpuBakeBatch.HeightScale = terrain.HeightScale;
                    tiled->GpuBakeBatch.TerrainOriginY = originY;
                    tiled->GpuBakeBatch.BaseFreq = kTileNoiseFrequency;
                    tiled->GpuBakeBatch.BaseAmp = kTileNoiseAmplitude;
                    tiled->GpuBakeBatch.BaseOctaves = kTileNoiseOctaves;
                    tiled->GpuBakeBatch.BaseSeed = kTileNoiseSeed;
                    // The GPU splat kernel normalizes altitude against the committed global height
                    // range. The eval-skip bake returned (BakeTiledRegion @gpuEvalSkip / BakeTiledFull)
                    // BEFORE the CPU path's #602 recompute+commit ran, so on the FIRST GPU bake the
                    // committed range is still the uninitialized [0,0] sentinel. Feeding [0,0] to the
                    // kernel is the same local-fallback normalization family as #602's creation seam
                    // (a degenerate divide the SplatEligible guard only MASKS, never corrects — which
                    // also left the GPU splat preview dead for the whole first stroke). Commit the
                    // deterministic resident band now (mirrors BakeTiledRegion's !SplatBakeRangeValid
                    // recompute; the CPU heights are the pre-stroke base, floored to the noise band by
                    // ComputeResidentGlobalHeightRange) so the GPU splat runs live from the first
                    // stroke against the SAME band the CPU region-splat would use — never [0,0].
                    if (!tiled->SplatBakeRangeValid)
                    {
                        float32 gpuSplatMinH = 0.0f, gpuSplatMaxH = 0.0f;
                        ComputeResidentGlobalHeightRange(*tiled, gpuSplatMinH, gpuSplatMaxH);
                        tiled->SplatBakeMinH = gpuSplatMinH;
                        tiled->SplatBakeMaxH = gpuSplatMaxH;
                        tiled->SplatBakeRangeValid = true;
                    }
                    // GPU splat: the kernel derives the procedural classification and composites
                    // the authored surface-rule rows on top, so the pass runs whenever the packer
                    // can express every splat writer in the bake. A PAINT modifier still cannot be
                    // expressed — the CPU splat is then a procedural-plus-paint composite the GPU
                    // would overwrite — so a paint-carrying bake keeps the CPU splat until settle.
                    // The committed range (now always valid) is what the CPU region-splat would
                    // use mid-stroke, so the GPU splat matches it; a mid-stroke range extension
                    // makes it a stale preview the settle readback re-splats CPU-side (no GPU-side
                    // renormalize).
                    tiled->GpuBakeBatch.SplatEligible = PackSurfaceRulesForGpuSplat(
                        modifiers, tiled->GpuBakeBatch.SurfaceRules,
                        tiled->GpuBakeBatch.SurfaceRuleConditions);
                    tiled->GpuBakeBatch.SplatMinH = tiled->SplatBakeMinH;
                    tiled->GpuBakeBatch.SplatMaxH = tiled->SplatBakeMaxH;
                    // Eval-skip left the CPU heightfield stale — the readback of every dispatched
                    // tile is load-bearing (refreshes physics / normal / splat at settle).
                    tiled->GpuBakeBatch.Settle = true;
                    tiled->GpuBakeBatch.Pending = true;
                }
                return;
            }

            // ---- Single-terrain path ----
            TerrainHandle handle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration};
            auto* data = terrainService->GetTerrainData(handle);
            if (!data)
                return;

            const uint32 w = data->Heightfield.GetWidth();
            const uint32 h = data->Heightfield.GetHeight();
            if (w < 2 || h < 2)
                return;

            // Stage-A base source (§3.1), resolved once per terrain. The
            // decoded heightmap is GUID-cached in the service, so region
            // fills sample exactly the data a full fill does.
            std::shared_ptr<const Terrain::HeightfieldData> baseHeightmap;
            if (terrain.BaseSource == Components::TerrainBaseSource::HeightmapAsset
                && !terrain.TerrainAssetGuid.IsNull())
            {
                baseHeightmap = terrainService->ResolveHeightmapAsset(terrain.TerrainAssetGuid.ToGuid());
            }

            if (!fullBake)
            {
                // Convert the world-space dirty unions to sample regions using
                // the same mapping as the apply helpers (min: trunc, max:
                // trunc + 1), padded against boundary rounding. The splat
                // region gets one extra sample of padding because splat texels
                // read neighbor heights (central-difference slope).
                const float32 spacingX = data->Config.WorldSizeX / static_cast<float32>(w - 1);
                const float32 spacingZ = data->Config.WorldSizeZ / static_cast<float32>(h - 1);
                auto toSampleRect = [&](const DirtyUnion& u, int32 pad,
                                        int32& outMinX, int32& outMinZ,
                                        int32& outMaxX, int32& outMaxZ) -> bool {
                    if (!u.Any)
                        return false;
                    outMinX = std::max(0, ModifierSampleIndex(u.MinX, originX, spacingX) - pad);
                    outMinZ = std::max(0, ModifierSampleIndex(u.MinZ, originZ, spacingZ) - pad);
                    outMaxX = std::min(static_cast<int32>(w - 1),
                                       ModifierSampleIndex(u.MaxX, originX, spacingX) + 1 + pad);
                    outMaxZ = std::min(static_cast<int32>(h - 1),
                                       ModifierSampleIndex(u.MaxZ, originZ, spacingZ) + 1 + pad);
                    return outMinX <= outMaxX && outMinZ <= outMaxZ;
                };

                int32 hMinX = 0, hMinZ = 0, hMaxX = 0, hMaxZ = 0;
                const bool heightsInRegion = toSampleRect(heightDirty, 1, hMinX, hMinZ, hMaxX, hMaxZ);
                int32 sMinX = 0, sMinZ = 0, sMaxX = 0, sMaxZ = 0;
                const bool splatInRegion = toSampleRect(splatDirty, 2, sMinX, sMinZ, sMaxX, sMaxZ);
                if (!heightsInRegion && !splatInRegion)
                    return; // Dirty region doesn't touch this terrain.

                // New global height range after this frame's edit. Read from the
                // region-patched quadtree root (O(1)); only a height edit can move
                // it, so a paint-only change keeps the committed range.
                float32 newMinH = data->SplatBakeMinH;
                float32 newMaxH = data->SplatBakeMaxH;
                if (heightsInRegion)
                {
                    // Region re-bake: reset the region to the base source,
                    // then apply ALL modifiers clamped to it. Samples outside
                    // kept their previous bake, and every modifier covering
                    // them is unchanged, so the result matches a full bake
                    // everywhere.
                    BakeSingleTerrainHeights(*data, terrain.BaseSource, baseHeightmap.get(), originX, originY,
                                             originZ, modifiers, hMinX, hMinZ, hMaxX, hMaxZ);

                    // Node min/max must follow the height changes. Patch only the
                    // quadtree nodes over the edited rect instead of a full
                    // O(all-samples) rebuild — bit-identical for unchanged nodes —
                    // and take the new global range from the tree root. Falls back
                    // to a full sample scan only when the finest level can't tile
                    // the field (a resolution clamped below its LOD-implied size).
                    if (!terrainService->PatchQuadtreeRegion(handle, hMinX, hMinZ, hMaxX, hMaxZ,
                                                             newMinH, newMaxH))
                    {
                        newMinH = kHeightRangeSeedMin;
                        newMaxH = kHeightRangeSeedMax;
                        for (uint32 sz = 0; sz < h; ++sz)
                            for (uint32 sx = 0; sx < w; ++sx)
                            {
                                const float32 s = data->Heightfield.GetSample(sx, sz);
                                newMinH = std::min(newMinH, s);
                                newMaxH = std::max(newMaxH, s);
                            }
                    }
                }

                const bool splatBaked = data->SplatmapWidth == w && data->SplatmapHeight == h
                                        && !data->Splatmap.empty();

                // Re-bake the touched rect against (minH, maxH). Shared by the stable-range
                // and deferred-preview paths (the single-terrain analogue of the tiled
                // regionSplat lambda).
                auto regionSplat = [&](float32 minH, float32 maxH) {
                    if (!splatInRegion || !splatBaked)
                        return;
                    BakeSingleTerrainSplat(*data, originX, originZ, minH, maxH, /*resetRect*/ true, modifiers,
                                           sMinX, sMinZ, sMaxX, sMaxZ);
                };

                // Full re-bake against the current heights: re-caches the range and sets
                // SplatmapFullDirty so the GPU upload goes full-texture (a height-rect
                // band would leave stale weights outside it). Commits the range in full,
                // so any deferred renormalize is discharged.
                auto fullSplat = [&]() {
                    data->ResetSplatmapAndCommitRange();
                    BakeSingleTerrainSplat(*data, originX, originZ, data->SplatBakeMinH, data->SplatBakeMaxH,
                                           /*resetRect*/ false, modifiers, 0, 0,
                                           static_cast<int32>(data->SplatmapWidth) - 1,
                                           static_cast<int32>(data->SplatmapHeight) - 1);
                    data->SplatResplatPending = false;
                    data->SplatResplatIdleFrames = 0;
                };

                if (!heightsInRegion)
                {
                    // Paint-only edit: heights (and the range) are unchanged, so a
                    // region splat against the committed range is exact. A first
                    // splat bake / size mismatch falls back to a full generate.
                    if (splatBaked && data->SplatBakeRangeValid)
                        regionSplat(data->SplatBakeMinH, data->SplatBakeMaxH);
                    else
                        fullSplat();
                }
                else if (data->SplatBakeRangeValid && splatBaked
                         && data->SplatBakeMinH == newMinH && data->SplatBakeMaxH == newMaxH)
                {
                    // Committed range unchanged: region splat is bit-exact.
                    regionSplat(data->SplatBakeMinH, data->SplatBakeMaxH);
                }
                else if (data->SplatBakeRangeValid && splatBaked
                         && interactiveDrag && DeferSplatResplatEnabled())
                {
                    // Range shifted DURING an interactive drag: renormalizing the whole
                    // splat every throttled preview is the O(all-samples) hitch that pins
                    // the #560 cadence at its cap. Instead region-splat the touched rect
                    // against the STILL-COMMITTED range (a consistent preview against the
                    // not-yet-renormalized rest) and defer the one full renormalize to the
                    // stroke-settle flush. The mouse-release settle bake (interactiveDrag
                    // == false) takes the eager branch below, so the released result is
                    // immediately byte-identical to a full bake; FlushDeferredSplatResplat
                    // covers the tail if the drag ends without a net-change settle bake.
                    regionSplat(data->SplatBakeMinH, data->SplatBakeMaxH);
                    data->SplatResplatPending = true;
                    data->SplatResplatIdleFrames = 0;
                    m_AnyResplatPending = true;
                }
                else
                {
                    // Range shifted on a discrete edit / the mouse-release settle bake /
                    // the kill switch (GE_TERRAIN_DEFER_RESPLAT=0), or this is the first
                    // splat bake with no committed range: renormalize the whole splat now
                    // so the result is immediately byte-identical to a full bake. Off the
                    // per-frame drag hot path, so the O(all-samples) cost is paid once.
                    fullSplat();
                }
                data->SplatmapDirty = true;

                if (heightsInRegion)
                {
                    // Heights changed: mark the upload rect and bump
                    // HeightfieldVersion (max is exclusive, like MarkFullDirty).
                    // Paint-only changes skip this entirely so splat drags never
                    // re-cook the physics heightfield.
                    data->MarkRegionDirty(hMinX, hMinZ, hMaxX + 1, hMaxZ + 1);
                }
                return;
            }

            // A full bake is a pure function of its inputs, so the derived-data
            // cache serves one baked before for the same key (TerrainBakeCache.h).
            const auto bakeStart = std::chrono::steady_clock::now();
            // The terrain's one artifact is named by its identity: the scene it was
            // loaded from and its scene tag. A terrain without one is not cached.
            const TerrainBakeCacheConfig& bakeCache = terrainService->GetBakeCache();
            const auto* sceneTag = world.GetComponent<Components::SceneEntityTag>(entity);
            const std::filesystem::path bakeFile = TerrainBakeFile(
                bakeCache.Directory, terrain.BakeOriginScene, sceneTag ? sceneTag->View() : std::string_view{});
            const uint64 bakeKey = bakeFile.empty()
                ? 0u
                : ComputeSingleTerrainBakeKey(modifiers, *data, terrain.BaseSource, baseHeightmap.get(),
                                              originX, originY, originZ);
            const bool cacheHit = !bakeFile.empty() && ApplyCachedTerrainBake(*data, bakeFile, bakeKey);
            if (!cacheHit)
            {
                const auto heightStart = std::chrono::steady_clock::now();
                const bool heightParallel =
                    BakeSingleTerrainHeights(*data, terrain.BaseSource, baseHeightmap.get(), originX, originY,
                                             originZ, modifiers, 0, 0, static_cast<int32>(w - 1),
                                             static_cast<int32>(h - 1));
                const auto splatStart = std::chrono::steady_clock::now();
                data->ResetSplatmapAndCommitRange();
                BakeSingleTerrainSplat(*data, originX, originZ, data->SplatBakeMinH, data->SplatBakeMaxH,
                                       /*resetRect*/ false, modifiers, 0, 0,
                                       static_cast<int32>(data->SplatmapWidth) - 1,
                                       static_cast<int32>(data->SplatmapHeight) - 1);
                if (BakeTimingEnabled())
                    Logger::Log::Info("Terrain.BakeTiming single heightMs={} splatMs={} samples={} parallel={}",
                        std::chrono::duration<double, std::milli>(splatStart - heightStart).count(),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - splatStart).count(),
                        static_cast<uint64>(w) * h, heightParallel ? 1 : 0);
                if (bakeCache.Writable && !bakeFile.empty())
                    StoreTerrainBake(*data, bakeFile, bakeKey);
            }
            if (BakeTimingEnabled())
                Logger::Log::Info("Terrain.BakeTiming single-full ms={} cache={} key={:016x}",
                                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                            bakeStart).count(),
                                  bakeFile.empty() ? "off" : (cacheHit ? "hit" : "miss"), bakeKey);

            // A full bake renormalizes the entire splat against the current range,
            // discharging any deferred renormalize a prior region edit left owed.
            data->SplatResplatPending = false;
            data->SplatResplatIdleFrames = 0;
            data->MarkFullDirty();
            data->SplatmapDirty = true;
            terrainService->RebuildQuadtree(handle);
        });

    // Record the applied bake for the next region diff. ComputeTerrainStateHash
    // is now bake-invariant (tile residency was removed from it — see the tiled
    // block there), so the post-bake hash equals the pre-bake one; recomputing it
    // costs a bounded terrain-entity scan and keeps this robust to any future
    // config field that IS folded. The per-tile ModifiersApplied flags (set by
    // the bakes above) are what keep the next frame quiescent for tile streaming.
    const uint64 postBakeTerrainStateHash = ComputeTerrainStateHash(world, *terrainService);
    uint64 postBakeHash = 0;
    for (const uint64 modifierHash : modifierHashes)
        postBakeHash = HashCombine(postBakeHash, modifierHash);
    postBakeHash = modifiers.empty() ? 0 : HashCombine(postBakeHash, postBakeTerrainStateHash);

    m_LastModifierHash = postBakeHash;
    m_LastTerrainStateHash = postBakeTerrainStateHash;
    // Keep ShouldGather's baseline in sync so the gate is also quiet next frame.
    m_LastSeenTerrainStateHash = postBakeTerrainStateHash;
    // Base inputs are bake-invariant (component fields + content versions),
    // so the pre-bake value is the post-bake baseline.
    m_LastBaseInputHash = baseInputHash;
    m_HasBaseInputBaseline = true;
    m_HasBaselineBake = true;
    m_LastBakeSnapshot.clear();
    m_LastBakeSnapshot.reserve(modifiers.size());
    m_BakedZonePayloads.clear();
    for (size_t i = 0; i < modifiers.size(); ++i)
    {
        const auto& mod = modifiers[i];
        m_LastBakeSnapshot.push_back(ModifierBakeSnapshot{
            mod.Entity, mod.ModType, modifierGeometryHashes[i], mod.DataVersion,
            mod.BoundsMinX, mod.BoundsMinZ, mod.BoundsMaxX, mod.BoundsMaxZ,
            mod.Position, ModifierFootprintRadius(mod),
            mod.IsSplatOnlyModifier(), IsSphereSupportedModifier(mod)});
        if (ResolvedModifier::IsZone(mod.ModType) && !mod.PayloadGuid.IsNull())
            m_BakedZonePayloads.push_back(mod.PayloadGuid);
    }
    // The bake consumed the payloads' current dirty rects; clear them so the
    // next stroke accumulates a fresh sub-rect (the snapshot now pins the baked
    // DataVersion, so an unchanged payload next frame is correctly a no-op).
    for (const GUID& guid : m_BakedZonePayloads)
        terrainService->ClearZonePayloadDirty(guid);

    // A bake ran this frame. Keep the pending flag only while a drag is still in
    // progress (a mid-drag preview) so the release still fires a settle — clear it
    // once the drag ended (settle bake) or this bake wasn't a coalesced drag at
    // all, so the gate can go quiet.
    if (!interactiveDrag)
        m_GeometryBakePending = false;

    // Live-preview throttle bookkeeping: only a mid-drag preview bake re-times the
    // cadence and drives the adaptive backoff. Re-baseline from the END of the bake
    // so the bake's own duration isn't charged against the next interval, then set
    // the next interval from the measured main-thread bake cost (kPreviewBudget-
    // Fraction): a cheap bake floors to ~every-other-frame; an expensive CPU bake
    // widens toward the cap so the drag can never collapse back to the #530 per-
    // frame single-digit fps.
    if (previewBakeThisFrame)
    {
        const float32 bakeMs =
            std::chrono::duration<float32, std::milli>(PreviewNow() - previewBakeStart).count();
        m_PreviewBakeBaseline = PreviewNow();
        if (!m_PreviewIntervalPinned)
            m_PreviewIntervalMs = std::clamp(bakeMs * kPreviewBudgetFraction,
                                             kMinPreviewIntervalMs, kMaxPreviewIntervalMs);
        ++m_DragPreviewBakeCount;
    }

    terrainService->EndModifierAccess();
}

const TerrainModifierSystem::DecodedStampMask*
TerrainModifierSystem::ResolveStampMask(const GUID& guid)
{
    if (const auto it = m_StampMaskCache.find(guid); it != m_StampMaskCache.end())
        return &it->second;

    DecodedStampMask mask;
    mask.ContentVersion = m_StampMaskVersions[guid];
    mask.LoadFailed = true;

    auto& engine = EngineCore::GetInstance();
    if (engine.IsInitialized())
    {
        AssetMetadata meta{};
        if (engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(guid, meta) && !meta.Path.empty())
        {
            TextureAsset texture(guid, meta.Path);
            if (texture.Load() && texture.GetPixelData() != nullptr
                && texture.GetWidth() > 0 && texture.GetHeight() > 0)
            {
                // R-channel decode into normalized floats. 8-bit integer
                // sources are the accepted v1 path (blend modes hide most
                // banding at sane HeightScale); float sources (EXR/HDR,
                // decoded to *32F) are the high-precision path. 16F and
                // block-compressed formats fall back to flat white.
                const uint32 width = texture.GetWidth();
                const uint32 height = texture.GetHeight();
                const uint32 channels = std::max(1u, texture.GetChannels());
                const std::size_t texelCount = static_cast<std::size_t>(width) * height;

                switch (texture.GetFormat())
                {
                case TextureFormat::R8:
                case TextureFormat::RG8:
                case TextureFormat::RGB8:
                case TextureFormat::RGBA8:
                {
                    const uint8* pixels = texture.GetPixelData();
                    constexpr float32 kInv255 = 1.0f / 255.0f;
                    mask.Texels.resize(texelCount);
                    for (std::size_t i = 0; i < texelCount; ++i)
                        mask.Texels[i] = static_cast<float32>(pixels[i * channels]) * kInv255;
                    mask.Width = width;
                    mask.Height = height;
                    mask.LoadFailed = false;
                    break;
                }
                case TextureFormat::R32F:
                case TextureFormat::RG32F:
                case TextureFormat::RGB32F:
                case TextureFormat::RGBA32F:
                {
                    const auto* pixels = reinterpret_cast<const float32*>(texture.GetPixelData());
                    mask.Texels.resize(texelCount);
                    for (std::size_t i = 0; i < texelCount; ++i)
                        mask.Texels[i] = pixels[i * channels];
                    mask.Width = width;
                    mask.Height = height;
                    mask.LoadFailed = false;
                    break;
                }
                default:
                    break;
                }
            }
        }

        if (mask.LoadFailed)
        {
            Logger::Log::Warning("TerrainStampEffect: failed to decode stamp mask {} — using flat white",
                                 guid.ToString());
        }
    }

    return &m_StampMaskCache.emplace(guid, std::move(mask)).first->second;
}

void TerrainModifierSystem::SeedDecodedStampMaskForTests(const GUID& guid,
                                                         std::vector<float32> texels,
                                                         uint32 width, uint32 height)
{
    DecodedStampMask mask;
    mask.Texels = std::move(texels);
    mask.Width = width;
    mask.Height = height;
    mask.ContentVersion = m_StampMaskVersions[guid];
    mask.LoadFailed = false;
    m_StampMaskCache[guid] = std::move(mask);
}

void TerrainModifierSystem::SetPreviewClockForTests(std::chrono::steady_clock::time_point now)
{
    m_HasTestClock = true;
    m_TestClockNow = now;
}

void TerrainModifierSystem::AdvancePreviewClockForTests(float32 milliseconds)
{
    m_HasTestClock = true;
    m_TestClockNow += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<float32, std::milli>(milliseconds));
}

void TerrainModifierSystem::SetPreviewIntervalForTests(float32 milliseconds)
{
    m_PreviewIntervalMs = milliseconds;
    m_PreviewIntervalPinned = true;
}

uint64 TerrainModifierSystem::GetTileHeightRescanCountForTests()
{
    return g_TileHeightRescanCount.load(std::memory_order_relaxed);
}

uint64 TerrainModifierSystem::GetSplatRenormalizeTexelCountForTests()
{
    return g_SplatRenormalizeTexelCount.load(std::memory_order_relaxed);
}

uint64 TerrainModifierSystem::GetGatherCountForTests()
{
    return g_GatherCount.load(std::memory_order_relaxed);
}

std::size_t TerrainModifierSystem::GetSplatRenormalizeTexelBudgetForTests()
{
    return kSplatRenormalizeTexelsPerFrame;
}

void TerrainModifierSystem::DrainAssetInvalidations(TerrainService& terrainService)
{
    for (const GUID& guid : terrainService.TakePendingAssetInvalidations())
    {
        bool relevant = false;

        if (m_StampMaskCache.count(guid) != 0 || m_StampMaskVersions.count(guid) != 0)
        {
            m_StampMaskCache.erase(guid);
            ++m_StampMaskVersions[guid];
            relevant = true;
        }

        relevant |= terrainService.EvictDecodedHeightmap(guid);
        relevant |= terrainService.EvictZonePayload(guid);

        if (relevant)
            m_ForceGatherAfterAssetEvict = true;
    }
}

} // namespace GameEngine::TerrainECS
