#pragma once

#include "Components/Spline/SplineFence.h"
#include "ECS/ECS.h"
#include "Events/Event.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Placement/FenceMitreVariants.h"
#include "Placement/FencePieceReuse.h"
#include "Placement/SplineChunkCommit.h"
#include "Placement/SplinePieceMaterial.h"
#include "Placement/SplineRebuildGate.h"
#include "Types/ScopedSubscription.h"
#include "Types/Types.h"

#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::ECS { class World; }
namespace GameEngine::Engine::Renderer { class RenderServices; }

namespace GameEngine::Editor
{

// Editor-side consumer of Components::SplineFence: drapes a dense centerline of
// the sibling SplineComponent's curve onto the visible scene via the editor
// raycast, walks one run per authored segment (authored points are mandatory
// stations, closed splines add the last->first run), and maintains
// RuntimeOnlyEntity-tagged post and span entities. The scene serializes only
// the recipe; the pieces rebuild from it every session.
//
// Pieces are CHILDREN of the recipe entity, so their Transform is parent-local
// (Placement/PieceEntity.h composes it) and the hierarchy collapses them under
// the placer instead of flooding the scene root. They are generated output,
// not authored content: a hand edit to a piece is overwritten on the next
// rebuild, which the inspector states (Inspectors/GeneratedEntityNotice.h).
//
// Ticked once per frame by RecipeControllers on the main thread —
// both the scene raycast entry points and model GPU registration are
// main-thread-bound. Sibling of SplinePlacementController, which does the same
// for surface tiles; the two share the draped-centerline machinery
// (CenterlineSampling, SplineSurfaceConform, TileLayout).
class SplineFenceController
{
public:
    // Subscribes the recipe's span overrides to spline point edits
    // (Placement/FenceSpanOverrideRemap.h), so an override keeps naming its
    // span when the points around it are inserted, removed or resampled.
    SplineFenceController();

    // Re-places pieces for every (SplineComponent, SplineFence) entity whose
    // spline data, world transform, recipe, or CONFORMED GROUND changed — after
    // the edit has been stable for the settle window. A conforming recipe's
    // FIRST build additionally waits for its ground to exist and stop moving
    // (Placement/SplineSurfaceConform.h); a non-conforming one applies
    // immediately. Pieces hold measured altitudes, so a modifier bake, a sculpt
    // stroke or a moved terrain re-places them exactly as a recipe edit does
    // (Placement/SplineSurfaceConform.h). Retires pieces whose recipe entity
    // disappeared.
    void Update(ECS::World& world, Engine::Renderer::RenderServices* renderServices,
                float32 deltaSeconds);

    // Destroys every generated piece, releases every cut span variant and drops
    // all per-spline state. A caller
    // that is about to clear the world MUST run it against the OUTGOING world:
    // ANY World::Clear restarts entity versions, so a piece handle that survives
    // one passes World::IsValid against whichever entity recycles its index, and
    // the next retire sweep destroys that entity.
    //
    // This covers the clears a caller can see coming (scene open, new scene).
    // Clear is also reached from World::DeserializeWorld — play-mode exit, undo
    // snapshot restore — where nothing gets to run first; Update carries the
    // reset-generation guard for those.
    void Shutdown(ECS::World& world, Engine::Renderer::RenderServices* renderServices);

private:
    // Everything a rebuild depends on; a change in any part schedules one.
    struct ObservedState
    {
        SplineObservedInputs Spline{};
        // Compared with the component's defaulted memberwise ==, so a future
        // field can never be missed and silently stop rebuilds.
        // SANITIZED (FenceLayout.h): a NaN in an authored float would make this
        // recipe unequal to itself and re-arm the settle rebuild forever.
        Components::SplineFence Recipe{};
        // The ground the last conform measured (ObservedSurfaceRevision). Pieces
        // hold measured altitudes, so a terrain bake or sculpt under a finished
        // fence invalidates them with nothing else in this state changing. On
        // scene load a rebuild can still run against ground the modifier volumes
        // have not shaped yet, and this is what re-places it when they do (the
        // FIRST build is held outright — DeferFirstBuild). Zero
        // for a ConformMode=None recipe: it measures no ground, and a digest
        // term would let unrelated terrain churn starve its rebuild settle.
        uint64 SurfaceRevision = 0;
        // FencePoolContentDigest of the span and crest pools: a reimported
        // piece keeps its handle and bounds, so without this nothing here
        // changes and its mitred pieces keep drawing the old cut.
        uint64 PoolContent = 0;

