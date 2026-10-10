#include "Placement/SplineFillRebuild.h"

#include "Components/Spline/SplineExtrude.h"
#include "Logger/Logger.h"
#include "Placement/PieceEntity.h"
#include "Placement/TileLayout.h"
#include "TerrainECS/PlanarHeightQuery.h"
#include "TerrainECS/TerrainModifierSystem.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Editor
{
namespace
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Vector3;

// Lattice corners per chunk edge. At the vignette terrain's 0.39 m spacing this
// is ~50 m, which lines up with the swept path's own chunk length, so a scene
// with both gets comparable culling granularity.
constexpr uint32 kFillChunkCells = 128u;

// Ground height standing in for "outside the terrain footprint". The edge of the
// world bounds a surface exactly the way a bank does, so it reads as a very high
// wall the flood cannot cross. Inside the footprint there is no such thing as
// unreadable ground: a tiled terrain's heights are composed rather than read out
// of the streamed tiles, so every corner within the footprint has an answer
// whether or not the camera has ever been near it.
constexpr float32 kOutsideFootprintHeight = 1.0e9f;

} // namespace

WaterFillRun BuildWaterFillRun(ECS::World& world,
                               std::span<const SG::SplineStripStation> worldStations,
                               const Components::SplineExtrude& recipe,
                               const float32 placerWorldMatrix[16],
                               const TerrainECS::TerrainModifierSystem* modifierSystem)
{
    WaterFillRun run;
    if (worldStations.empty())
        return run;

    const TerrainECS::PlanarHeightQuery terrain = TerrainECS::ResolvePlanarHeightQuery(world);
    const float32 spacingX = terrain.LatticeSpacingX();
    const float32 spacingZ = terrain.LatticeSpacingZ();
    if (!terrain.IsValid() || !(spacingX > 0.0f) || !(spacingZ > 0.0f))
    {
        run.Outcome = WaterFillOutcome::NoTerrain;
        return run;
    }
    // A tiled terrain's ground is COMPOSED, not read: its tiles stream out behind
    // the camera, so the store holds no answer for a corridor the camera is not
    // near. Without the system that composes it there is no ground source at all,
    // which is the same "nothing to fill against" NoTerrain already names.
    if (terrain.Tiled && !modifierSystem)
    {
        run.Outcome = WaterFillOutcome::NoTerrain;
        return run;
    }

    const float32 reach = std::max(0.0f, recipe.MaxHalfWidth);

    // ---- Region of interest: the run's XZ bounds, inflated by the reach ----
    //
    // Snapped OUTWARD to the terrain's own lattice, so every corner of the grid
    // is a heightfield sample rather than an interpolation of one. Plus a
    // one-cell margin, which is what the field's stamp halo needs to interpolate
    // the corridor boundary instead of collapsing it onto a lattice line.
    float32 minX = worldStations.front().Position.x;
    float32 maxX = minX;
    float32 minZ = worldStations.front().Position.z;
    float32 maxZ = minZ;
    for (const SG::SplineStripStation& station : worldStations)
    {
        minX = std::min(minX, station.Position.x);
        maxX = std::max(maxX, station.Position.x);
        minZ = std::min(minZ, station.Position.z);
        maxZ = std::max(maxZ, station.Position.z);
    }
    const float32 margin = reach + std::max(spacingX, spacingZ) * 2.0f;
    const float32 originX = terrain.LatticeOriginX();
    const float32 originZ = terrain.LatticeOriginZ();
    const int32 firstX = static_cast<int32>(std::floor((minX - margin - originX) / spacingX));
    const int32 lastX = static_cast<int32>(std::ceil((maxX + margin - originX) / spacingX));
    const int32 firstZ = static_cast<int32>(std::floor((minZ - margin - originZ) / spacingZ));
    const int32 lastZ = static_cast<int32>(std::ceil((maxZ + margin - originZ) / spacingZ));

    SG::SplineGroundGrid grid;
    grid.OriginX = originX + static_cast<float32>(firstX) * spacingX;
    grid.OriginZ = originZ + static_cast<float32>(firstZ) * spacingZ;
    grid.SpacingX = spacingX;
    grid.SpacingZ = spacingZ;
    grid.CountX = static_cast<uint32>(std::max(lastX - firstX + 1, 0));
    grid.CountZ = static_cast<uint32>(std::max(lastZ - firstZ + 1, 0));
    if (grid.CountX < 2u || grid.CountZ < 2u)
    {
        run.Outcome = WaterFillOutcome::NoTerrain;
        return run;
    }
    run.WindowCorners = static_cast<uint64>(grid.CountX) * grid.CountZ;
    run.WindowSizeX = static_cast<float32>(grid.CountX - 1u) * spacingX;
    run.WindowSizeZ = static_cast<float32>(grid.CountZ - 1u) * spacingZ;
    if (run.WindowCorners > kMaxFillWindowCorners)
    {
        run.Outcome = WaterFillOutcome::SampleWindowTooLarge;
        return run;
    }

    // ---- Ground, once per corner ----
    //
    // The window snapped to the terrain's own lattice above, so firstX/firstZ ARE
    // global lattice indices and the composed block lines up corner for corner
    // with no resampling.
    std::vector<float32> composed;
    const bool composedWindow =
        terrain.Tiled &&
        modifierSystem->ComposeTiledGroundBlock(*terrain.Tiled, terrain.HeightScale,
                                                terrain.OriginY, firstX, firstZ, grid.CountX,
                                                grid.CountZ, composed);

    std::vector<float32> heights(static_cast<size_t>(grid.CountX) * grid.CountZ, 0.0f);
    for (uint32 cz = 0; cz < grid.CountZ; ++cz)
    {
        const float32 worldZ = grid.WorldZ(cz);
        for (uint32 cx = 0; cx < grid.CountX; ++cx)
        {
            const float32 worldX = grid.WorldX(cx);
            const uint32 index = grid.Index(cx, cz);
            if (!terrain.ContainsXZ(worldX, worldZ))
            {
                heights[index] = kOutsideFootprintHeight;
                continue;
            }
            if (composedWindow)
            {
                heights[index] = terrain.OriginY + composed[index] * terrain.HeightScale;
                continue;
            }
            // Single terrain: one heightfield over the whole footprint, always
            // resident. SampleHeight rejects the same rectangle ContainsXZ just
            // accepted, so a failure here is a boundary corner an ulp outside it —
            // the edge of the world, which bounds the surface like a bank.
            if (!terrain.SampleHeight(worldX, worldZ, heights[index]))
                heights[index] = kOutsideFootprintHeight;
        }
    }
    grid.Heights = heights;

    SG::SplineFillParams params;
    params.MaxHalfWidth = reach;
    params.EdgeDrop = std::max(0.0f, recipe.EdgeDrop);
    params.SeaLevelFloor = recipe.SeaLevelFloor;
    // MaxWetCorners is left at the field's own default ON PURPOSE. Passing the
    // WINDOW budget here aliases the two guards, and since wet corners are a
    // subset of the window the water guard could then never fire.

    const SG::SplineFillResult field = SG::BuildSplineFillField(worldStations, grid, params);
    run.Diagnostics = field.Diagnostics;

    if (field.Diagnostics.ExceededBudget)
    {
        run.Outcome = WaterFillOutcome::OverBudget;
        return run;
    }
    if (!field.HasRegion())
    {
        run.Outcome = WaterFillOutcome::NoRegion;
        return run;
    }

    // ---- Mesh, then bring the vertices into the placer's local space ----
    SG::SplineFillMeshParams meshParams;
    meshParams.EdgeDrop = params.EdgeDrop;
    // uv0 is (arc along the flow, still-water depth) in WORLD metres, stamped by
    // the mesher before the vertices move into the placer's local space below.
    // The loop therefore transforms Position, Normal and Tangent and leaves uv0
    // alone: a placer scale must not rescale a depth the shader reads in metres.

    const Mathematics::Matrix4x4 invPlacerWorld = InvertPlacerWorld(placerWorldMatrix);
    const Vector3 invOrigin = invPlacerWorld.TransformPoint(Vector3(0.0f, 0.0f, 0.0f));
    // Directions map through the inverse world matrix and are normalized
    // INDIVIDUALLY, the same treatment — and the same accepted degradation under
    // a non-uniform placer scale — the swept path documents for its frames.
    const auto toLocalDirection = [&](const Vector3& worldDirection)
    {
        return NormalizedOrFallback(invPlacerWorld.TransformPoint(worldDirection) - invOrigin,
                                    Vector3(0.0f, 1.0f, 0.0f));
    };

    const std::vector<SG::SplineFillChunkRange> ranges = SG::CarveFillChunks(grid, kFillChunkCells);
    run.Chunks.reserve(ranges.size());
    for (const SG::SplineFillChunkRange& range : ranges)
    {
        SG::SplineFillMesh mesh =
            SG::BuildSplineFillMesh(field, grid, worldStations, range, meshParams);
        if (mesh.IsValid())
        {
            bool first = true;
            Vector3 minBounds{};
            Vector3 maxBounds{};
            for (SG::SplineVertex& vertex : mesh.Vertices)
            {
                vertex.Position = invPlacerWorld.TransformPoint(vertex.Position);
                vertex.Normal = toLocalDirection(vertex.Normal);
                const Vector3 tangent =
                    toLocalDirection(Vector3(vertex.Tangent.x, vertex.Tangent.y, vertex.Tangent.z));
                vertex.Tangent = Mathematics::Vector4{tangent.x, tangent.y, tangent.z,
                                                      vertex.Tangent.w};
                if (first)
                {
                    minBounds = vertex.Position;
                    maxBounds = vertex.Position;
                    first = false;
                }
                else
                {
                    minBounds.x = std::min(minBounds.x, vertex.Position.x);
                    minBounds.y = std::min(minBounds.y, vertex.Position.y);
                    minBounds.z = std::min(minBounds.z, vertex.Position.z);
                    maxBounds.x = std::max(maxBounds.x, vertex.Position.x);
                    maxBounds.y = std::max(maxBounds.y, vertex.Position.y);
                    maxBounds.z = std::max(maxBounds.z, vertex.Position.z);
                }
            }
            // Bounds are recomputed AFTER the transform: the mesher's are world
            // space, and these feed a local-space LocalBounds.
            mesh.MinBounds = minBounds;
            mesh.MaxBounds = maxBounds;
        }
        run.Chunks.push_back(std::move(mesh));
    }

    run.Outcome = WaterFillOutcome::Built;
    return run;
}

