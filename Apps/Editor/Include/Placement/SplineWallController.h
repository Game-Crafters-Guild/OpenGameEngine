#pragma once

#include "Components/Spline/SplineWall.h"
#include "ECS/ECS.h"
#include "Placement/SplineChunkCommit.h"
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

// Editor-side consumer of Components::SplineWall: drapes the sibling
// SplineComponent's curve onto the visible scene with the conform rays and
// ignore list the other spline recipes use, builds the wall through the shared
// sweep (Placement/SplineWallBuild.h), and keeps RuntimeOnlyEntity-tagged chunk
// entities carrying the generated mesh.
//
// Its lifecycle is the extrude's: a rebuild after the edit has been stable for
// the settle window, dirty on spline version, service handle, world matrix,
// recipe inequality and the conformed ground's revision; chunk meshes
// registered under stable keys and re-registered in place through the shared
// chunk commit (Placement/SplineChunkCommit.h), so a steady-state rebuild
// churns no entities.
//
// Ticked once per frame by RecipeControllers on the main thread: the scene
// raycast and mesh registration are both main-thread-bound.
class SplineWallController
{
public:
    // Rebuilds every (SplineComponent, SplineWall) entity whose spline,
    // transform, recipe or conformed ground changed, once the edit has been
    // stable for the settle window, and retires the chunks of walls whose entity
    // disappeared.
    void Update(ECS::World& world, Engine::Renderer::RenderServices* renderServices,
                float32 deltaSeconds);

    // Releases every registered chunk mesh and destroys every chunk entity. Run
    // it against the OUTGOING world before a scene swap clears it: the meshes
    // outlive the entities otherwise, and a surviving chunk handle would
    // validate against whichever incoming entity recycles its index.
    void Shutdown(ECS::World& world, Engine::Renderer::RenderServices* renderServices);

private:
    // Everything a rebuild depends on; a change in any part schedules one.
    struct ObservedState
    {
        SplineObservedInputs Spline{};
        // Compared with the component's defaulted memberwise ==, so a future
        // field can never be missed and silently stop rebuilds.
        Components::SplineWall Recipe{};
        // The ground the last rebuild measured (ObservedSurfaceRevision); zero
        // for a wall that does not conform.
        uint64 SurfaceRevision = 0;

        bool operator==(const ObservedState&) const = default;
    };

    struct WallState
    {
        SplineRebuildGate<ObservedState> Gate;
        bool CenterlineBudgetLogged = false;
        uint32 LoggedConformMisses = 0;
        // Validation lines last logged for this wall, so they log once per
        // change rather than once per rebuild (ReportRecipeValidation).
        std::vector<std::string> LoggedValidation;
        SplineChunkCommit Chunks;
        uint64 VisitStamp = 0;
    };

    struct Candidate
    {
        ECS::EntityHandle Entity{};
        // As authored, for the validation a sanitized value no longer shows.
        Components::SplineWall Authored{};
        // Sanitized: what is compared and built.
        Components::SplineWall Recipe{};
        uint32 SplineDataIndex = 0;
        uint32 SplineDataGeneration = 0;
        float32 WorldMatrix[16] = {};
    };

    bool Rebuild(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                 const Candidate& candidate, WallState& state);
    // Binds the wall's material and puts its chunk meshes on screen through the
    // chunk commit.
    bool Commit(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                const Candidate& candidate, WallState& state, const std::vector<Mesh>& meshes);
    // Drops all cached state after a world reset WITHOUT destroying anything
    // (SplineChunkCommit::DropAfterWorldReset).
    void DropStateAfterWorldReset(Engine::Renderer::RenderServices* renderServices);

    std::unordered_map<uint32, WallState> m_States; // key: spline entity id
    // This frame's walls, kept between frames so collecting them allocates only
    // when the count grows.
    std::vector<Candidate> m_Candidates;
    uint64 m_VisitStamp = 0;
    // World::GetLifecycleResetGeneration() as of the last Update.
    uint64 m_LastWorldResetGeneration = 0;
    bool m_DefaultMaterialRegistered = false;
    OverrideMaterialCache m_OverrideMaterials;
};

} // namespace GameEngine::Editor
