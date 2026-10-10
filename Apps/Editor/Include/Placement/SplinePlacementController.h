#pragma once

#include "Components/Spline/SplinePlacement.h"
#include "ECS/ECS.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Placement/SplinePieceMaterial.h"
#include "Placement/SplineRebuildGate.h"
#include "Types/Types.h"

#include <unordered_map>
#include <vector>

namespace GameEngine::ECS { class World; }
namespace GameEngine::Engine::Renderer { class RenderServices; }

namespace GameEngine::Editor
{

// Editor-side consumer of Components::SplinePlacement: drapes a dense
// centerline of the sibling SplineComponent's curve onto the visible scene via
// the editor raycast (Picking::RaycastScene), spaces tiles by true 3D distance
// along that draped centerline (yaw from each tile's own chord, mesh footprint
// centered on the line), and maintains RuntimeOnlyEntity-tagged tile entities.
// The scene serializes only the recipe (SplineComponent + SplinePlacement);
// tiles rebuild from it every session.
//
// Tiles are CHILDREN of the recipe entity, so their Transform is parent-local
// (Placement/PieceEntity.h composes it) and the hierarchy collapses them under
// the placer instead of flooding the scene root. They are generated output,
// not authored content: a hand edit to a tile is overwritten on the next
// rebuild, which the inspector states (Inspectors/GeneratedEntityNotice.h).
//
// Ticked once per frame by RecipeControllers on the main thread —
// both the scene raycast entry points and model GPU registration are
// main-thread-bound.
//
// The spline entity's world transform is applied to every sampled frame before
// raycasting or spawning: SampleUniform returns ENTITY-LOCAL frames (Points
// are local). The shared ResolveWorldSpline helper is a later phase; the
// transform is applied inline here until it lands.
class SplinePlacementController
{
public:
    // Re-places tiles for every (SplineComponent, SplinePlacement) entity whose
    // spline data, world transform, recipe, or CONFORMED GROUND changed — after
    // the edit has been stable for the settle window. A conforming recipe's
    // FIRST build additionally waits for its ground to exist and stop moving
    // (Placement/SplineSurfaceConform.h); a non-conforming one applies
    // immediately. Tiles hold measured altitudes, so a modifier bake, a sculpt
    // stroke or a moved terrain re-places them exactly as a recipe edit does
    // (Placement/SplineSurfaceConform.h). Retires tiles whose recipe entity
    // disappeared.
    void Update(ECS::World& world, Engine::Renderer::RenderServices* renderServices,
                float32 deltaSeconds);

    // Destroys every generated tile and drops all per-spline state. A caller
    // that is about to clear the world MUST run it against the OUTGOING world:
    // ANY World::Clear restarts entity versions, so a tile handle that survives
    // one passes World::IsValid against whichever entity recycles its index, and
    // the next retire sweep destroys that entity.
    //
    // This covers the clears a caller can see coming (scene open, new scene).
    // Clear is also reached from World::DeserializeWorld — play-mode exit, undo
    // snapshot restore — where nothing gets to run first; Update carries the
    // reset-generation guard for those.
    void Shutdown(ECS::World& world);

private:
    // Everything a rebuild depends on; a change in any part schedules one.
    struct ObservedState
    {
        SplineObservedInputs Spline{};
        // Compared with the component's defaulted memberwise ==, so a future
        // field can never be missed and silently stop rebuilds.
        Components::SplinePlacement Recipe{};
        // The ground the last conform measured (ObservedSurfaceRevision). Tiles
        // hold measured altitudes, so a terrain bake or sculpt under a finished
        // path invalidates them with nothing else in this state changing. On
        // scene load a rebuild can still run against ground the modifier volumes
        // have not shaped yet, and this is what re-places it when they do (the
        // FIRST build is held outright — DeferFirstBuild). Zero
        // for a ConformMode=None recipe: it measures no ground, and a digest
        // term would let unrelated terrain churn starve its rebuild settle.
        uint64 SurfaceRevision = 0;

        bool operator==(const ObservedState&) const = default;
    };

    struct PlacementState
    {
        SplineRebuildGate<ObservedState> Gate;
        bool LoadFailureLogged = false;
        bool CenterlineBudgetLogged = false;
        // Conform misses reported for this entity, so a scene that rebuilds every frame
        // warns when the count moves rather than once per frame.
        uint32 LoggedConformMisses = 0;
        // Same on-change contract for the tilt ceiling's held-station count.
        uint32 LoggedTiltClamps = 0;
        std::vector<ECS::EntityHandle> Tiles;
        // The station ordinal each live tile was built for, parallel to Tiles.
        // Dropout means a rebuild can produce the same NUMBER of tiles from a
        // different SET of stations, and the mesh pick keys on the ordinal — so
        // "same count" is not enough to know the existing tiles still show the
        // right meshes. Matching this sequence is.
        std::vector<uint32> TileStationIndices;
        uint64 VisitStamp = 0;
    };

    // One placement candidate gathered from the ECS query (copied out so the
    // world can be mutated after iteration).
    struct Candidate
    {
        ECS::EntityHandle Entity{};
        Components::SplinePlacement Recipe{};
        uint32 SplineDataIndex = 0;
        uint32 SplineDataGeneration = 0;
        float32 WorldMatrix[16] = {};
    };

    // Returns true when tiles were spawned or destroyed (caller flushes the
    // world's deferred commands once per frame).
    bool Rebuild(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                 const Candidate& candidate, PlacementState& state);
    static bool RetireTiles(ECS::World& world, PlacementState& state);

    std::unordered_map<uint32, PlacementState> m_States; // key: spline entity id
    Engine::Renderer::ModelResolveCache m_ModelCache;
    OverrideMaterialCache m_OverrideMaterials;
    uint64 m_VisitStamp = 0;
    // World::GetLifecycleResetGeneration() as of the last Update.
    uint64 m_LastWorldResetGeneration = 0;
};

} // namespace GameEngine::Editor
