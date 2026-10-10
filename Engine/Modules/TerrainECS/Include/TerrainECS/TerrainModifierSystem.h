#pragma once

#include "TerrainECS/BakeWarningLatch.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainRoutePolyline.h"
#include "TerrainECS/Erosion/ErosionFilter.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "ECS/ChangeFilter.h"
#include "ECS/Systems.h"
#include "ECS/ECS.h"
#include "Mathematics/Vector3.h"
#include "Spline/SplineData.h"
#include "Types/StringId.h"
#include "AssetCore/GUID.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <functional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace JobSystem { class WorkStealingThreadPool; class TaskHandle; }

namespace GameEngine::TerrainECS
{
struct BakeStoreSnapshot;

// ---- Shape falloff ---------------------------------------------------------

// Shape weight [0,1] for a sample `distFromEdge` metres from the shape edge,
// signed POSITIVE INSIDE.
//
// `falloff` and `falloffInward` are the two halves of ONE ramp, not two
// independent bands: the weight leaves 0 at `falloff` metres outside the edge,
// rises monotonically THROUGH the edge, and reaches 1 at `falloffInward` metres
// inside it. Evaluating both halves from a single normalized parameter is what
// keeps them from disagreeing at the edge they share — a rim with both set is
// continuous, with no step at the boundary.
//
// Either half may be 0, collapsing that endpoint onto the edge itself:
// `falloffInward` = 0 gives the flat interior every pre-volume modifier has,
// `falloff` = 0 gives a hard outer edge, and both 0 is a binary in/out mask.
// Those three cases evaluate the same expressions they always have, so a
// single-falloff bake is unchanged to the bit.
inline float32 ShapeFalloffWeight(float32 distFromEdge, float32 falloff, float32 falloffInward)
{
    // Cubic smoothstep on an already-clamped [0,1] parameter.
    auto smooth = [](float32 t) { return t * t * (3.0f - 2.0f * t); };

    if (falloff > 0.0f && falloffInward > 0.0f)
    {
        // One ramp across [-falloff, +falloffInward], the edge falling wherever
        // the two widths put it rather than at a fixed parameter value.
        const float32 t = (distFromEdge + falloff) / (falloff + falloffInward);
        return smooth(std::clamp(t, 0.0f, 1.0f));
    }
    if (distFromEdge <= 0.0f)
    {
        // Outside the shape, or exactly on the edge.
        if (falloff <= 0.0f)
            return 0.0f;
        return smooth(std::clamp(1.0f + distFromEdge / falloff, 0.0f, 1.0f));
    }
    if (falloffInward <= 0.0f)
        return 1.0f;
    return smooth(std::clamp(distFromEdge / falloffInward, 0.0f, 1.0f));
}

// ---- World position -> sample index ----------------------------------------

// Bound on any sample index this conversion produces. Far past the largest
// heightfield the engine can allocate, and small enough that the +1 and the
// small pads the call sites add on top cannot overflow int32.
constexpr int32 kMaxModifierSampleIndex = 1 << 24;

// Heightfield/splatmap sample index for a world coordinate, clamped in FLOAT
// space before the cast.
//
// float -> int32 conversion is UNDEFINED once the value leaves int32's range,
// and a Shape::Global volume's world bounds are infinite by construction, so
// this clamp is what lets an unbounded footprint flow through the same index
// arithmetic a finite one uses. Truncation is toward zero, matching the bare
// cast bit-for-bit across every index a real terrain can address. A NaN — an
// unresolved spline leaves inverted bounds behind — lands on the low bound.
inline int32 ModifierSampleIndex(float32 world, float32 origin, float32 spacing)
{
    const float32 t = (world - origin) / spacing;
    if (!(t > static_cast<float32>(-kMaxModifierSampleIndex)))
        return -kMaxModifierSampleIndex;
    if (t > static_cast<float32>(kMaxModifierSampleIndex))
        return kMaxModifierSampleIndex;
    return static_cast<int32>(t);
}

// ---- World Y -> normalized height ------------------------------------------

// The Y analogue of ModifierSampleIndex: the heightfield value a WORLD-space Y
// lands on, for a terrain whose entity sits at `terrainOriginY`.
//
// Every consumer of a heightfield sample — CBT's surface (cbt_layout.glsl), the
// editor height query, the collider — reads it as
// `world = normalized * heightScale + terrainOriginY`, so anything AUTHORED in
// world Y has to come back through this inverse before it can be blended into a
// sample. Dropping the origin term is invisible on a terrain at Y = 0 and wrong
// by exactly the terrain's own translation everywhere else.
inline float32 NormalizedHeightForWorldY(float32 worldY, float32 terrainOriginY,
                                         float32 heightScale)
{
    return (worldY - terrainOriginY) / heightScale;
}

// ---- Effect payloads -------------------------------------------------------
// One struct per kind of thing a modifier can DO to a terrain sample. Named (not
// anonymous inside the union) so a resolved effect's payload can be passed and
// filled on its own.
//
// BlendSmoothing is the SmoothMin / SmoothMax blend radius in metres of height;
// the other modes ignore it.

struct ModifierFlattenParams
{
    // PLANAR bake: a true world-space target Y, or — when UseVolumeHeight is set
    // — an offset from the volume's per-sample reference height, itself a world
    // Y (the volume entity's for circle/rectangle volumes, the route's own Y at
    // the nearest station for a SplinePath volume, the curve's Y at the closest
    // point for a SplineArea one). Either way the resolved target is a world Y,
    // which the bake converts with NormalizedHeightForWorldY.
    //
    // SPHERE bake: read radially instead (FillSphereFlattenEffect) — a planet has
    // no single up for a Y to mean anything.
    float32 TargetHeight;
    bool UseVolumeHeight;
    Components::TerrainModifierBlend Blend;
    float32 BlendSmoothing;