void ReportWaterFillDiagnostics(const WaterFillRun& run, uint32 entityId,
                                SG::SplineFillDiagnostics& lastReported,
                                WaterFillOutcome& lastOutcome)
{
    const SG::SplineFillDiagnostics& d = run.Diagnostics;
    const bool changed = lastOutcome != run.Outcome || lastReported.DrySeeds != d.DrySeeds ||
                         lastReported.OverBankCorners != d.OverBankCorners ||
                         lastReported.UnseenBankCorners != d.UnseenBankCorners ||
                         lastReported.WetCorners != d.WetCorners ||
                         lastReported.SealedPocketCorners != d.SealedPocketCorners;
    if (!changed)
        return;
    lastReported = d;
    lastOutcome = run.Outcome;

    switch (run.Outcome)
    {
    case WaterFillOutcome::NoTerrain:
        Logger::Log::Warning(
            "SplineExtrude: entity {} fits to banks but there is no planar terrain under it to "
            "fill against. The fill reads the heightfield directly, so a mesh-built channel is "
            "not seen: put the run over terrain, or switch WidthMode to Channel and author a "
            "width.",
            entityId);
        return;
    case WaterFillOutcome::SampleWindowTooLarge:
        // Deliberately NOT the OverBudget message. This refusal happens before a
        // single height is read, so it says nothing about the water — and the
        // knobs that answer OverBudget cannot move it: the reach enters only
        // through the window's margin. Naming them here would send an author to
        // shrink a river that was never too wide, only too long.
        Logger::Log::Warning(
            "SplineExtrude: entity {}'s fill window spans {:.0f} x {:.0f} m (run extent plus the "
            "width margin), so the water fill would have to "
            "read {} ground samples in one window against a budget of {} — it built NOTHING "
            "rather than a coarser surface. Max Half Width does not move this; the run's own "
            "extent does. Split the run into shorter spline entities.",
            entityId, run.WindowSizeX, run.WindowSizeZ, run.WindowCorners,
            kMaxFillWindowCorners);
        return;
    case WaterFillOutcome::OverBudget:
        Logger::Log::Warning(
            "SplineExtrude: entity {} flooded more than the water fill's cell budget, so it "
            "built NOTHING rather than a coarser surface. Reduce Max Half Width — it is the "
            "bound on how far the water may spread — or raise the bed.",
            entityId);
        return;
    case WaterFillOutcome::NoRegion:
        // Two different failures reach this outcome and they take OPPOSITE
        // fixes, so they must not share a message: a bed ABOVE the water (raise
        // the water), and a bed below it with nothing anywhere to hold the water
        // in (raise the banks, or the reach that would let the fill see them).
        if (d.OverBankCorners > 0u && d.DrySeeds < d.Seeds)
            Logger::Log::Warning(
                "SplineExtrude: entity {} found no water it could hold: the bed IS below the "
                "waterline, but everywhere it is, the water would flow away over ground lower "
                "than itself — worst overshoot {:.2f} m, and {} sample(s) never saw a bank at "
                "all inside Max Half Width. Give the channel banks, or raise Max Half Width "
                "until the real ones are inside it.",
                entityId, d.MaxOverBankMetres, d.UnseenBankCorners);
        else
            Logger::Log::Warning(
                "SplineExtrude: entity {} found no water to fill: the bed is not below the "
                "waterline at any of its {} station(s). Carve the channel deeper, or raise "
                "Vertical Offset so the surface sits above the bed.",
                entityId, d.Seeds);
        return;
    case WaterFillOutcome::Built:
        break;
    }

    if (d.DrySeeds > 0u)
    {
        Logger::Log::Warning(
            "SplineExtrude: entity {} has bed at or above the waterline at {} of {} stations, "
            "first at ({:.2f}, {:.2f}, {:.2f}) — the water does not reach there. Carve deeper "
            "along that stretch, or raise Vertical Offset.",
            entityId, d.DrySeeds, d.Seeds, d.FirstDrySeed.x, d.FirstDrySeed.y, d.FirstDrySeed.z);
    }
    if (d.OverBankCorners > 0u)
    {
        Logger::Log::Warning(
            "SplineExtrude: entity {} asked for water standing over its own banks at {} "
            "sample(s), worst {:.2f} m at ({:.2f}, {:.2f}) — {:.1f} m along the run — so the "
            "surface stops at the bank instead: water does not stand over ground it could flow "
            "across. Carve the channel deeper there, or lower Vertical Offset.",
            entityId, d.OverBankCorners, d.MaxOverBankMetres, d.WorstOverBank.X,
            d.WorstOverBank.Z, d.WorstOverBank.ArcDistance);
    }
    if (d.UnseenBankCorners > 0u)
    {
        Logger::Log::Warning(
            "SplineExtrude: entity {} could not see a bank at {} sample(s), worst at "
            "({:.2f}, {:.2f}) — {:.1f} m along the run: the water's way out ran past Max Half "
            "Width, so the fill has no evidence anything holds it there and left it out. Raise "
            "Max Half Width until the banks are inside it.",
            entityId, d.UnseenBankCorners, d.WorstUnseenBank.X, d.WorstUnseenBank.Z,
            d.WorstUnseenBank.ArcDistance);
    }
    if (d.WaterlineRiseMetres > SG::kWaterlineRiseToleranceMetres)
    {
        Logger::Log::Warning(
            "SplineExtrude: entity {} has a waterline that RISES {:.2f} m going downstream, at "
            "{:.1f} m along the run. Water does not flow uphill: check the centreline's "
            "altitudes there. The surface is built as authored rather than clamped.",
            entityId, d.WaterlineRiseMetres, d.WaterlineRiseAtArc);
    }
    if (d.SealedPocketCorners > 0u)
    {
        Logger::Log::Debug(
            "SplineExtrude: entity {} enclosed {} dry sample(s) on every side and filled them — "
            "the banks dip below the waterline there, but water cannot drain through water. The "
            "same dip at the region's edge would still take a bite out of it.",
            entityId, d.SealedPocketCorners);
    }
    if (d.MedialStepMetres > 0.25f)
    {
        Logger::Log::Debug(
            "SplineExtrude: entity {} filled one pool that its run enters twice; the surface "
            "steps {:.2f} m down the middle, which is the along-run drop between the two legs.",
            entityId, d.MedialStepMetres);
    }
}

} // namespace GameEngine::Editor
