#include "Placement/SplineFenceController.h"

#include "Assets/ModelAsset.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
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
#include "Placement/FenceEmission.h"
#include "Placement/FenceLayout.h"
#include "Placement/FencePieceReuse.h"
#include "Placement/FenceRecipeLayout.h"
#include "Placement/FenceSpanOverrideRemap.h"
#include "Placement/PieceEntity.h"
#include "Placement/RecipeValidation.h"
#include "Placement/SplineSurfaceConform.h"
#include "Placement/TileLayout.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"
#include "SplineLayout/SpanMitre.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace GameEngine::Editor
{
namespace
{

using Mathematics::Vector3;

// Posts, spans, crests and caps together. A fence is one piece per station, one
// per span and, with a crest row, about one per crest cell, so the budget holds
// roughly three times a tile strip's pieces at the same pitch.
// One budget for every role, and the layout emits the crest row last, so a long
// wall cuts its decoration before its structure.
constexpr uint32 kMaxFencePieces = 4096;

Rendering::MeshGPURegistry* MeshRegistryOf(Engine::Renderer::RenderServices* renderServices)
{
    return renderServices ? &renderServices->GetMeshGPURegistry() : nullptr;
}

// What one pool offers the variant cutter, slot for slot.
std::vector<FenceMitreSource> MitreSources(const Rendering::MeshGPURegistry& registry,
                                           const Components::ModelRef* pool,
                                           const std::vector<FencePoolPiece>& pieces,
                                           const std::vector<FencePieceBounds>& bounds)
{
    std::vector<FenceMitreSource> sources(pieces.size());
    const Rendering::MeshGPURegistry::TableScope scope(registry);
    for (size_t i = 0; i < pieces.size(); ++i)
    {
        sources[i].Model = pool[i].ToGuid();
        sources[i].SourceMesh = pieces[i].SourceMesh;
        const Rendering::MeshGPUEntry* entry = registry.Find(
            Rendering::MeshGPUHandle(pieces[i].Renderer.meshGpuHandleId));
        sources[i].Content = entry ? entry->contentHash : 0ull;
        sources[i].Name = pieces[i].SourceMesh ? pieces[i].SourceMesh->Name : "";
        sources[i].Bounds = bounds[i];
    }
    return sources;
}

// Whether any override makes a span a gate. The gate pool is resolved only
// then: a gate model that fails to load idles the whole fence, which a pool no
// span draws from must not do.
bool AnyGateOverride(const Components::SplineFence& recipe)
{
    return std::any_of(std::begin(recipe.Overrides), std::end(recipe.Overrides),
                       [](const Components::SplineSpanOverride& entry)
                       { return entry.Kind == Components::SplineSpanOverrideKind::Gate; });
}

// FencePoolContentDigest of the pools a recipe cuts variants from. The gate
// pool counts only while some span is a gate, as it is resolved only then.
uint64 PoolContentOf(const Rendering::MeshGPURegistry& registry,
                     const Components::SplineFence& recipe)
{
    std::vector<GUID> models;
    for (uint32 i = 0; i < Components::ActivePoolCount(recipe.SpanPool); ++i)
        models.push_back(recipe.SpanPool[i].ToGuid());
    for (uint32 i = 0; i < Components::ActivePoolCount(recipe.CrestPool); ++i)
        models.push_back(recipe.CrestPool[i].ToGuid());
    if (AnyGateOverride(recipe))
    {
        for (uint32 i = 0; i < Components::ActivePoolCount(recipe.GatePool); ++i)
            models.push_back(recipe.GatePool[i].ToGuid());
    }
    return FencePoolContentDigest(registry, models);
}

// Which variant each piece draws, with the ones that could not be cut (no
// mesh to cut) handed back to their pool piece.
std::vector<uint32> DrawnVariants(const std::vector<uint32>& planned,
                                  const FenceMitreVariants& variants)
{
    std::vector<uint32> drawn(planned);
    for (uint32& variant : drawn)
    {
        if (variant != SplineLayout::kNoMitreVariant && !variants.Meshes[variant].Handle.IsValid())
            variant = SplineLayout::kNoMitreVariant;
    }
    return drawn;
}

// The pool piece a cut variant was made from: the one its role and slot name.
const FencePoolPiece& PoolPieceOfVariant(const SplineLayout::MitreVariantKey& key,
                                         const std::vector<FencePoolPiece>& spans,
                                         const std::vector<FencePoolPiece>& crests,
                                         const std::vector<FencePoolPiece>& gates)
{
    switch (key.Role)
    {
    case SplineLayout::MitrePieceRole::Crest:
        return crests[key.PoolSlot];
    case SplineLayout::MitrePieceRole::Gate:
        return gates[key.PoolSlot];
    case SplineLayout::MitrePieceRole::Span:
    default:
        return spans[key.PoolSlot];
    }
}

// Resolves one pool's active entries to render resources and bounds. All active
// entries must load: a partial pool would have to substitute picks — and
// reshuffle the fence the moment the missing mesh appeared — so the recipe
// idles until it is whole.
bool ResolvePool(Engine::Renderer::RenderServices& renderServices,
                 Engine::Renderer::ModelResolveCache& cache,
                 const Components::ModelRef* pool, uint32 activeCount, const char* roleName,
                 uint32 entityId, const Components::MaterialRef& overrideMaterial,
                 bool& loadFailureLogged, std::vector<FencePoolPiece>& outPieces)
{
    constexpr uint32 kSubmeshIndex = 0u; // v1: first submesh; kit pieces are single-mesh
    outPieces.assign(activeCount, FencePoolPiece{});
    for (uint32 p = 0; p < activeCount; ++p)
    {
        const GUID modelGuid = pool[p].ToGuid();
        auto cacheIt = cache.find(modelGuid);
        if (cacheIt == cache.end())
        {
            cacheIt = cache.emplace(modelGuid, Engine::Renderer::RegisterModelRenderResources(
                                                   renderServices, modelGuid))
                          .first;
        }
        const auto& resources = cacheIt->second;
        if (!resources.has_value() || !resources->modelAsset)
        {
            if (!loadFailureLogged)
            {
                Logger::Log::Warning(
                    "SplineFence: {}-pool model {} failed to load — the fence on entity {} is "
                    "idle until the mesh reference is fixed",
                    roleName, modelGuid.ToString(), entityId);
                loadFailureLogged = true;
            }
            return false;
        }

        FencePoolPiece& piece = outPieces[p];
        const auto& meshes = resources->modelAsset->GetMeshes();
        piece.SourceMesh = kSubmeshIndex < meshes.size() ? &meshes[kSubmeshIndex] : nullptr;
        const auto* gpuEntry =
            (kSubmeshIndex < resources->meshHandles.size())
                ? renderServices.GetMeshGPURegistry().Find(resources->meshHandles[kSubmeshIndex])
                : nullptr;
        if (gpuEntry)
            piece.Bounds.Box = gpuEntry->bounds;
        else
            piece.Bounds = Engine::Renderer::ComputeModelBounds(*resources->modelAsset);
        BindPieceMaterial(piece.Renderer, *resources, kSubmeshIndex, overrideMaterial);
        piece.Renderer.castShadows = true;
        piece.Renderer.receiveShadows = true;
        piece.Renderer.motionVectors = false; // pieces only move on re-place
    }
    return true;
}

// The bounds one resolved pool presents to the layout, in slot order. Every
// role whose members may differ in length is measured per slot: the span fill,
// the crest pitch floor and the cap fit each answer to the piece they picked,
// not to the family.
std::vector<FencePieceBounds> PoolBounds(const std::vector<FencePoolPiece>& pieces)
{
    std::vector<FencePieceBounds> bounds(pieces.size());
    for (size_t i = 0; i < pieces.size(); ++i)
    {
        bounds[i].Center = pieces[i].Bounds.Box.center;
        bounds[i].HalfExtents = pieces[i].Bounds.Box.halfExtents;
    }
    return bounds;
}

} // namespace

SplineFenceController::SplineFenceController()
    : m_PointEditSubscription(SplinePointEdited().Subscribe(
          [this](const SplinePointEdit& edit)
          {
              const auto state = m_States.find(edit.Entity.id);
              RemapFenceOverridesOnPointEdit(
                  edit, state != m_States.end() ? std::span<const uint32>(state->second.RunSpanCounts)
                                                : std::span<const uint32>());
          }))
{
}

void SplineFenceController::Update(ECS::World& world,
                                   Engine::Renderer::RenderServices* renderServices,
                                   float32 deltaSeconds)
{
    auto* splineService = SplineECS::SplineService::TryGet();
    if (!splineService || !renderServices)
        return;

    // A world reset destroys every generated piece with no per-entity Removed
    // event and restarts entity versions, so the cached handles now alias
    // unrelated entities — the retire sweep below would destroy those. The
    // scene-swap sites release this controller beforehand, but World::Clear is
    // also reached through World::DeserializeWorld (play-mode exit, undo
    // snapshot restore), which no swap-site hook can cover. Forgetting the
    // state is the release for the pieces, which instance already-registered
    // model meshes. The cut span variants are this controller's own
    // registrations and not handle-based, so those are released here.
    const uint64 resetGeneration = world.GetLifecycleResetGeneration();
    if (resetGeneration != m_LastWorldResetGeneration)
    {
        m_LastWorldResetGeneration = resetGeneration;
        for (auto& [entityId, state] : m_States)
            state.MitreVariants.DropAfterWorldReset(&renderServices->GetMeshGPURegistry());
        m_States.clear();
    }

    ++m_VisitStamp;

    Rendering::MeshGPURegistry& meshRegistry = renderServices->GetMeshGPURegistry();
    if (m_SubscribedRegistry != &meshRegistry)
    {
        m_MeshReloadSubscription = meshRegistry.SubscribeReload(
            [this](const GUID&) { m_MeshReloads.fetch_add(1u, std::memory_order_relaxed); });
        m_SubscribedRegistry = &meshRegistry;
        m_MeshReloads.fetch_add(1u, std::memory_order_relaxed);
    }
    const uint64 meshReloads = m_MeshReloads.load(std::memory_order_relaxed);

    // Copy candidates out of the query; rebuilds mutate the world.
    std::vector<Candidate> candidates;
    world.Query<ECS::Read<Components::SplineComponent>,
                ECS::Read<Components::SplineFence>,
                ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle e,
                  const Components::SplineComponent& spline,
                  const Components::SplineFence& fence,
                  const Components::WorldTransform& xf)
        {
            Candidate c;
            c.Entity = e;
            // Sanitize once, here: everything downstream — the settle
            // comparison included — sees only finite floats, so a poisoned
            // field can never make a recipe unequal to itself.
            c.Recipe = SanitizeFenceRecipe(fence);
            c.SplineDataIndex = spline.SplineDataIndex;
            c.SplineDataGeneration = spline.SplineDataGeneration;
            std::memcpy(c.WorldMatrix, xf.matrix, sizeof(c.WorldMatrix));
            candidates.push_back(c);
        });

    bool worldDirty = false;

    // One reading of the ground per frame, shared by every candidate that
    // conforms: it is a property of the world, not of a spline. A scene where
    // no fence recipe conforms never scans the terrains for it.
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
        FenceState& state = m_States[candidate.Entity.id];
        state.VisitStamp = m_VisitStamp;

        ObservedState current;
        current.Spline = ObserveSpline(*splineService, candidate.SplineDataIndex,
                                       candidate.SplineDataGeneration, candidate.WorldMatrix);
        current.Recipe = candidate.Recipe;
        // Conforming is this recipe's only ground read — it has no width fit.
        const bool conforms =
            candidate.Recipe.ConformMode != Components::SplinePlacementConform::None;
        current.SurfaceRevision = ObservedSurfaceRevision(conforms, surfaceRevision);
        if (state.PoolContentReloads != meshReloads)
        {
            state.PoolContent = PoolContentOf(meshRegistry, current.Recipe);
            state.PoolContentReloads = meshReloads;
        }
        current.PoolContent = state.PoolContent;

        // A fence with no post mesh is first-class (the measured kits build the
        // post into each panel's ends), so the idle gate is "no pieces at all".
        const uint32 activePosts = Components::ActivePoolCount(current.Recipe.PostPool);
        const uint32 activeSpans = Components::ActivePoolCount(current.Recipe.SpanPool);
        if (!current.Spline.Valid || (activePosts == 0u && activeSpans == 0u))
        {
            worldDirty |= RetireAll(world, renderServices, state);
            state.Gate.MarkUnbuilt();
            continue;
        }

        const bool piecesAlive =
            std::all_of(state.Pieces.begin(), state.Pieces.end(),
                        [&world](const FencePieceEntity& p) { return world.IsValid(p.Entity); });
        if (!state.Gate.ShouldRebuild(current, piecesAlive, surfaceReadiness, conforms, deltaSeconds,
                                      candidate.Entity.id))
            continue;

        if (Rebuild(world, *renderServices, candidate, state))
            worldDirty = true;
        // The rebuild registered its pools and released variants, which moves
        // the digest without any content changing: take it as built.
        state.PoolContent = PoolContentOf(meshRegistry, current.Recipe);
        state.PoolContentReloads = m_MeshReloads.load(std::memory_order_relaxed);
        current.PoolContent = state.PoolContent;
        state.Gate.MarkBuilt(current);
    }

    // Retire pieces whose recipe entity no longer matches the query.
    for (auto it = m_States.begin(); it != m_States.end();)
    {
        if (it->second.VisitStamp != m_VisitStamp)
        {
            worldDirty |= RetireAll(world, renderServices, it->second);
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

void SplineFenceController::Shutdown(ECS::World& world,
                                     Engine::Renderer::RenderServices* renderServices)
{
    bool worldDirty = false;
    for (auto& [entityId, state] : m_States)
        worldDirty |= RetireAll(world, renderServices, state);
    m_States.clear();
    if (worldDirty)
        world.ProcessCommands();
}

bool SplineFenceController::RetirePieces(ECS::World& world, FenceState& state)
{
    bool destroyedAny = false;
    for (const FencePieceEntity& piece : state.Pieces)
    {
        if (world.IsValid(piece.Entity))
        {
            world.DestroyEntity(piece.Entity);
            destroyedAny = true;
        }
    }
    state.Pieces.clear();
    return destroyedAny;
}

bool SplineFenceController::RetireAll(ECS::World& world,
                                      Engine::Renderer::RenderServices* renderServices,
                                      FenceState& state)
{
    const bool destroyedAny = RetirePieces(world, state);
    state.MitreVariants.Retire(world, MeshRegistryOf(renderServices));
    state.MitreCuts.clear();
    return destroyedAny;
}

bool SplineFenceController::Rebuild(ECS::World& world,
                                    Engine::Renderer::RenderServices& renderServices,
                                    const Candidate& candidate, FenceState& state)
{
    auto* splineService = SplineECS::SplineService::TryGet();
    const SplineECS::SplineHandle handle(candidate.SplineDataIndex,
                                         candidate.SplineDataGeneration);

    // Self-heal a stale arc-length LUT (a points writer that forgot RebuildCache).
    if (const auto* stale = splineService->GetSplineData(handle); stale && stale->Dirty)
        splineService->RebuildCache(handle);

    const Spline::SplineData* data = splineService->GetSplineData(handle);
    if (!data || !data->IsValid() || data->TotalArcLength <= 0.0f)
        return RetireAll(world, &renderServices, state);

    const Components::SplineFence& recipe = candidate.Recipe;
    const uint32 activePosts = Components::ActivePoolCount(recipe.PostPool);
    const uint32 activeSpans = Components::ActivePoolCount(recipe.SpanPool);
    if (activePosts == 0u && activeSpans == 0u)
        return RetireAll(world, &renderServices, state); // Update() gates on this; keep Rebuild safe alone
    // Resolved even when the span pool is empty: the layout owns the report
    // that a crest row has no span tops to stand on, and it only speaks when
    // it is handed the crest pieces.
    const uint32 activeCrests = Components::ActivePoolCount(recipe.CrestPool);
    const uint32 activeCaps =
        activeCrests > 0u ? Components::ActivePoolCount(recipe.CapPool) : 0u;
    const uint32 activeGates =
        AnyGateOverride(recipe) ? Components::ActivePoolCount(recipe.GatePool) : 0u;

    std::vector<FencePoolPiece> postPieces;
    std::vector<FencePoolPiece> spanPieces;
    std::vector<FencePoolPiece> crestPieces;
    std::vector<FencePoolPiece> capPieces;
    std::vector<FencePoolPiece> gatePieces;
    const bool overrideRenderable =
        EnsureOverrideMaterialRegistered(renderServices, recipe.OverrideMaterial, m_OverrideMaterials);
    const Components::MaterialRef boundOverride =
        overrideRenderable ? recipe.OverrideMaterial : Components::MaterialRef{};
    if (!ResolvePool(renderServices, m_ModelCache, recipe.PostPool, activePosts, "post",
                     candidate.Entity.id, boundOverride, state.LoadFailureLogged,
                     postPieces) ||
        !ResolvePool(renderServices, m_ModelCache, recipe.SpanPool, activeSpans, "span",
                     candidate.Entity.id, boundOverride, state.LoadFailureLogged,
                     spanPieces) ||
        !ResolvePool(renderServices, m_ModelCache, recipe.CrestPool, activeCrests, "crest",
                     candidate.Entity.id, boundOverride, state.LoadFailureLogged,
                     crestPieces) ||
        !ResolvePool(renderServices, m_ModelCache, recipe.CapPool, activeCaps, "cap",
                     candidate.Entity.id, boundOverride, state.LoadFailureLogged,
                     capPieces) ||
        !ResolvePool(renderServices, m_ModelCache, recipe.GatePool, activeGates, "gate",
                     candidate.Entity.id, boundOverride, state.LoadFailureLogged,
                     gatePieces))
    {
        return RetireAll(world, &renderServices, state);
    }
    state.LoadFailureLogged = false;

    // The spline entity's world transform: SampleUniform frames are entity-local.
    const Mathematics::Matrix4x4 worldM = Mathematics::Matrix4x4::FromColumnMajor(candidate.WorldMatrix);

    // Pieces are children of this entity, so their Transform is parent-local
    // while everything below lays out in world space (Placement/PieceEntity.h).
    const Mathematics::Matrix4x4 invPlacerWorld = InvertPlacerWorld(candidate.WorldMatrix);

    // Everything runtime-only is invisible to conform rays.
    const std::vector<ECS::EntityHandle> ignore = CollectConformRayIgnoreList(world);

    // Conform misses are otherwise indistinguishable from authored altitude; tally them
    // across the whole rebuild and report once at the end.
    ConformMissTally conformMisses;

    // ---- Draped centerline (shared with the tile recipe) ----
    const float32 worldArcLength = WorldCenterlineLength(data->TotalArcLength, worldM);
    const bool overCenterlineBudget = worldArcLength > kMaxCenterlineLengthMetres;
    if (overCenterlineBudget && !state.CenterlineBudgetLogged)
    {
        Logger::Log::Warning(
            "SplineFence: entity {} has a {:.0f} m centerline, past the {:.0f} m drape budget — "
            "the {:.2f} m sampling step coarsens beyond it, so the drape measures short and "
            "station spacing there is approximate",
            candidate.Entity.id, worldArcLength, kMaxCenterlineLengthMetres, kCenterlineStepMetres);
    }
    state.CenterlineBudgetLogged = overCenterlineBudget;

    const uint32 denseCount = CenterlineSampleCount(worldArcLength);
    std::vector<Spline::SplineFrame> localFrames;
    Spline::SampleUniform(*data, denseCount, localFrames);
    if (localFrames.size() < 2u)
        return RetireAll(world, &renderServices, state);

    const Vector3 worldUp(0.0f, 1.0f, 0.0f);
    const bool conform = recipe.ConformMode != Components::SplinePlacementConform::None;
    std::vector<CenterSample> center;
    center.reserve(localFrames.size());
    for (const Spline::SplineFrame& frame : localFrames)
    {
        CenterSample sample;
        sample.Pos = worldM.TransformPoint(frame.Position);
        sample.Normal = worldUp;
        if (conform)
        {
            Vector3 hit;
            Vector3 hitNormal;
            if (ConformRayDown(world, sample.Pos, ignore, hit, &hitNormal, &conformMisses,
                               recipe.ConformTarget))
            {
                sample.Pos.y = hit.y;
                if (recipe.ConformMode == Components::SplinePlacementConform::HeightAndSlope)
                    sample.Normal = NormalizedOrFallback(hitNormal, worldUp);
            }
            else
            {
                // No ground here, so the authored altitude survives only as a
                // fallback. Declaring the sample unmeasured keeps it out of the
                // length the stations are derived from.
                sample.SurfaceValid = false;
            }
        }
        center.push_back(sample);
    }

    // ---- Run boundaries: every authored point is a mandatory station ----
    // SampleUniform spaces its frames evenly in LOCAL arc length, so an
    // authored point's arc length maps straight onto a fractional sample index.
    // A closed spline's parameterisation already covers the last->first
    // segment, so the wrap run needs no special case beyond the extra boundary.
    const uint32 segmentCount = data->GetSegmentCount();
    if (segmentCount == 0u)
        return RetireAll(world, &renderServices, state);
    const float32 lastIndex = static_cast<float32>(localFrames.size() - 1u);
    const float32 indexScale = lastIndex / data->TotalArcLength;
    std::vector<float32> runBoundaries;
    runBoundaries.reserve(segmentCount + 1u);
    for (uint32 i = 0; i <= segmentCount; ++i)
    {
        const float32 t = static_cast<float32>(i) / static_cast<float32>(segmentCount);
        const float32 distance = Spline::ParametricToDistance(*data, t);
        runBoundaries.push_back(std::clamp(distance * indexScale, 0.0f, lastIndex));
    }

    const std::vector<FencePieceBounds> postBounds = PoolBounds(postPieces);
    const std::vector<FencePieceBounds> spanBounds = PoolBounds(spanPieces);
    const std::vector<FencePieceBounds> crestBounds = PoolBounds(crestPieces);
    const std::vector<FencePieceBounds> capBounds = PoolBounds(capPieces);
    const std::vector<FencePieceBounds> gateBounds = PoolBounds(gatePieces);
    FenceRecipePieces recipePieces;
    recipePieces.PostFootprint = postBounds.empty() ? nullptr : &postBounds.front();
    recipePieces.Spans = spanBounds;
    recipePieces.Gates = gateBounds;
    recipePieces.Crests = crestBounds;
    recipePieces.Caps = capBounds;
    FenceLayoutParams layout = FenceLayoutParamsOf(recipe, recipePieces);
    layout.RunBoundaries = std::span<const float32>(runBoundaries);
    layout.Closed = data->IsEffectivelyClosed();
    layout.MaxPieces = kMaxFencePieces;

    if (conform)
    {
        // The station probe must land on the same kind of surface the drape
        // measured, or the run's length and its stations answer to different
        // grounds.
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

    TiltClampReport tiltClamp;
    layout.OutTiltClamp = &tiltClamp;

    const FenceLayoutResult built = BuildFenceLayout(center, layout);
    state.RunSpanCounts = built.RunSpanCounts;
    // Every conform ray for this rebuild has now been cast (the centerline drape plus
    // BuildFenceLayout's per-station probes), so the tally is complete.
    ReportConformMisses(conformMisses, candidate.Entity.id, state.LoggedConformMisses);
    ReportTiltClamp(tiltClamp, candidate.Entity.id, layout.MaxTiltDegrees, state.LoggedTiltClamps);
    ReportSpanRollBorrowed(built.RollBorrowedSpans, candidate.Entity.id,
                           state.LoggedRollBorrowedSpans);
    // One report per rebuild, so the latch compares like with like: the layout's
    // lines, and on a fence that builds, the mitre variants' lines after them.
    const auto* entityName = world.GetComponent<Components::Name>(candidate.Entity);
    const std::string_view name =
        entityName ? entityName->View() : std::string_view();
    if (built.Stations.empty() && built.Spans.empty())
    {
        ReportRecipeValidation(built.Validation, name, candidate.Entity.id, state.LoggedValidation);
        return RetireAll(world, &renderServices, state);
    }

    // ---- Cut variants at the bare joins (fence design section 4c) ----
    const SplineLayout::MitreVariantPlan plan = SplineLayout::PlanMitreVariants(built);
    const Rendering::MeshGPURegistry& meshRegistry = renderServices.GetMeshGPURegistry();
    const std::vector<FenceMitreSource> spanSources =
        MitreSources(meshRegistry, recipe.SpanPool, spanPieces, spanBounds);
    const std::vector<FenceMitreSource> crestSources =
        MitreSources(meshRegistry, recipe.CrestPool, crestPieces, crestBounds);
    const std::vector<FenceMitreSource> gateSources =
        MitreSources(meshRegistry, recipe.GatePool, gatePieces, gateBounds);
    const FenceMitreVariants variants = CommitFenceMitreVariants(
        renderServices.GetMeshGPURegistry(), state.MitreVariants, state.MitreCuts,
        candidate.Entity.id, plan, {spanSources, crestSources, gateSources});
    std::vector<FencePoolPiece> variantPieces(variants.Meshes.size());
    for (size_t v = 0; v < variants.Meshes.size(); ++v)
        variantPieces[v] = MitreVariantPiece(
            PoolPieceOfVariant(plan.Variants[v], spanPieces, crestPieces, gatePieces),
            variants.Meshes[v]);
    const std::vector<uint32> spanVariants = DrawnVariants(plan.SpanVariants, variants);
    const std::vector<uint32> crestVariants = DrawnVariants(plan.CrestVariants, variants);

    std::vector<std::string> report = built.Validation;
    report.insert(report.end(), variants.Report.begin(), variants.Report.end());
    ReportRecipeValidation(report, name, candidate.Entity.id, state.LoggedValidation);

    FenceEmissionPools pools;
    pools.Posts = postPieces;
    pools.Spans = spanPieces;
    pools.Crests = crestPieces;
    pools.Caps = capPieces;
    pools.Gates = gatePieces;
    pools.SpanBounds = spanBounds;
    pools.GateBounds = gateBounds;
    pools.CrestBounds = crestBounds;
    pools.CapBounds = capBounds;
    // Stations were laid out on the pool's first footprint (the family stands
    // in for its members), so the pose and the transform agree only if the
    // transform reads that same footprint's axis.
    pools.PostAxis = layout.PostPiece.Axis();
    pools.Seed = recipe.Seed;
    pools.Variants = variantPieces;
    pools.SpanVariants = spanVariants;
    pools.CrestVariants = crestVariants;
    // In the order the layout spent the piece budget in.
    const std::vector<FenceEmission> emissions = BuildFenceEmissions(built, pools);
    if (emissions.empty())
        return RetireAll(world, &renderServices, state);

    // Pieces keep their entities for as long as what they are survives
    // (Placement/FencePieceReuse.h), so a rebuild keeps the selection: the span
    // an author just made a gate is still the one selected.
    return ReconcileFencePieces(world, candidate.Entity, invPlacerWorld, emissions, state.Pieces);
}

} // namespace GameEngine::Editor