    // Pooling. PoolGroup is read ONLY when Blend is Average: the hash of the
    // authored name (kDefaultHeightPool when unnamed). RespectClaims applies in
    // EVERY blend mode, masking this member's own contribution by (1 - claim).
    // StationSpacing is not here — the geometry belongs to the volume.
    StringId PoolGroup;
    bool RespectClaims;
};

struct ModifierHeightOffsetParams
{
    float32 Offset; // world units, scaled by the volume weight
    Components::TerrainModifierBlend Blend;
    float32 BlendSmoothing;
    // Pooling, same meaning as the flatten's. The pooled value is a
    // DISPLACEMENT, so the pool ADDS the weighted average rather than lerping
    // the ground toward it.
    StringId PoolGroup;
    bool RespectClaims;
};

struct ModifierNoiseParams
{
    float32 Frequency; float32 Amplitude; uint32 Octaves; uint32 Seed;
    float32 Lacunarity; float32 Persistence;
    Components::TerrainModifierBlend Blend;
    float32 BlendSmoothing;
    // Strength 0 means the bake takes the plain fBM path. This is a union
    // member — every fill site sets it explicitly, including to a zeroed
    // ErosionParams when the source component authors no erosion.
    Erosion::ErosionParams Erosion;
    // Pooling, same meaning as the flatten's. The pooled value is a
    // DISPLACEMENT, so overlapping members CROSSFADE between their noise fields
    // instead of summing into twice the roughness.
    StringId PoolGroup;
    bool RespectClaims;
};

struct ModifierStampParams
{
    float32 HeightScale; float32 Rotation;
    Components::TerrainModifierBlend Blend;
    float32 BlendSmoothing;
    // Decoded mask, owned by the modifier system's GUID cache and valid for the
    // frame (the cache only mutates during gather / the pre-gather invalidation
    // drain). Null = flat white.
    const float32* MaskTexels;
    uint32 MaskWidth; uint32 MaskHeight;
    // Identity + decoded-content version, folded into HashModifierState so mask
    // reassignments and hot-reloads re-bake the stamp's region.
    uint64 MaskGuidHash; uint64 MaskVersion;
    // Pooling, same meaning as the flatten's. The pooled value is a
    // DISPLACEMENT, so two overlapping craters average instead of stacking.
    StringId PoolGroup;
    bool RespectClaims;
};

struct ModifierPaintLayerParams
{
    uint32 LayerIndex; float32 Strength; bool Replace;
    // Masks this stroke's per-texel weight by (1 - claim), read at the slot the
    // effect paints in — the same ownership rule the height effects use.
    bool RespectClaims;
};

// Authored rule rows, resolved. The rows are a POINTER into the gather's own
// storage rather than a copy in place: the authored block is ~900 bytes, and
// inlining it here would grow EVERY effect — including the flatten and noise the
// per-texel stack walk strides over — by more than an order of magnitude.
// Owned by the modifier system and valid for the frame, exactly like a stamp's
// decoded mask. Null with Count 0 when the effect authors no rows.
struct ModifierSurfaceRulesParams
{
    const Components::TerrainSurfaceRule* Rules; uint32 RuleCount;
    // Masks the volume weight every row is scaled by, so the whole block defers
    // together. Also refuses the GPU splat pack: the kernel has no ownership
    // buffer, so a packed claim-respecting block would disagree with the CPU.
    bool RespectClaims;
};

// The pool every pooled effect joins unless its author names one, and the id
// ResolvePoolGroup returns for a blank group name.
//
// Blending is the DEFAULT once Average is chosen: routes that cross share their
// ground, because that is what a junction is. Naming a group is how a region opts
// OUT into a system of its own — a road network that must not average with the
// footpaths around it.
//
// Shared by every poolable kind, which is safe because a pool is keyed by
// (kind, group): the default id collides across kinds by construction and the
// kind separates them.
inline constexpr StringId kDefaultHeightPool = 0;

// A ground claim: ownership, no height. Strength scales the volume's own shape
// weight; the bake takes the MAX across claimants.
struct ModifierGroundClaimParams
{
    float32 Strength;
};

struct ModifierSplineParams
{
    float32 HeightOffset; bool FlattenToSpline;
    bool DoPaintLayer; uint32 PaintLayerIndex; float32 PaintStrength;
    Components::TerrainModifierBlend Blend;
};

// Zone payloads (design §3.2). The pointers are owned by the TerrainService
// zone-payload store and valid for the frame; null when the payload isn't
// resident (unassigned/decode fail) — the zone then contributes nothing.
// Dimensions match the payload texture.
struct ModifierSculptZoneParams
{
    const float32* Offsets; uint32 Width; uint32 Height;
    Components::TerrainModifierBlend Blend;
};

struct ModifierPaintZoneParams
{
    const uint8* Mask; uint32 Width; uint32 Height;
    uint32 LayerIndex; float32 Strength;
};

// One entry of a modifier volume's effect stack: what to do, and where in the
// stack to do it. Deliberately carries no shape — the volume owns that, and the
// bake hands the effect a scalar weight (plus, for Flatten, a reference height).
struct ResolvedEffect
{
    // Count is a BOUND, never a value — no ResolvedEffect ever carries it. It
    // exists so that adding a kind MOVES a number, which HashModifierGeometry
    // static_asserts on: a new kind whose parameters are not folded into the
    // geometry hash edits silently and never re-bakes. Keep it last.
    enum class Kind : uint8 { Flatten, HeightOffset, Noise, Stamp, PaintLayer, Rules,
                              GroundClaim, Grass, Count };

    // Effects that only touch the splatmap, never the height. Both bake arms read this: the
    // HEIGHT pack skips these kinds rather than refusing on them (they contribute no height at
    // all), and the SPLAT packer walks exactly this set to decide what the kernel must reproduce.
    static bool IsSplat(Kind k) { return k == Kind::PaintLayer || k == Kind::Rules; }

    Kind EffectKind = Kind::Flatten;
    int32 StackOrder = 0;

    union
    {
        ModifierFlattenParams Flatten;
        ModifierHeightOffsetParams HeightOffset;
        ModifierNoiseParams Noise;
        ModifierStampParams Stamp;
        ModifierPaintLayerParams PaintLayer;
        ModifierSurfaceRulesParams Rules;
        ModifierGroundClaimParams GroundClaim;
        struct { float32 HeightScale; float32 DensityScale; } Grass;
    };

    ResolvedEffect() : Flatten{} {}
};

// A stamp effect's mask-sampling basis, hoisted out of the per-texel loop:
// inverse rotation (entity yaw plus the stamp's own Rotation) and inverse shape
// extents mapping stamp-local offsets into [0,1]² mask UV. Params is null when
// the modifier carries no stamp at all.
struct StampSampleBasis
{
    const ModifierStampParams* Params = nullptr;
    bool HasMask = false;
    float32 Cos = 1.0f, Sin = 0.0f;
    float32 InvExtX = 0.0f, InvExtZ = 0.0f;
};

// Resolved modifier data: type-erased snapshot of a modifier component
// combined with its world-space transform. Built once per frame from ECS queries.
struct ResolvedModifier
{
    enum class Type : uint8 { SculptZone, PaintZone, Volume };

    static bool IsZone(Type t) { return t == Type::SculptZone || t == Type::PaintZone; }
    // Zones that only touch the splatmap, never the height.
    static bool IsSplatOnly(Type t) { return t == Type::PaintZone; }

    // Source entity, used to match modifiers across bakes for region diffing.
    ECS::EntityHandle Entity{};

    Type ModType;
    Components::TerrainModifierShape Shape;
    float32 Priority;
    bool Enabled;

    // World-space position of the modifier entity.
    Mathematics::Vector3 Position;
    float32 YawRadians; // Y-axis rotation for rectangle orientation

    // Shape parameters (resolved from component).
    float32 Radius;
    float32 RectHalfX;
    float32 RectHalfZ;
    float32 Falloff;

    // Volume-only shape refinements. FalloffInward is the inner half of the
    // ShapeFalloffWeight ramp Falloff opens (0 = the flat interior every
    // pre-volume modifier has); Weight is the volume's master strength;
    // SplineFillInterior records TerrainVolumeShape::SplineArea (fill a closed
    // loop) against SplinePath (band only); both shapes take their footprint
    // from the analytic curve. StationSpacing is the arc-length step the route
    // below is resampled at — the route feeds only a pooled flatten's
    // per-station heights. Defaults reproduce the pre-volume behaviour exactly.
    float32 FalloffInward = 0.0f;
    float32 Weight = 1.0f;
    float32 StationSpacing = 0.5f;
    bool SplineFillInterior = true;

    // TerrainVolumeShape::Global: no footprint, weight 1 at every sample of
    // every terrain. Carried alongside Shape rather than inside it because
    // Shape is the pre-volume TerrainModifierShape, which has no global member —
    // the same reason SplineFillInterior sits here.
    //
    // The consumers that branch on Shape test this FIRST: the weight ramp
    // (ComputeWeight), the world AABB (ComputeModifierBounds), GPU-bake
    // eligibility (PackModifiersForGpuBake) and sphere support
    // (IsSphereSupportedModifier). Two deliberately do not, because a global
    // never reaches them: MakeStampBasis — the gather refuses a Stamp effect
    // inside a global volume, and the basis carries a defensive guard anyway —
    // and ModifierFootprintRadius, which only sphere-supported modifiers read.
    bool GlobalScope = false;

    // World-space AABB (computed from shape + position + falloff). Infinite on
    // both axes for a global-scoped modifier, which is what makes every
    // footprint-driven consumer — dirty region, tile eligibility, streamed-tile
    // arrival — include it without a special case of its own.
    float32 BoundsMinX, BoundsMinZ;
    float32 BoundsMaxX, BoundsMaxZ;

