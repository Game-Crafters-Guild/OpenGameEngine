#pragma once

#include "AssetCore/GUID.h"
#include "Components/Spline/SplineExtrude.h"
#include "ECS/ECS.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Placement/SplineChunkCommit.h"
#include "Placement/SplineFillRebuild.h"
#include "Placement/SplinePieceMaterial.h"
#include "Placement/SplineRebuildGate.h"
#include "Types/Types.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::ECS { class World; }
namespace GameEngine::Engine::Renderer { class RenderServices; }

namespace GameEngine::Editor
{

// Editor-side consumer of Components::SplineExtrude: drapes a dense centerline
// of the sibling SplineComponent's curve onto the visible scene with the same
// conform rays and the same ignore list the placement recipes use, turns those
// stations into geometry, and maintains RuntimeOnlyEntity-tagged chunk entities
// carrying the generated mesh.
//
// TWO emitters hang off that one centerline, and they are different shapes
// rather than two settings of one shape. Channel SWEEPS the recipe's
// cross-section along the stations. FitToBanks FILLS instead: it floods the
// region the water occupies over the terrain and meshes it as an area, because
// the shape water takes is decided by the ground and is routinely not a
// thickened curve at all. They share the centerline, the chunk slots, the
// materials and the settle timer, and nothing else — they never meet in one
// mesh.
//
// It shares the placement controller's lifecycle wholesale — settle-timer
// coalescing, dirty on spline version / service handle / world matrix / recipe
// inequality / conformed-ground revision, dead output forcing a rebuild — so
// there is one mental model across all three spline recipes.
//
// What it does NOT share is the output shape. Placement spawns one entity per
// tile; this registers one GENERATED MESH per chunk under a stable key and
// re-registers it in place on rebuild, which is the mesh registry's own update
// path: the handle and the GPUScene row survive, so the steady-state rebuild
// churns no entities at all. Only a change in chunk COUNT spawns or retires one,
// and only a retired key needs unregistering — there is no refcount, so this
// controller owns that.
//
// Ticked once per frame by RecipeControllers on the main thread: the
// scene raycast and mesh registration are both main-thread-bound.
class SplineExtrudeController
{
public:
    // Regenerates geometry for every (SplineComponent, SplineExtrude) entity
    // whose spline, transform, recipe or CONFORMED GROUND changed, after the
    // edit has been stable for the settle window. Retires chunks whose recipe
    // entity disappeared.
    void Update(ECS::World& world, Engine::Renderer::RenderServices* renderServices,
                float32 deltaSeconds);

    // Releases every registered chunk mesh and destroys every chunk entity.
    // A caller that is about to clear the world MUST run it against the
    // OUTGOING world, for two independent reasons: the meshes outlive the
    // entities otherwise (the registry does not refcount), and ANY
    // World::Clear restarts entity versions, so a chunk handle that survives
    // one passes World::IsValid against whichever entity recycles its index —
    // the next retire sweep then destroys that entity.
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
        Components::SplineExtrude Recipe{};
        // The ground the last rebuild measured (ObservedSurfaceRevision).
        // Generated geometry holds measured altitudes exactly as placed tiles
        // do, so a terrain bake or sculpt under a finished path invalidates it
        // with nothing else here changing; on scene load a rebuild can still
        // run against ground the modifier volumes have not shaped yet, and this
        // re-places it when they do (the FIRST build is held outright —
        // DeferFirstBuild). Zero only for a
        // recipe that reads no ground at all (Components::ReadsSurface): it has
        // nothing to invalidate, and a digest term would let unrelated terrain
        // churn starve its rebuild settle. Conforming is not the only read — a
        // FitToBanks recipe measures its banks whatever its ConformMode, so it
        // carries the digest and re-fits when the bed under it is carved.
        uint64 SurfaceRevision = 0;

        bool operator==(const ObservedState&) const = default;
    };

    struct ExtrudeState
    {
        SplineRebuildGate<ObservedState> Gate;
        bool CenterlineBudgetLogged = false;
        uint32 LoggedConformMisses = 0;
        // Validation lines last logged for this recipe (a retired profile), so
        // they log once per change rather than once per frame.
        std::vector<std::string> LoggedValidation;
        // Last reported water-fill state, so the diagnostics log once per change
        // rather than once per rebuild.
        SplineGeometry::SplineFillDiagnostics LastFillDiagnostics{};
        WaterFillOutcome LastFillOutcome = WaterFillOutcome::Built;
        SplineChunkCommit Chunks;
        uint64 VisitStamp = 0;
    };

    struct Candidate
    {
        ECS::EntityHandle Entity{};
        Components::SplineExtrude Recipe{};
        // Saved with the retired Rectangle profile: built into nothing and
        // reported (CarriesRetiredWallProfile).
        bool RetiredProfile = false;
        uint32 SplineDataIndex = 0;
        uint32 SplineDataGeneration = 0;
        float32 WorldMatrix[16] = {};
    };

    bool Rebuild(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                 const Candidate& candidate, ExtrudeState& state);
    // Binds the recipe's material and puts a freshly built set of chunk meshes
    // on screen through the chunk commit (Placement/SplineChunkCommit.h).
    // Shared by both emitters — templated only because the two return two mesh
    // types with the same shape.
    template <typename MeshT>
    bool CommitChunkMeshes(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                           const Candidate& candidate, ExtrudeState& state,
                           const std::vector<MeshT>& chunkMeshes);

    // Drops all cached state after a world reset WITHOUT destroying anything
    // (SplineChunkCommit::DropAfterWorldReset).
    void DropStateAfterWorldReset(Engine::Renderer::RenderServices* renderServices);

    std::unordered_map<uint32, ExtrudeState> m_States; // key: spline entity id
    uint64 m_VisitStamp = 0;
    // World::GetLifecycleResetGeneration() as of the last Update.
    uint64 m_LastWorldResetGeneration = 0;
    bool m_DefaultMaterialRegistered = false;
    OverrideMaterialCache m_OverrideMaterials;
};

} // namespace GameEngine::Editor
