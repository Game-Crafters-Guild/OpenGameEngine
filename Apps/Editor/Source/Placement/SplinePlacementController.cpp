#include "Placement/SplinePlacementController.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplinePoolSelection.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Mathematics/Matrix4x4.h"
#include "Placement/CenterlineSampling.h"
#include "Placement/PieceEntity.h"
#include "Placement/SeamShear.h"
#include "Placement/SplineSurfaceConform.h"
#include "Placement/TileLayout.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>

namespace GameEngine::Editor
{
namespace
{

using Mathematics::Vector3;

constexpr uint32 kMaxTiles = 2048;
constexpr float32 kMinSpacing = 0.05f;

} // namespace

void SplinePlacementController::Update(ECS::World& world,
                                       Engine::Renderer::RenderServices* renderServices,
                                       float32 deltaSeconds)
{
    auto* splineService = SplineECS::SplineService::TryGet();
    if (!splineService || !renderServices)
        return;

    // A world reset destroys every generated tile with no per-entity Removed
    // event and restarts entity versions, so the cached handles now alias
    // unrelated entities — the retire sweep below would destroy those. The
    // scene-swap sites release this controller beforehand, but World::Clear is
    // also reached through World::DeserializeWorld (play-mode exit, undo
    // snapshot restore), which no swap-site hook can cover. Forgetting the
    // state is the whole release here: tiles instance already-registered model
    // meshes, so this controller owns no mesh registrations to hand back.
    const uint64 resetGeneration = world.GetLifecycleResetGeneration();
    if (resetGeneration != m_LastWorldResetGeneration)
    {
        m_LastWorldResetGeneration = resetGeneration;
        m_States.clear();
    }

    ++m_VisitStamp;

    // Copy candidates out of the query; rebuilds mutate the world.
    std::vector<Candidate> candidates;
    world.Query<ECS::Read<Components::SplineComponent>,
                ECS::Read<Components::SplinePlacement>,
                ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle e,
                  const Components::SplineComponent& spline,
                  const Components::SplinePlacement& placement,
                  const Components::WorldTransform& xf)
        {
            Candidate c;
            c.Entity = e;
            c.Recipe = placement;
            c.SplineDataIndex = spline.SplineDataIndex;
            c.SplineDataGeneration = spline.SplineDataGeneration;
            std::memcpy(c.WorldMatrix, xf.matrix, sizeof(c.WorldMatrix));
            candidates.push_back(c);
        });

    bool worldDirty = false;

    // One reading of the ground per frame, shared by every candidate that
    // conforms: it is a property of the world, not of a spline. A scene where
    // no placement recipe conforms never scans the terrains for it.
    const bool anyConforms = std::any_of(
        candidates.begin(), candidates.end(), [](const Candidate& c)
        { return c.Recipe.ConformMode != Components::SplinePlacementConform::None; });
    const uint64 surfaceRevision = anyConforms ? ConformSurfaceRevision(world, /*conforms by raycast*/ nullptr) : 0u;
    // Whether that ground is there at all yet, on the same terms and for the
    // same reason: asked once per frame, and only when something conforms.
    const ConformSurfaceReadiness surfaceReadiness =
        anyConforms ? ConformSurfaceState(world) : ConformSurfaceReadiness::NoSurface;

    for (const Candidate& candidate : candidates)
    {
        PlacementState& state = m_States[candidate.Entity.id];
        state.VisitStamp = m_VisitStamp;

        ObservedState current;
        current.Spline = ObserveSpline(*splineService, candidate.SplineDataIndex,
                                       candidate.SplineDataGeneration, candidate.WorldMatrix);
        current.Recipe = candidate.Recipe;
        // Conforming is this recipe's only ground read — it has no width fit.
        const bool conforms =
            candidate.Recipe.ConformMode != Components::SplinePlacementConform::None;
        current.SurfaceRevision = ObservedSurfaceRevision(conforms, surfaceRevision);

        if (!current.Spline.Valid || Components::ActivePoolCount(current.Recipe.StraightPool) == 0u)
        {
            worldDirty |= RetireTiles(world, state);
            state.Gate.MarkUnbuilt();
            continue;
        }

        const bool tilesAlive =
            std::all_of(state.Tiles.begin(), state.Tiles.end(),
                        [&world](ECS::EntityHandle t) { return world.IsValid(t); });
        if (!state.Gate.ShouldRebuild(current, tilesAlive, surfaceReadiness, conforms, deltaSeconds,
                                      candidate.Entity.id))
            continue;

        if (Rebuild(world, *renderServices, candidate, state))
            worldDirty = true;
        state.Gate.MarkBuilt(current);
    }

    // Retire tiles whose recipe entity no longer matches the query.
    for (auto it = m_States.begin(); it != m_States.end();)
    {
        if (it->second.VisitStamp != m_VisitStamp)
        {
            worldDirty |= RetireTiles(world, it->second);
            it = m_States.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (worldDirty)
        world.ProcessCommands();
}

void SplinePlacementController::Shutdown(ECS::World& world)
{
    bool worldDirty = false;
    for (auto& [entityId, state] : m_States)
        worldDirty |= RetireTiles(world, state);
    m_States.clear();
    if (worldDirty)
        world.ProcessCommands();
}

bool SplinePlacementController::RetireTiles(ECS::World& world, PlacementState& state)
{
    bool destroyedAny = false;
    for (ECS::EntityHandle tile : state.Tiles)
    {
        if (world.IsValid(tile))
        {
            world.DestroyEntity(tile);
            destroyedAny = true;
        }
    }
    state.Tiles.clear();
    state.TileStationIndices.clear();
    return destroyedAny;
}

bool SplinePlacementController::Rebuild(ECS::World& world,
                                        Engine::Renderer::RenderServices& renderServices,
                                        const Candidate& candidate, PlacementState& state)
{
    auto* splineService = SplineECS::SplineService::TryGet();
    const SplineECS::SplineHandle handle(candidate.SplineDataIndex,
                                         candidate.SplineDataGeneration);

    // Self-heal a stale arc-length LUT (a points writer that forgot RebuildCache).
    if (const auto* data = splineService->GetSplineData(handle); data && data->Dirty)
        splineService->RebuildCache(handle);

    const Spline::SplineData* data = splineService->GetSplineData(handle);
    if (!data || !data->IsValid() || data->TotalArcLength <= 0.0f)
        return RetireTiles(world, state);

    const Components::SplinePlacement& recipe = candidate.Recipe;
    const float32 spacing = std::max(recipe.Spacing, kMinSpacing);
    const float32 arcLength = data->TotalArcLength;

    // Resolve every active straight-pool entry, each once per GUID (the cache
    // is shared across every placement). All active entries must load: a
    // partial pool would have to substitute picks — and reshuffle the path the
    // moment the missing mesh appeared — so placement idles until the recipe
    // is whole.
    const uint32 activeStraights = Components::ActivePoolCount(recipe.StraightPool);
    if (activeStraights == 0u)
        return RetireTiles(world, state); // Update() gates on this; keep Rebuild safe alone
    const uint32 submeshIndex = 0u; // v1: first submesh; kit tiles are single-mesh
    const bool overrideRenderable =
        EnsureOverrideMaterialRegistered(renderServices, recipe.OverrideMaterial, m_OverrideMaterials);
    const Components::MaterialRef boundOverride =
        overrideRenderable ? recipe.OverrideMaterial : Components::MaterialRef{};
    struct PoolPiece
    {
        Components::LocalBounds Bounds{};
        Components::MeshRenderer Renderer{};
    };
    std::vector<PoolPiece> pieces(activeStraights);
    std::string failedGuids; // every failing entry, so no failure hides behind the first
    for (uint32 p = 0; p < activeStraights; ++p)
    {
        const GUID modelGuid = recipe.StraightPool[p].ToGuid();
        auto cacheIt = m_ModelCache.find(modelGuid);
        if (cacheIt == m_ModelCache.end())
        {
            cacheIt = m_ModelCache
                          .emplace(modelGuid, Engine::Renderer::RegisterModelRenderResources(
                                                  renderServices, modelGuid))
                          .first;
        }
        const auto& resources = cacheIt->second;
        if (!resources.has_value() || !resources->modelAsset)
        {
            if (!failedGuids.empty())
                failedGuids += ", ";
            failedGuids += modelGuid.ToString();
            continue;
        }

        PoolPiece& piece = pieces[p];
        const auto* gpuEntry =
            (submeshIndex < resources->meshHandles.size())
                ? renderServices.GetMeshGPURegistry().Find(resources->meshHandles[submeshIndex])
                : nullptr;
        if (gpuEntry)
            piece.Bounds.Box = gpuEntry->bounds;
        else
            piece.Bounds = Engine::Renderer::ComputeModelBounds(*resources->modelAsset);
        BindPieceMaterial(piece.Renderer, *resources, submeshIndex, boundOverride);
        piece.Renderer.castShadows = true;
        piece.Renderer.receiveShadows = true;
        piece.Renderer.motionVectors = false; // tiles only move on re-place
    }
    if (!failedGuids.empty())
    {
        if (!state.LoadFailureLogged)
        {
            Logger::Log::Warning(
                "SplinePlacement: straight-pool model(s) {} failed to load — placement for "
                "entity {} is idle until the mesh references are fixed",
                failedGuids, candidate.Entity.id);
            state.LoadFailureLogged = true;
        }
        return RetireTiles(world, state);
    }
    state.LoadFailureLogged = false;

    // The spline entity's world transform: SampleUniform frames are entity-local.
    const Mathematics::Matrix4x4 worldM = Mathematics::Matrix4x4::FromColumnMajor(candidate.WorldMatrix);
    const Vector3 worldOrigin = worldM.TransformPoint(Vector3(0.0f, 0.0f, 0.0f));

    // Tiles are children of this entity, so their Transform is parent-local
    // while everything below lays out in world space (Placement/PieceEntity.h).
    const Mathematics::Matrix4x4 invPlacerWorld = InvertPlacerWorld(candidate.WorldMatrix);

    // Everything runtime-only is invisible to conform rays.
    const std::vector<ECS::EntityHandle> ignore = CollectConformRayIgnoreList(world);

    // Conform misses are otherwise indistinguishable from authored altitude; tally them
    // across the whole rebuild and report once at the end.
    ConformMissTally conformMisses;

    // Layout walks one footprint for the whole family: a pool is a set of
    // interchangeable cross-sections (validated at edit time), so the first
    // entry's bounds stand in for every member.
    const Components::LocalBounds& layoutBounds = pieces[0].Bounds;

    // ---- Draped centerline ----
    // Tiles are spaced along the CONFORMED centerline, not the authored arc:
    // on sloped ground the authored curve's arc length under-counts the
    // distance the tiles actually cover (measured: 3D steps up to ~2x the
    // requested spacing on a steep crest), which opened gaps downhill. Dense
    // samples of the authored curve are transformed, laterally offset, and
    // draped by the shared conform ray; tile stations then walk this polyline
    // by true 3D length (Placement/TileLayout.h).
    //
    // The 3D walk has a known cost on near-vertical ground: arc length there is
    // almost all altitude, so a face climbing 9 m inside 0.2 m of horizontal
    // distance takes five stations at 2 m spacing and stacks five tiles on the
    // same footprint. The tilt ceiling keeps each of them upright rather than
    // edge-on, and the clamp report tells the author the ground is that steep,
    // but the pile-up itself is inherent to spacing by draped length and is not
    // fixed here — the bent-strip work is where it goes away.
    const float32 worldArcLength = WorldCenterlineLength(arcLength, worldM);
    const bool overCenterlineBudget = worldArcLength > kMaxCenterlineLengthMetres;
    if (overCenterlineBudget && !state.CenterlineBudgetLogged)
    {
        Logger::Log::Warning(
            "SplinePlacement: entity {} has a {:.0f} m centerline, past the {:.0f} m drape "
            "budget — the {:.2f} m sampling step coarsens beyond it, so the drape measures "
            "short and tile spacing there is approximate",
            candidate.Entity.id, worldArcLength, kMaxCenterlineLengthMetres,
            kCenterlineStepMetres);
    }
    state.CenterlineBudgetLogged = overCenterlineBudget;
    const uint32 denseCount = CenterlineSampleCount(worldArcLength);
    std::vector<Spline::SplineFrame> localFrames;
    Spline::SampleUniform(*data, denseCount, localFrames);
    if (localFrames.size() < 2u)
        return RetireTiles(world, state);

    const Vector3 worldUp(0.0f, 1.0f, 0.0f);
    const bool conform = recipe.ConformMode != Components::SplinePlacementConform::None;
    std::vector<CenterSample> center;
    center.reserve(localFrames.size());
    for (const Spline::SplineFrame& frame : localFrames)
    {
        Vector3 pos = worldM.TransformPoint(frame.Position);
        const Vector3 worldTangent =
            NormalizedOrFallback(worldM.TransformPoint(frame.Forward) - worldOrigin,
                                 Vector3(0.0f, 0.0f, 1.0f));
        // Ground-plane travel direction; vertical tangents fall back to +Z.
        const Vector3 forward = NormalizedOrFallback(Vector3(worldTangent.x, 0.0f, worldTangent.z),
                                                     Vector3(0.0f, 0.0f, 1.0f));
        if (recipe.LateralOffset != 0.0f)
        {
            // Cross(worldUp, forward) is the RIGHT of travel in this LH +Y-up
            // engine (+X when travelling +Z).
            const Vector3 right = NormalizedOrFallback(Vector3::Cross(worldUp, forward), Vector3(1.0f, 0.0f, 0.0f));
            pos = pos + right * recipe.LateralOffset;
        }

        CenterSample sample;
        sample.Pos = pos;
        sample.Normal = worldUp;
        if (conform)
        {
            // One ray carries position and full surface orientation: the pick
            // returns the hit triangle's normal (mesh ground) or the
            // heightfield-gradient normal (planar terrain), so tiles get pitch
            // AND lateral roll.
            Vector3 hit;
            Vector3 hitNormal;
            if (ConformRayDown(world, pos, ignore, hit, &hitNormal, &conformMisses,
                               recipe.ConformTarget))
            {
                sample.Pos.y = hit.y;
                if (recipe.ConformMode == Components::SplinePlacementConform::HeightAndSlope)
                    sample.Normal = NormalizedOrFallback(hitNormal, worldUp);
            }
            else
            {
                // No ground here, so the authored altitude survives only as a
                // fallback. Declaring the sample unmeasured keeps it out of
                // the length the stations are derived from.
                sample.SurfaceValid = false;
            }
        }
        center.push_back(sample);
    }

    TileLayoutParams layout;
    layout.Spacing = spacing;
    layout.Fit = recipe.Fit;
    layout.AlignToSurfaceNormal =
        recipe.ConformMode == Components::SplinePlacementConform::HeightAndSlope;
    layout.SlopeBlend = recipe.SlopeBlend;
    layout.MaxTiltDegrees = recipe.MaxTiltDegrees;
    layout.MeshBoundsCenter = layoutBounds.Box.center;
    layout.MeshBoundsHalfExtents = layoutBounds.Box.halfExtents;
    layout.PlantMode = recipe.PlantMode;
    layout.MaxTiles = kMaxTiles;
    layout.Seed = recipe.Seed;
    layout.SpacingJitterMetres = recipe.SpacingJitterMetres;
    layout.YawJitterDegrees = recipe.YawJitterDegrees;
    layout.LateralJitterMetres = recipe.LateralJitterMetres;
    layout.DropoutChance = recipe.DropoutChance;
    layout.EndTaperMetres = recipe.EndTaperMetres;
    TiltClampReport tiltClamp;
    layout.OutTiltClamp = &tiltClamp;
    if (conform)
    {
        // One more ray per tile, at the station the tile actually lands on:
        // the dense polyline is a chord approximation, and interpolating it
        // cuts every kerb, berm and crest that falls between two samples.
        const Components::SplineConformTarget conformTarget = recipe.ConformTarget;
        layout.Probe = [&world, &ignore, &conformMisses, conformTarget](
                           const Vector3& at, float32& outAltitude, Vector3& outNormal)
        {
            Vector3 hit;
            Vector3 hitNormal;
            if (!ConformRayDown(world, at, ignore, hit, &hitNormal, &conformMisses, conformTarget))
                return false;
            outAltitude = hit.y;
            outNormal = hitNormal;
            return true;
        };
    }
    std::vector<TilePose> poses = BuildTilePoses(center, layout);
    // Every conform ray for this rebuild has now been cast (the centerline drape plus
    // BuildTilePoses' per-station probes), so the tally is complete. Reported before the
    // empty-layout return so a spline that missed the ground entirely still says so.
    ReportConformMisses(conformMisses, candidate.Entity.id, state.LoggedConformMisses);
    ReportTiltClamp(tiltClamp, candidate.Entity.id, recipe.MaxTiltDegrees,
                    state.LoggedTiltClamps);
    if (poses.empty())
        return RetireTiles(world, state);

    // Per-station family pick — a pure function of (Seed, station index), so a
    // rebuild reproduces identical picks and an upstream knot edit leaves
    // earlier stations' meshes untouched (SplinePoolSelection.h). The key is the
    // pose's STATION ORDINAL, not its index in this vector: dropout removes
    // stations, and numbering the survivors 0..N-1 would give every tile past
    // the first gap a different mesh each time a gap opened or closed.
    std::vector<uint32> picks(poses.size());
    for (size_t i = 0; i < poses.size(); ++i)
        picks[i] = Components::SplinePoolSelect(recipe.Seed, Components::SplinePoolRole::Straight,
                                                poses[i].StationIndex, activeStraights);

    // Which local axis of the family's footprint runs along the path — the same
    // rule the fence recipe reads, so a piece is laid along the axis its extent
    // was measured on. BuildTilePoses derived it from these same bounds; the
    // shear and the emitted transform below must not answer it differently.
    const PieceAxis pieceAxis = ChoosePieceAxis(layoutBounds.Box.halfExtents);

    // Seam-shear clamp (geometry in Placement/SeamShear.h): skew each tile so
    // its across-travel edge lands on its joints' bisectors. The bisector is a
    // WORLD direction, so the shear is not signed by which way the tile is laid
    // — the axis reaches Position, through the centering term ApplySeamShear
    // carries. It gets the SAME layoutBounds center the poses were built with,
    // not the picked mesh's own. The renderer's CPU inverse-transpose normal
    // matrix absorbs the shear, so lighting stays correct.
    if (recipe.SeamShearMaxDegrees > 0.0f && poses.size() >= 2)
    {
        std::vector<Vector3> forwards;
        forwards.reserve(poses.size());
        for (const TilePose& pose : poses)
            forwards.push_back(pose.Forward);
        ApplySeamShear(poses, ComputeSeamShearFactors(forwards, recipe.SeamShearMaxDegrees),
                       layoutBounds.Box.center, pieceAxis);
    }

    // The station ordinals this rebuild produced, parallel to `poses`. They are
    // what the picks key on, so they are also what decides whether the live
    // tiles can be kept.
    std::vector<uint32> stationIndices;
    stationIndices.reserve(poses.size());
    for (const TilePose& pose : poses)
        stationIndices.push_back(pose.StationIndex);

    // Count-stable update with unchanged picks: rewrite local Transforms only —
    // the hierarchy system propagates to WorldTransform and bumps its Version.
    // Picks are pure in (Seed, station ordinal), so an unchanged pool, Seed and
    // ORDINAL SEQUENCE means every tile keeps the mesh it already renders. The
    // sequence is the load-bearing term: with dropout on, a rebuild can return
    // the same number of stations drawn from a different set, and comparing
    // counts alone would leave those tiles showing the previous set's meshes.
    const bool samePicks =
        state.Gate.AppliedOnce() &&
        std::equal(std::begin(state.Gate.Applied().Recipe.StraightPool),
                   std::end(state.Gate.Applied().Recipe.StraightPool),
                   std::begin(recipe.StraightPool)) &&
        state.Gate.Applied().Recipe.Seed == recipe.Seed &&
        state.TileStationIndices == stationIndices;
    const bool allTilesAlive = std::all_of(state.Tiles.begin(), state.Tiles.end(),
                                           [&](ECS::EntityHandle t) { return world.IsValid(t); });
    if (samePicks && allTilesAlive && state.Tiles.size() == poses.size())
    {
        for (size_t i = 0; i < poses.size(); ++i)
        {
            if (auto* t = world.GetComponentForWrite<Components::Transform>(state.Tiles[i]))
                WriteParentLocalPose(*t, invPlacerWorld, poses[i], pieceAxis, 1.0f);
        }
        return false;
    }

    RetireTiles(world, state);

    state.Tiles.reserve(poses.size());
    for (size_t i = 0; i < poses.size(); ++i)
    {
        const PoolPiece& piece = pieces[picks[i]];
        ECS::Entity tile = world.Create();
        Components::Transform transform{};
        WriteParentLocalPose(transform, invPlacerWorld, poses[i], pieceAxis, 1.0f);
        tile.Set(transform);
        tile.Set(piece.Renderer);
        tile.Set(piece.Bounds);
        tile.Set(Components::RuntimeOnlyEntity{});
        // Parenting is what keeps the hierarchy readable: the tiles collapse
        // under the placer instead of flooding the scene root.
        tile.Set(Components::Parent{candidate.Entity});
        tile.Set(MakePieceLabel("Tile", static_cast<uint32>(i)));
        state.Tiles.push_back(tile.GetHandle());
    }
    state.TileStationIndices = std::move(stationIndices);
    return true;
}

} // namespace GameEngine::Editor