        bool operator==(const ObservedState&) const = default;
    };

    struct FenceState
    {
        SplineRebuildGate<ObservedState> Gate;
        bool LoadFailureLogged = false;
        bool CenterlineBudgetLogged = false;
        // Conform misses reported for this entity, so a scene that rebuilds every frame
        // warns when the count moves rather than once per frame.
        uint32 LoggedConformMisses = 0;
        // Same on-change contract for the station tilt ceiling and for spans
        // that stood too near vertical to keep a roll of their own.
        uint32 LoggedTiltClamps = 0;
        uint32 LoggedRollBorrowedSpans = 0;
        // The pieces this recipe placed, in emission order, each with what it
        // is and the mesh it renders (Placement/FencePieceReuse.h). A rebuild
        // keeps an entity for as long as its identity survives and rewrites its
        // renderer only when its mesh signature changes, so a knot drag rewrites
        // transforms instead of churning entities (and the shadow-cache
        // invalidation that drags in).
        std::vector<FencePieceEntity> Pieces;
        // The last layout's span count per run, by the authored point opening
        // it: what a point edit re-addresses this recipe's overrides by.
        std::vector<uint32> RunSpanCounts;
        // The cut variants mitred spans and registered crests draw (fence
        // design section 4c), registered through the chunk commit and released
        // when no piece draws them any more.
        SplineChunkCommit MitreVariants;
        FenceMitreCuts MitreCuts;
        // The pool content digest and the mesh reload count it was taken at,
        // so it is re-read only after the registry reports a reload.
        uint64 PoolContent = 0;
        uint64 PoolContentReloads = ~0ull;
        std::vector<std::string> LoggedValidation;
        uint64 VisitStamp = 0;
    };

    // One fence candidate gathered from the ECS query (copied out so the world
    // can be mutated after iteration).
    struct Candidate
    {
        ECS::EntityHandle Entity{};
        Components::SplineFence Recipe{}; // sanitized at gather time
        uint32 SplineDataIndex = 0;
        uint32 SplineDataGeneration = 0;
        float32 WorldMatrix[16] = {};
    };

    // Returns true when pieces were spawned or destroyed (caller flushes the
    // world's deferred commands once per frame).
    bool Rebuild(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                 const Candidate& candidate, FenceState& state);
    // Destroys the recipe's piece entities; its cut variants stay registered
    // for the pieces about to be spawned in their place.
    static bool RetirePieces(ECS::World& world, FenceState& state);
    // Destroys the pieces and releases the cut variants: the recipe draws
    // nothing any more.
    static bool RetireAll(ECS::World& world, Engine::Renderer::RenderServices* renderServices,
                          FenceState& state);

    std::unordered_map<uint32, FenceState> m_States; // key: spline entity id
    Engine::Renderer::ModelResolveCache m_ModelCache;
    OverrideMaterialCache m_OverrideMaterials;
    uint64 m_VisitStamp = 0;
    // World::GetLifecycleResetGeneration() as of the last Update.
    uint64 m_LastWorldResetGeneration = 0;
    // Mesh unregisters and in-place reloads the registry has reported. Bumped
    // from the registry's callback, which runs under its table mutex on
    // whichever thread unregisters, so it is an atomic and nothing more.
    std::atomic<uint64> m_MeshReloads{0};
    const Rendering::MeshGPURegistry* m_SubscribedRegistry = nullptr;
    // Declared after the counter it bumps, so it is released first.
    ScopedSubscription m_MeshReloadSubscription;
    EventSubscription m_PointEditSubscription;
};

} // namespace GameEngine::Editor