    // Type-specific data (union-like, selected by ModType). Unused for
    // Type::Volume, whose payload is the Effects stack below.
    union
    {
        ModifierSculptZoneParams SculptZone;
        ModifierPaintZoneParams PaintZone;
    };

    // Type::Volume only: the entity's effect components in stack order. Empty
    // for every other type. Allocated during the (change-gated) gather, so the
    // per-texel bake reads a contiguous, already-ordered span.
    std::vector<ResolvedEffect> Effects;

    // True when this modifier contributes nothing to the heightfield — a paint
    // zone, and a volume whose whole stack is paint effects.
    bool IsSplatOnlyModifier() const
    {
        if (ModType != Type::Volume)
            return IsSplatOnly(ModType);
        if (Effects.empty())
            return true;
        for (const auto& fx : Effects)
            if (!ResolvedEffect::IsSplat(fx.EffectKind))
                return false;
        return true;
    }

    // True when this modifier writes the splatmap at all.
    bool TouchesSplat() const
    {
        if (ModType == Type::Volume)
        {
            for (const auto& fx : Effects)
                if (ResolvedEffect::IsSplat(fx.EffectKind))
                    return true;
            return false;
        }
        return IsSplatOnly(ModType);
    }

    // For Spline modifiers: pointer to the resolved spline data (valid for the frame).
    // Not inside the union because it's a pointer alongside the value data.
    const Spline::SplineData* ResolvedSpline = nullptr;

    // A SplinePath volume's resampled route, owned by the modifier system and
    // valid for the frame. Non-null for exactly those volumes. It carries the
    // route's per-station heights, and a pooled flatten is its only reader
    // (ClosestStationXZ for the grade sample); the volume's footprint — shape
    // weight, reference height, world AABB — stays on ResolvedSpline's analytic
    // curve for both spline shapes.
    const RoutePolyline* Route = nullptr;

    // ---- Zone change tracking (non-union: applies only to zones) ----
    // DataVersion mirrors the store payload's version; the region diff folds it
    // separately from the geometry hash so a pure brush stroke (same transform,
    // new payload) re-bakes only its footprint, not the whole zone (§3.2).
    uint64 DataVersion = 0; // 0 for non-zone modifiers
    GUID PayloadGuid{};     // for clearing the store's dirty rect after bake
    // World-space dirty rect from the payload's accumulated dirty texel rect
    // (mapped through the zone transform, padded by falloff). Used when only the
    // payload changed; when absent the diff falls back to the full zone bounds.
    bool HasPayloadDirty = false;
    float32 PayloadDirtyMinX = 0.0f, PayloadDirtyMinZ = 0.0f;
    float32 PayloadDirtyMaxX = 0.0f, PayloadDirtyMaxZ = 0.0f;
};

// GPU splat gate + packer, in one call because the answer and the payload come from the same
// walk: the kernel reproduces the procedural classification (slope + altitude) plus any authored
// SURFACE RULE rows, and nothing else.
//
// Returns false — the caller then keeps the byte-identical CPU splat — when the bake carries a
// splat writer the kernel has no twin for: paint of any kind (a PaintLayer/PaintZone component, a
// paint effect on a volume, or a spline painting its path), or a rules volume on a SPLINE shape,
// whose signed-distance field the kernel cannot evaluate. On false the out params are left empty.
//
// On true, `outRules` holds one flattened row per authored rule, each carrying its volume's scope,
// in the order ApplySplatModifiers walks them: modifier priority, then effect stack order. Row
// conditions live in `outConditions`, addressed by the row's ConditionBase — never as an array
// member of the row, which is what keeps the kernel's dynamic index off per-invocation scratch.
bool PackSurfaceRulesForGpuSplat(const std::vector<ResolvedModifier>& mods,
                                 std::vector<SurfaceRuleGpu>& outRules,
                                 std::vector<SurfaceRuleConditionGpu>& outConditions);

// The shape ramp over an already-resolved signed distance from the shape's edge, times the
// volume's master strength — the tail every branch of ComputeWeight shares.
//
// Named because the bake also needs it directly: a SplinePath volume's texel pays one
// O(stations) polyline scan for a sample that carries its GRADE as well as its distance, so the
// bake scans once and finishes the weight here rather than asking ComputeWeight to scan again.
inline float32 VolumeShapeWeight(const ResolvedModifier& mod, float32 distFromEdge)
{
    return ShapeFalloffWeight(distFromEdge, mod.Falloff, mod.FalloffInward) * mod.Weight;
}

// The weight (0-1) a modifier's SCOPE contributes at a world-space XZ position: the shape ramp
// (ShapeFalloffWeight over the shape's signed distance from its edge) times the volume's master
// strength, or the master strength alone for a global, which has no edge for a ramp to cross.
//
// A free function beside the other two, not a member: it reads nothing but its arguments, and the
// GPU splat kernel's GE_SurfaceRuleShapeWeight mirrors it expression for expression — which the
// parity gate can only assert by calling it.
float32 ComputeWeight(const ResolvedModifier& mod, float32 worldX, float32 worldZ);

// Per-modifier record from the last applied bake: identity, state hash, and
// world-space bounds. Diffed against the current gather to compute the dirty
// region (old bounds ∪ new bounds of every modifier whose state changed).
struct ModifierBakeSnapshot
{
    ECS::EntityHandle Entity{};
    ResolvedModifier::Type ModType;
    // Geometry/param hash WITHOUT the zone payload version: a change here means
    // the modifier moved/rotated/scaled or a parameter changed → dirty old ∪
    // new bounds. Equal to the full state hash for non-zone modifiers.
    uint64 GeometryHash = 0;
    // Zone payload DataVersion (0 for non-zone modifiers). A change here with an
    // unchanged GeometryHash is a pure brush stroke → dirty the payload sub-rect.
    uint64 PayloadVersion = 0;
    float32 BoundsMinX = 0.0f, BoundsMinZ = 0.0f;
    float32 BoundsMaxX = 0.0f, BoundsMaxZ = 0.0f;
    // Sphere placement (spherical terrains): the modifier's world position + world-metre footprint
    // radius at bake time, so the next bake's region diff can classify its OLD footprint to cube-
    // face rects (the sphere analogue of the world-XZ Bounds above). Unused for planar terrains.
    Mathematics::Vector3 WorldPos{};
    float32 FootprintRadius = 0.0f;
    // Recorded rather than re-derived from ModType: a volume's answer depends on
    // its effect stack, which the snapshot deliberately does not keep.
    bool SplatOnly = false;       // contributed nothing to the heightfield
    bool SphereSupported = false; // had a sphere bake path
};

// ECS system that discovers terrain modifier entities, resolves their parameters,
// and applies them to the heightfield and splatmap of affected terrains.
// Scheduled after TransformHierarchy and SplineExtraction (declared edges) and
// BEFORE TerrainExtraction, which depends on it — so a modifier edit reaches the
// GPU in the same frame it is made.
class TerrainModifierSystem : public ECS::ISystem
{
public:
    TerrainModifierSystem();
    ~TerrainModifierSystem() override;

    const char* GetName() const override { return "TerrainModifierSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // The document last saved or opened as `previousScene` (null when it never was) was
    // saved as `savedScene` (the editor's save step). Restamps the document's own
    // terrains, those whose Terrain.BakeOriginScene is `previousScene` or null, with
    // `savedScene` (a Save As, or a first save, moves them to the new scene's identity);
    // a terrain from a subscene or a blueprint keeps the nested file's identity, which a
    // load stamps and a save of the parent leaves unchanged. Then asks the first settled
    // update to store each of the document's single terrains under its current key and
    // to prune the saved scene's cache folder to them (TerrainBakeCache.h). Settled means no
    // geometry bake deferred, no splat renormalize pending and no interactive edit:
    // the in-memory result then equals a full bake of the current inputs. No bake
    // runs for this; the copy of the bake and the file writes run off the update, on the
    // job pool. A later update that is about to bake takes the copy itself if the job has
    // not started it, so a busy pool costs at most one inline copy, never a wait for it.
    void RequestBakeCacheStore(ECS::World& world, const GUID& previousScene, const GUID& savedScene);

    // Modifier-applied ground for a block of a tiled terrain's own sample lattice,
    // composed on demand — NO TILE NEEDS TO BE RESIDENT.
    //
    // Runs ComposeTileHeights per overlapped tile — the same function the bakes run,
    // over the modifier stack the last bake applied. It resolves purely from world
    // position, so a block equals what those tiles hold once they stream in and
    // bake, including for tiles the camera has streamed out, which the tile store
    // cannot answer for at all. Pinned by
    // TerrainRegionBake.TileHeightsDoNotDependOnNeighbourResidency.
    //
    // The block is addressed in GLOBAL lattice indices: sample (i, j) is world
    // (WorldOriginX + (firstLatticeX + i) * spacing, ...) where spacing is
    // TileWorldSize / (HeightmapWidth - 1). Heights come back NORMALIZED — the
    // units the tile heightfield stores — so a caller scales by HeightScale and
    // offsets by the terrain's world Y exactly as SampleTiledHeightNormalized's
    // callers do.
    //
    // Indices outside the tile grid are left untouched in `outNormalized`; the
    // caller owns "outside the footprint", which is a wall rather than a hole.
    // Returns false only when the terrain carries no usable lattice.
    //
    // `heightScale` and `terrainOriginY` are passed rather than read off the tiled
    // config so they are the SAME values the caller scales the result back through
    // — world-Y effects resolve against them, and a composition that normalized by
    // one scale while its reader multiplied by another would bend the ground.
    [[nodiscard]] bool ComposeTiledGroundBlock(const TiledTerrainData& tiled,
                                               float32 heightScale, float32 terrainOriginY,
                                               int64 firstLatticeX, int64 firstLatticeZ,
                                               uint32 countX, uint32 countZ,
                                               std::vector<float32>& outNormalized) const;

    // Monotonic count of bakes that moved GROUND — a full bake, or a region bake
    // carrying height dirt. A paint-only edit does not bump it, because it leaves
    // every height byte-identical.
    //
    // A consumer of COMPOSED ground must fold this into whatever it uses to decide
    // that the surface changed. Resident tile versions cannot stand in for it: an
    // edit whose footprint lies outside the streamed set moves no resident tile, so
    // a revision built only from tile versions is unchanged while the ground the
    // composition returns has in fact moved.
    [[nodiscard]] uint64 GetAppliedGroundRevision() const { return m_AppliedGroundRevision; }

    // Test seam: unit tests run without an AssetManager, so they inject
    // decoded masks directly. Runtime population goes through the gather-time
    // GUID -> path -> TextureAsset decode into the same cache.
    void SeedDecodedStampMaskForTests(const GUID& guid, std::vector<float32> texels,
                                      uint32 width, uint32 height);

    // Test seam: monotonically counts how many times a tile's full heightfield was
    // scanned for its min/max (RefreshTileHeightRange). The tiled global height range
    // is now an O(tiles) aggregate of cached per-tile ranges, so an IDLE tiled terrain
    // must produce ZERO new scans per frame and a region edit must scan only the tiles
    // it touched — never all resident tiles. A perf-regression oracle guards the class
    // of per-frame O(all-samples) rescan that tanked tiled idle fps.
    static uint64 GetTileHeightRescanCountForTests();

    // Test seam: monotonically counts how many times the change gate opened and
    // the modifier gather actually ran. An idle frame must not increment it —
    // the quiet-frame oracle for scenes whose modifiers reference splines,
    // whose edits live in SplineService rather than in any ECS column.
    static uint64 GetGatherCountForTests();

    // Test seams for the spread settle renormalize (Fix 2). GetSplatRenormalizeTexelCount
    // is a monotonic count of procedural-splat texels the band-spread flush regenerated;
    // the settle-spike bound oracle asserts the per-frame delta never exceeds the budget
    // (+ one texel row of round-up slop) and that a large terrain spreads across frames.
    static uint64 GetSplatRenormalizeTexelCountForTests();
    static std::size_t GetSplatRenormalizeTexelBudgetForTests();

    // Test seams for the interactive-drag live-preview throttle. The throttle
    // paces mid-drag preview re-bakes by wall-clock time; tests inject a
    // deterministic clock (and optionally pin the interval so the adaptive
    // backoff leaves it fixed) to drive the cadence without sleeping.
    // GetDragPreviewBakeCountForTests returns how many preview bakes landed
    // during the current or most-recent drag (reset on each drag's first frame).
    void SetPreviewClockForTests(std::chrono::steady_clock::time_point now);
    void AdvancePreviewClockForTests(float32 milliseconds);
    void SetPreviewIntervalForTests(float32 milliseconds);
    uint32 GetDragPreviewBakeCountForTests() const { return m_DragPreviewBakeCount; }
    // Row bands the last bake pass fanned out across the pool; 0 when it ran whole.
    uint32 GetLastBakeBandCountForTests() const { return m_LastBakeBandCount; }

    // Row-band height for splitting a large region bake across pool workers (RunBakeRowBands). A
    // wide bake is dominated by per-texel noise + modifier + splat evaluation, all pure functions of
    // the sample's world position, so partitioning a region's rows is byte-identical to one
    // whole-region call (the region-vs-full oracles lock it) while keeping every worker busy.
    static constexpr int32 kBakeBandRows = 32;

private:
    // Decoded stamp mask: R channel normalized to [0,1], row-major.
    struct DecodedStampMask
    {
        std::vector<float32> Texels;
        uint32 Width = 0;
        uint32 Height = 0;
        uint64 ContentVersion = 0;
        bool LoadFailed = false; // negative cache: don't re-attempt every gather
    };

    // Cache lookup + synchronous decode on miss (masks are small and the
    // gather is already change-gated). Never returns null; failed decodes
    // negative-cache as flat white until the next hot-reload eviction.
    const DecodedStampMask* ResolveStampMask(const GUID& guid);

    // Drain the service's pending asset invalidations (enqueued on the
    // watcher thread) on the main thread: evict decoded mask/heightmap cache
    // entries, bump their content versions, and force one gather so the
    // re-decoded content reaches the bake.
    void DrainAssetInvalidations(TerrainService& terrainService);
    // Gather all modifier components into m_AppliedModifiers, sorted. The service
    // resolves zone payloads (and is where a moved/edited zone's payload lives).
    //
    // There is deliberately NO out-parameter: the gather clears the three arenas
    // the resolved entries point into, so a gather into anything other than
    // m_AppliedModifiers would leave that member holding dangling pointers. One
    // destination keeps the stack and its storage refreshed together.
    void GatherModifiers(ECS::World& world, TerrainService& terrainService);

    // Apply height-modifying modifiers to the heightfield. Application is
    // clamped to the sample region [regionMinX, regionMaxX] x
    // [regionMinZ, regionMaxZ] (inclusive); pass the full range for a full bake.
    //
    // `terrainOriginY` is the terrain entity's world Y — the offset every sample
    // is read back through — so effects authored in world Y (Flatten) resolve to
    // the height the author typed rather than to that height above the terrain.
    //
    // Static because it reads nothing but its arguments: every effect resolves at
    // terrainOrigin + index * spacing, so the result depends only on the world
    // positions the grid covers and the stack applied to them. That is what lets
    // ComposeTileHeights reproduce a tile the camera never streamed in.
    static void ApplyHeightModifiers(Terrain::HeightfieldData& heightfield,
                                     float32 worldSizeX, float32 worldSizeZ,
                                     float32 heightScale,
                                     float32 terrainOriginX, float32 terrainOriginY,
                                     float32 terrainOriginZ,
                                     const std::vector<ResolvedModifier>& modifiers,
                                     int32 regionMinX, int32 regionMinZ,
                                     int32 regionMaxX, int32 regionMaxZ);

    // THE tiled height composition: the terrain's base over `field` (the tile at
    // tileWorldOrigin, FillTiledBaseRegion), then the modifier stack, both clamped to
    // the same sub-rect (pass kRegionUnbounded for a whole tile — both steps clamp to
    // the field internally).
    //
    // Every producer of tiled ground goes through here — the full bake, the region
    // bake, the streamed-tile bake, and ComposeTiledGroundBlock's off-camera read —
    // so an off-camera query cannot drift from what the terrain renders. The seam
    // oracles compare a composed block against baked tiles, which only detects a
    // divergence the two paths could have; sharing the call pair removes the way
    // they could acquire one.
    static void ComposeTileHeights(Terrain::HeightfieldData& field, const TiledTerrainData& tiled,
                                   float32 tileWorldOriginX, float32 tileWorldOriginZ,
                                   float32 heightScale, float32 terrainOriginY,
                                   const std::vector<ResolvedModifier>& modifiers,
                                   int32 minX, int32 minZ, int32 maxX, int32 maxZ);

    // ComposeTiledGroundBlock's composition under `modifiers`: the normalized height of every
    // sample of a block of the terrain's global tile lattice, row-major.
    static bool ComposeLatticeBlock(const TiledTerrainData& tiled, float32 heightScale, float32 terrainOriginY,
                                    const std::vector<ResolvedModifier>& modifiers, int64 firstLatticeX,
                                    int64 firstLatticeZ, uint32 countX, uint32 countZ,
                                    std::vector<float32>& outNormalized);

    // Apply every splat-writing effect — surface rules and paint — to the
    // splatmap, in stack order, clamped like ApplyHeightModifiers.
    //
    // `heightfield`, `heightScale` and the (splatMinH, splatMaxH) range are what
    // surface rules measure: the splat grid is the heightfield grid, so a texel's
    // slope and height come from the same indices; the heightfield stores height
    // normalized to [0,1], so metres need the scale; and the range is the one the
    // procedural pass just normalized against. Every call site generates the
    // procedural splat immediately before this and therefore already holds them.
    void ApplySplatModifiers(std::vector<uint8>& splatmap,
                             uint32 splatmapWidth, uint32 splatmapHeight,
                             const Terrain::HeightfieldData& heightfield,
                             float32 worldSizeX, float32 worldSizeZ,
                             float32 terrainOriginX, float32 terrainOriginZ,
                             float32 heightScale,
                             float32 splatMinH, float32 splatMaxH,
                             const std::vector<ResolvedModifier>& modifiers,
                             int32 regionMinX, int32 regionMinZ,
                             int32 regionMaxX, int32 regionMaxZ);

    // World-space accumulated dirty bounds (union of changed modifier
    // footprints). Shared by the single-terrain and tiled bake paths.
    struct DirtyUnion
    {
        float32 MinX = 0.0f, MinZ = 0.0f, MaxX = 0.0f, MaxZ = 0.0f;
        bool Any = false;
        void Add(float32 minX, float32 minZ, float32 maxX, float32 maxZ)
        {
            if (!Any)
            {
                MinX = minX; MinZ = minZ; MaxX = maxX; MaxZ = maxZ;
                Any = true;
            }
            else
            {
                MinX = std::min(MinX, minX);
                MinZ = std::min(MinZ, minZ);
                MaxX = std::max(MaxX, maxX);
                MaxZ = std::max(MaxZ, maxZ);
            }
        }
    };

    // Brings the tiled terrain's height page overlay (TiledTerrainData::PageOverlay) up to
    // `modifiers` after a bake: the composed height minus the base over the lattice rect the
    // height modifiers reach, recomputed only over `heightDirty` when the rect still holds it
    // (TerrainModifierPageOverlay.cpp). Builds nothing for a terrain with no overlay cap (one that
    // keeps its height texture) and refuses an overlay over the cap (TiledTerrainData::PageOverlayByteCap).
    void BakeHeightPageOverlay(TiledTerrainData& tiled, float32 heightScale, float32 terrainOriginY,
                               const std::vector<ResolvedModifier>& modifiers, bool fullBake,
                               const DirtyUnion& heightDirty);

    // BakeHeightPageOverlay for every tiled terrain that awaits its overlay
    // (TiledTerrainData::AwaitsPageOverlay) under an unchanged modifier set.
    void BakeAwaitedPageOverlays(ECS::World& world, TerrainService& terrainService,
                                 const std::vector<ResolvedModifier>& modifiers);

    // Full re-bake of every resident (Full) tile of a tiled terrain: the
    // terrain's base + height modifiers, then a procedural splatmap over the
    // global height range + paint modifiers. Records the baked range so a later
    // region bake can tell whether the range shifted. Appends each baked tile's
    // coord to outBakedTiles (the runtime debug signal prints signed pairs).
    // gpuEvalSkip (GE_TERRAIN_GPU_BAKE, slice-1c): when true, the per-texel height
    // evaluation + all height-derived CPU work (range, splat, quadtree, dirty marking)
    // is SKIPPED — each affected tile is recorded into TiledTerrainData::GpuBakeBatch and
    // the GPU produces the height, with the settle readback later refreshing the CPU store.
    void BakeTiledFull(TiledTerrainData& tiled, float32 heightScale, float32 terrainOriginY,
                       const std::vector<ResolvedModifier>& modifiers,
                       std::vector<TileCoord>& outBakedTiles, bool gpuEvalSkip);

    // Region-scoped re-bake of a tiled terrain (E6): only tiles intersecting the
    // world-space dirty rects are touched, each at region cost. Bit-exact with a
    // full bake for the same modifier state — the terrain's base and modifiers
    // clamped to the region are deterministic per world sample, so untouched
    // tiles keep their previous (already-correct) bake. A global height-range
    // shift forces a splat regen across all tiles (height-based layers depend on
    // it) without re-baking any untouched heights.
    void BakeTiledRegion(TiledTerrainData& tiled, float32 heightScale, float32 terrainOriginY,
                         const std::vector<ResolvedModifier>& modifiers,
                         const DirtyUnion& heightDirty, const DirtyUnion& splatDirty,
                         std::vector<TileCoord>& outBakedTiles, bool gpuEvalSkip);

    // One inclusive sample rectangle [MinX, MaxX] x [MinZ, MaxZ] of a bake pass.
    struct BakeBandRegion
    {
        int32 MinX = 0;
        int32 MinZ = 0;
        int32 MaxX = 0;
        int32 MaxZ = 0;
    };
    // Applies rowFn(regionIndex, minX, maxX, z0, z1) over every region. With a pool
    // and enough texels the rows are split into kBakeBandRows bands fanned across
    // m_JobPool and joined; otherwise each region runs whole on this thread. Each
    // band writes a disjoint row range of its region. Returns true when it fanned out.
    bool RunBakeRowBands(std::span<const BakeBandRegion> regions,
                         const std::function<void(std::size_t, int32, int32, int32, int32)>& rowFn);

    // A single (untiled) terrain's height pass over an inclusive sample rect: reset
    // to the base source, then every modifier clamped to it, in row bands across the
    // pool for a large rect. True when it fanned out.
    bool BakeSingleTerrainHeights(TerrainData& data, Components::TerrainBaseSource baseSource,
                                  const Terrain::HeightfieldData* baseHeightmap, float32 originX, float32 originY,
                                  float32 originZ, const std::vector<ResolvedModifier>& modifiers, int32 minX,
                                  int32 minZ, int32 maxX, int32 maxZ);
    // A single terrain's splat pass over an inclusive texel rect against (minH, maxH),
    // resetting the rect first when resetRect is set, in row bands for a large rect.
    void BakeSingleTerrainSplat(TerrainData& data, float32 originX, float32 originZ, float32 minH, float32 maxH,
                                bool resetRect, const std::vector<ResolvedModifier>& modifiers, int32 minX,
                                int32 minZ, int32 maxX, int32 maxZ);

    // Renormalize every resident Full tile's splatmap against (globalMinH, globalMaxH)
    // and commit that range (first bake and the deferred stroke-settle flush).
    void RenormalizeAllTileSplats(TiledTerrainData& tiled, float32 tileSize,
                                  const std::vector<ResolvedModifier>& modifiers,
                                  float32 globalMinH, float32 globalMaxH,
                                  std::vector<TileCoord>& outBakedTiles);

    // Renormalize a horizontal texel-row band [rowStart, rowEnd] (inclusive) of one
    // resident tile's splatmap against (globalMinH, globalMaxH), leaving the rest of
    // the tile untouched and NOT marking it dirty. The band-spread settle flush
    // (FlushDeferredSplatResplat) calls this to hold each settle frame within a texel
    // budget; each row is visited exactly once across a tile's bands, so the fully
    // banded result is byte-identical to RenormalizeAllTileSplats (same generator,
    // same frozen range). Returns the number of texels processed (band area).
    std::size_t RenormalizeTileSplatRows(TerrainTileData& tile, float32 tileSize,
                                         float32 heightScale,
                                         const std::vector<ResolvedModifier>& modifiers,
                                         float32 globalMinH, float32 globalMaxH,
                                         int32 rowStart, int32 rowEnd);

    // Settle a deferred whole-terrain splat renormalize once a stroke has stopped
    // (edit-realtime). Called on quiescent frames (ShouldGather == false) while any
    // tiled terrain carries SplatResplatPending: counts idle frames per terrain and,
    // after kSplatResplatSettleFrames of quiet, runs the one full renormalize against
    // the final range so the settled splat is byte-identical to a full bake. Recomputes
    // m_AnyResplatPending. No work once every terrain has settled.
    void FlushDeferredSplatResplat(ECS::World& world, TerrainService& terrainService);

    // Apply the authored modifiers to tiles that streamed in since the last bake
    // (LodState Full but ModifiersApplied still false), scoped to those tiles.
    // A tile arrives from streaming with its base only, so its modifiers must
    // be baked in — but a stream-in must NOT re-bake or version-bump any already-
    // resident tile (the E6-review#3 stream-churn contract: no collider re-cook
    // storm during streaming flight). Bit-identical to how BakeTiledFull would
    // treat each new tile (same base fill + modifier application + global-range
    // splat), verified by parity oracle. Other tiles keep their splat; residual
    // global-range drift is reconciled by the next authored edit.
    void BakeTiledStreamedTiles(TiledTerrainData& tiled, float32 heightScale,
                                float32 terrainOriginY,
                                const std::vector<ResolvedModifier>& modifiers,
                                std::vector<TileCoord>& outBakedTiles);

    // The height a flatten effect levels toward when it asks for the volume's
    // reference: the route's own Y at the nearest station for a SplinePath
    // volume, the curve's own Y at the closest XZ point for a SplineArea one (so
    // a sloped road flattens to its own profile either way), and the entity's Y
    // for every other shape. Only called when a flatten effect is present.
    //
    // `routeSample` is the caller's already-computed polyline scan, non-null
    // exactly when mod.Route is: the SplinePath answer IS that sample's Height,
    // and re-scanning to fetch it would double the per-texel cost of the very
    // geometry this volume was resampled to make cheap.
    static float32 ComputeVolumeReferenceHeight(const ResolvedModifier& mod,
                                                const RouteSample* routeSample,
                                                float32 worldX, float32 worldZ);

    // The value ONE height effect contributes at ONE sample, in normalized
    // height. The single implementation of the four effect maths: the in-place
    // blend below and the pool accumulator both read it, so a pooled noise and an
    // unpooled one cannot evaluate different fields.
    //
    // Returns FALSE when the effect contributes nothing at all here — a splat-only
    // kind, a ground claim, or a stamp in a global volume. That is distinct from
    // contributing zero: a pooled member that contributed weight with value 0
    // would drag its pool's average toward zero instead of standing aside.
    //
    // `routeSample` is the pooled flatten's per-station grade and null everywhere
    // else; only the flatten reads it.
    static bool HeightEffectValue(const ResolvedModifier& mod, const ResolvedEffect& effect,
                                  float32 worldX, float32 worldZ,
                                  float32 worldSizeX, float32 heightScale,
                                  float32 terrainOriginY,
                                  const StampSampleBasis& stampBasis,
                                  const RouteSample* routeSample,
                                  float32& outValueNorm);

    // Apply ONE height effect to ONE heightfield sample (normalized height,
    // nominally [0,1] but accumulated UNCLAMPED across effects,
    // hence heightScale). Every height effect routes through it, so the CPU bake
    // has exactly one implementation of the per-texel maths.
    //
    // A POOLED effect never comes through here: its pool owns the one write it
    // makes, and a second in-place write would double-apply it. That is also why
    // no route reaches this function — the station polyline carries a ROUTE's
    // grade, and only a pooled flatten grades along one. An unpooled flatten on a
    // spline volume references the curve, as every spline footprint does.
    static void ApplyHeightEffectSample(const ResolvedModifier& mod, const ResolvedEffect& effect,
                                        float32& currentHeight, float32 weight,
                                        float32 worldX, float32 worldZ,
                                        float32 worldSizeX, float32 heightScale,
                                        float32 terrainOriginY,
                                        const StampSampleBasis& stampBasis);

    // The stamp basis for a modifier, or a Params-null basis when it has no
    // stamp. Reads the volume's stack (one effect of each kind per entity, so a
    // volume has at most one stamp).
    static StampSampleBasis MakeStampBasis(const ResolvedModifier& mod);

    // Spherical (planet) bake: re-derive the sphere sculpt atlas's modifier layer over the (face,
    // rect) regions the changed modifiers touch. A planet has no planar heightfield (the CBT
    // renderer displaces the cube-sphere procedurally), so the modifier stack bakes into
    // TerrainService's sphere sculpt layer instead — the sphere analogue of ApplyHeightModifiers.
    // Supported modifier types are the height-affecting, footprinted ones: Noise, Sculpt zones,
    // Flatten (levels the base relief toward a radial target — needs `relief` to cancel the
    // closed-form relief per texel), and Stamp (Add/Subtract a mask disc), each with a circle or
    // rectangle shape. Paint / Spline modifiers — and any modifier whose shape is Spline, which has
    // no spherical SDF analogue — are not baked on planets and are surfaced honestly (one-shot log
    // + inspector notice), never silently dropped. `fullBake` re-derives all six faces; otherwise the region diff (current vs the last
    // bake snapshot) unions each changed modifier's old ∪ new footprint into the dirty face set.
    void BakeSphereModifiers(TerrainService& terrainService, float32 planetRadius,
                             const Components::TerrainPlanetRelief& relief,
                             const std::vector<ResolvedModifier>& modifiers,
                             const std::vector<uint64>& modifierGeometryHashes, bool fullBake);

    // Change gate: decides whether the gather + hash + bake path runs this
    // frame. True when any modifier component / modifier transform changed
    // (Changed<> scans over modifier archetypes), a modifier was added or
    // removed (lifecycle events), the terrain-state hash moved, or a
    // structural fallback demands a full run (first gated run, lifecycle
    // window gap > 1, World::Clear). Always true when gating is disabled
    // (GE_TERRAIN_CHANGE_GATING=0 or GE_ECS_CHANGE_FILTER=0).
    //
    // The scanned type set is folded out of ModifierRootComponents /
    // ModifierEffectComponents (TerrainModifierComponents.h) so it cannot drift
    // from what GatherModifiers reads — a component the gather reads but the
    // gate misses bakes once and then goes deaf to its own edits.
    bool ShouldGather(ECS::World& world, TerrainService& terrainService);

    // Hash of all state that affects a modifier's output (fields + spline
    // version + zone payload version).
    static uint64 HashModifierState(const ResolvedModifier& mod);

    // Geometry/parameter hash EXCLUDING the zone payload version. Recorded in
    // the bake snapshot so a pure brush stroke (payload-only change) is
    // distinguishable from a move/param edit in the region diff (design §3.2).
    static uint64 HashModifierGeometry(const ResolvedModifier& mod);

    // Work-stealing pool captured from the World at Update entry, used to fan the
    // large-region bake's per-tile height/splat passes across workers. Null (no
    // pool) falls back to the serial path — the brush-dab regions run serially
    // regardless (they are below the parallel-dispatch threshold).
    JobSystem::WorkStealingThreadPool* m_JobPool = nullptr;

    // The update proper; Update runs it, then a settled store request.
    void UpdateBakes(ECS::World& world);
    // Serves a RequestBakeCacheStore once the bake is settled; keeps it pending otherwise.
    void StoreRequestedBakes(ECS::World& world);
    GUID m_BakeStoreScene{};
    bool m_BakeStoreRequested = false;
    // The last store's job: it copies each terrain's bake, then writes the files. Waited
    // before the next store and at destruction.
    std::unique_ptr<JobSystem::TaskHandle> m_BakeStoreWrites;
    // The last store's copy of each terrain's bake, until a bake has made sure it is taken.
    // The copy reads the terrain data in place and only this system's bakes write it.
    std::shared_ptr<BakeStoreSnapshot> m_PendingBakeStoreSnapshot;
    // Before a bake writes terrain data: takes the pending store's copy inline when the job
    // has not claimed it yet, otherwise waits for the job's copy, which is already running.
    void TakePendingBakeStoreSnapshot();
    // Bands the last RunBakeRowBands fanned out across the pool; 0 when it ran whole.
    uint32 m_LastBakeBandCount = 0;
    // The splat pass warns once per update for each out-of-range paint layer (keyed
    // by the index) and for each splat/heightfield size mismatch that leaves rules
    // unaddressable (keyed by the four sizes), however many terrains, tiles and
    // concurrent row bands meet it. Reset at the top of Update.
    BakeWarningLatch m_LayerClampWarnings;
    BakeWarningLatch m_UnaddressableRulesWarnings;

    // The stack the last gather resolved, kept rather than discarded so an
    // off-camera ground query composes against exactly what the last bake applied
    // instead of re-gathering a stack from a different moment.
    //
    // THE BORROW RULE, stated once for this member and the three arenas below.
    // A ResolvedModifier is not self-contained: its ResolvedSpline, surface-rule
    // block and Route point into m_TransformedSplines / m_ResolvedRuleBlocks /
    // m_RoutePolylines. GatherModifiers clears all three unconditionally, so those
    // pointers survive exactly until the next gather — NOT merely "for the frame",
    // because this stack now outlives the frame that produced it and answers
    // queries on later ones.
    //
    // The invariant that keeps it sound: EVERY gather writes this member, which is
    // why GatherModifiers has no out-parameter. A gather into a local would clear
    // the arenas while leaving this stack pointing into the freed elements, and the
    // next compose would read them.
    std::vector<ResolvedModifier> m_AppliedModifiers;

    // Same gather/arena lifetime as m_AppliedModifiers, containing only grass
    // effects. Kept separate so grass-only changes cannot dirty physics/materials.
    std::vector<ResolvedModifier> m_GrassModifiers;
    void BakeGrassFields(ECS::World& world, TerrainService& terrainService);
    struct GrassBakeSnapshot
    {
        ECS::EntityHandle Entity;
        uint64 Hash;
        float32 MinX, MinZ, MaxX, MaxZ;
    };
    std::vector<GrassBakeSnapshot> m_LastGrassSnapshot;
    uint64 m_LastGrassHash = 0;
    uint64 m_LastGrassTerrainHash = 0;
    bool m_HasGrassBaseline = false;

    // See GetAppliedGroundRevision.
    uint64 m_AppliedGroundRevision = 0;

    // Track modifier state hash to avoid re-applying unchanged modifiers.
    uint64 m_LastModifierHash = 0;

    // One-shot: warn once when a spherical terrain carries modifier types the sphere bake does
    // not support yet (paint / flatten / stamp / spline), so it is never a silent no-op.
    bool m_SphereUnsupportedWarned = false;

    // One-shot: warn once when the analytic-eligible modifier count exceeds the bounded set
    // (kMaxSphereAnalyticModifiers) — the excess falls back to the store bake, never dropped.
    bool m_SphereAnalyticOverflowWarned = false;

    // True while any tiled terrain has a deferred splat renormalize owed (set when a
    // stroke's height edit shifts the global range). Keeps the quiescent-frame path
    // running the settle check until every terrain has flushed; false in steady state.
    bool m_AnyResplatPending = false;

    // Change-gating state. One gate shared by all of ShouldGather's scans:
    // like TransformHierarchySystem's serial gate, they feed a single
    // all-or-nothing decision and are written back to one entry sample
    // (M14 contract — entry-sample before the scans, write-back after).
    ECS::ChangeGate m_ChangeGate;
    bool m_ChangeGateValid = false;
    uint64 m_LastLifecycleSwapGeneration = 0;
    uint64 m_LastLifecycleResetGeneration = 0;
    // Terrain-side signal: covers terrain config/handle churn and tiled tile
    // streaming (service-internal state no Changed<> scan can see).
    uint64 m_LastSeenTerrainStateHash = 0;

    // Region diffing state: what the last applied bake looked like.
    // Region re-bakes are only valid while the terrain set itself is unchanged
    // (m_LastTerrainStateHash) and a baseline bake exists (m_HasBaselineBake).
    std::vector<ModifierBakeSnapshot> m_LastBakeSnapshot;
    uint64 m_LastTerrainStateHash = 0;
    bool m_HasBaselineBake = false;

    // Transformed copies of spline data (local-space points → world-space).
    // Stored here so pointers in ResolvedModifier::ResolvedSpline stay valid for
    // the lifetime the borrow rule on m_AppliedModifiers describes. A deque, not a
    // vector: the volume gather appends to it and takes pointers as it goes, so
    // growth must not relocate.
    std::deque<Spline::SplineData> m_TransformedSplines;

    // Resolved copies of authored rule blocks, for the same reason and with the
    // same lifetime as the splines above: ModifierSurfaceRulesParams points into
    // this, so an append must not relocate what an earlier volume already holds.
    std::deque<Components::TerrainSurfaceRulesEffect> m_ResolvedRuleBlocks;

    // Resampled routes, same lifetime and same reason as the two above:
    // ResolvedModifier::Route points into this.
    std::deque<RoutePolyline> m_RoutePolylines;

    // Entities already warned about a Shape::Spline with no SplineComponent to
    // resolve it against — a scene-authoring error that cannot fix itself, so it
    // is reported once per entity instead of every gather.
    std::unordered_set<uint32> m_SplineShapeUnresolvedWarned;

    // Entities already warned about a Stamp effect on a Shape::Global volume.
    // Same reasoning as above — an authoring error the scene cannot resolve on
    // its own, reported once per entity rather than every gather. Warned from
    // the GATHER, which is single-threaded; the bake that would otherwise be the
    // natural site runs its rows across job workers.
    //
    // Never cleared, which is the trade both of these sets make: once-per-entity
    // -per-session means someone who fixes the shape and later reintroduces the
    // same mistake on that entity gets no second warning. Clearing on the fixing
    // edit would restore it, at the cost of re-warning every time a reload or an
    // undo walks back through the bad state — the noisier failure. The inspector
    // notice is recomputed from live component state, so it is the channel that
    // always tells the truth; this one is a log breadcrumb, not the UI.
    std::unordered_set<uint32> m_GlobalStampRefusedWarned;

    // Entities already warned about a pooled flatten on a SPHERICAL terrain,
    // which has no bake region to accumulate a pool into. Same reasoning and
    // same lifetime as the two sets above.
    std::unordered_set<uint32> m_SpherePooledEffectWarned;

    // A claim is accumulated at its OWN slot in the priority order and a
    // claim-respecting effect reads it at the slot that effect applies in, so a
    // claim that sorts after that slot is a silent no-op on it: the ordering is
    // load-bearing, and nothing in the authoring surface says so. The same pass
    // reports the other way a claim silences an effect — one written by the very
    // volume that shapes the ground, at a lower stack position. Reported once per
    // entity, same lifetime as the sets above.
    void WarnOnClaimsThatCannotMask(const std::vector<ResolvedModifier>& modifiers);
    std::unordered_set<uint32> m_ClaimAboveEffectWarned;
    std::unordered_set<uint32> m_EffectSelfClaimWarned;

    // The splat side's companion trap: the splatmap starts EMPTY every bake and
    // an empty texel resolves to material channel 0, so masking a splat effect
    // off claimed ground preserves the claimant's material only if the claimant
    // paints it. A claim with no paint under a deferring effect cuts a patch of
    // the DEFAULT material instead of preserving anything. Reported once per
    // claimant, same lifetime as the sets above.
    void WarnOnClaimedGroundWithNoPaint(const std::vector<ResolvedModifier>& modifiers);
    std::unordered_set<uint32> m_ClaimWithoutPaintWarned;

    // Clamp a resolved rule block's counts to the caps, reporting once per
    // entity what was dropped. Reaching here means the value came in by a route
    // that is not the scene loader (which rejects an over-cap count outright) or
    // the editor — so it is a genuine authoring error, and silently baking the
    // first kMaxTerrainSurfaceRules rows would hide it.
    void ClampRuleCountsToCaps(Components::TerrainSurfaceRulesEffect& block,
                               ECS::EntityHandle entity);
    std::unordered_set<uint32> m_RuleCapOverflowWarned;

    // Decoded stamp masks by GUID. Content versions survive eviction so
    // HashModifierState sees reloaded mask content as a state change.
    std::unordered_map<GUID, DecodedStampMask> m_StampMaskCache;
    std::unordered_map<GUID, uint64> m_StampMaskVersions;

    // Set by DrainAssetInvalidations when a referenced asset was evicted;
    // ShouldGather ORs it in (no ECS state changes on hot-reload, so the
    // Changed<> scans and lifecycle events can't see it).
    bool m_ForceGatherAfterAssetEvict = false;

    // Zone-payload edit epoch baseline. A brush stroke mutates the service
    // store, not any ECS component, so ShouldGather wakes on an epoch change
    // (like the asset-evict flag). Seeded on the first gather; re-baselined
    // after every gather so the gate is quiet once the stroke is baked.
    uint64 m_LastSeenZonePayloadEpoch = 0;
    bool m_HasZonePayloadEpochBaseline = false;

    // Spline edit epoch baseline, the same shape as the zone-payload epoch above:
    // a control-point drag mutates SplineService and no ECS column, so the gate
    // wakes on an epoch change. Seeded on the first gather and re-sampled every
    // frame, so an idle spline scene stays quiet.
    uint64 m_LastSeenSplineEpoch = 0;
    bool m_HasSplineEpochBaseline = false;

    // Zone payload GUIDs baked this frame — their store dirty rects are cleared
    // after the snapshot is recorded so the next stroke accumulates afresh.
    std::vector<GUID> m_BakedZonePayloads;

    // ---- Interactive-drag live-preview throttle ----
    // TWO editor gestures re-parameterize a modifier every frame: dragging its
    // gizmo, and dragging an Inspector control that writes one (a surface-rule
    // condition band, its feather, a rule's strength). Both signal through
    // TerrainService::IsInteractiveModifierEdit, which is the OR of its sources —
    // the scene view writes its own source every frame from the gizmo's live
    // state, so one shared flag would have that write clear the Inspector's arm.
    // An Inspector drag on a GLOBAL-scoped volume is the worst case of the two:
    // its dirty rect is every resident tile, with no footprint to bound it.
    //
    // Dragging a modifier/zone gizmo changes its geometry every frame, and the
    // region diff dirties the full old∪new footprint — an O(region × tiles)
    // re-bake (base refill + all-modifier apply + per-tile min/max rescan +
    // atlas patch + collider re-cook) that, run every frame, dropped a 2 km atlas
    // terrain to single-digit fps. Rather than defer EVERY frame to one settle bake
    // (which gave no preview for a sub-2 s drag — "delayed until movement
    // finished"), the modifier system re-bakes the affected region at a bounded
    // wall-clock cadence while the editor signals an interactive drag
    // (TerrainService::IsInteractiveModifierEdit), so the modifier's effect updates
    // live as it is placed. Each throttled preview is itself an exact region bake,
    // so the release settle stays byte-identical to a fresh full bake of the final
    // position (settle diff = last-preview bounds ∪ final bounds). Payload edits
    // (brush strokes) are never coalesced (already sub-rect-scoped, and they need
    // live feedback). Kill switch: GE_TERRAIN_MODIFIER_COALESCE=0 restores the
    // per-frame bake.
    //
    // The cadence adapts to the measured main-thread bake cost (kPreviewBudget-
    // Fraction): a cheap bake (small terrain, or a GPU-eval-skip preview that only
    // records dirty tiles) floors near kMinPreviewIntervalMs (~every other frame);
    // an expensive CPU bake widens toward kMaxPreviewIntervalMs so a large-terrain
    // drag can never collapse back to the #530 per-frame single-digit fps.
    static constexpr float32 kBasePreviewIntervalMs = 150.0f; // first preview + pre-measurement cadence
    static constexpr float32 kMinPreviewIntervalMs = 33.0f;   // floor (~every other frame @ 60 fps)
    static constexpr float32 kMaxPreviewIntervalMs = 500.0f;  // cap (an expensive bake still steps >=2x/s)
    static constexpr float32 kPreviewBudgetFraction = 3.0f;   // hold bake work to <= 1/3 of the interval it drives

    // Still owed a settle bake: a drag deferred/threw previews and the mouse was
    // released with net movement, so the release frame (change gate quiet) must
    // still run the final bake. Cleared once that settle lands.
    bool m_GeometryBakePending = false;

    // Preview-throttle running state (reset on each drag's rising edge).
    bool m_WasInteractiveDrag = false;
    bool m_HasPreviewBakeBaseline = false;
    std::chrono::steady_clock::time_point m_PreviewBakeBaseline{};
    float32 m_PreviewIntervalMs = kBasePreviewIntervalMs;
    bool m_PreviewIntervalPinned = false; // test seam pins the interval (no adaptive backoff)
    uint32 m_DragPreviewBakeCount = 0;

    // Injected monotonic clock for deterministic throttle tests (see the seams).
    bool m_HasTestClock = false;
    std::chrono::steady_clock::time_point m_TestClockNow{};

    // Monotonic clock for the preview throttle: the injected test clock when set,
    // else steady_clock::now().
    std::chrono::steady_clock::time_point PreviewNow() const
    {
        return m_HasTestClock ? m_TestClockNow : std::chrono::steady_clock::now();
    }

    // Baseline for the modifier-free base re-fill trigger: component-level
    // base inputs only (BaseSource, heightmap GUID, decoded-content version).
    // With an empty modifier list the combined hash pins to the 0 sentinel,
    // so base deltas (heightmap hot-reload evict, BaseSource/GUID edits)
    // would otherwise never bake. Seeded on the first gather WITHOUT baking
    // (startup must stay bake-free — the extraction creation fill owns the
    // initial base) and re-baselined after every bake (#405 discipline).
    uint64 m_LastBaseInputHash = 0;
    bool m_HasBaseInputBaseline = false;
};

} // namespace GameEngine::TerrainECS
