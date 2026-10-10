#pragma once

#include "Types/Types.h"

#include <memory>

namespace GameEngine::ECS { class World; }
namespace GameEngine::Engine::Renderer { class RenderServices; }

namespace GameEngine::Editor
{

class SplineExtrudeController;
class SplineFenceController;
class SplineGroundAuthorityController;
class SplinePlacementController;
class SplineWallController;
class UndoRedoService;

// Owns every spline recipe controller: the ones that generate what a recipe
// component describes (tiles, fence pieces, swept paths and roads, walls) and
// the one that keeps the terrain effects under a generated run in step with its
// recipe.
//
// The editor ticks this once per frame and releases it once at a scene swap,
// so it never names a recipe; a new recipe's controller is added here.
//
// Ticked on the main thread: the scene raycast, model GPU registration and mesh
// registration the controllers do are all main-thread-bound. The order is
// load-bearing only for the ground authority, which runs after the generators
// so its terrain writes reach the terrain modifier system's change gate in the
// same frame, exactly as an inspector edit does.
class RecipeControllers
{
public:
    RecipeControllers();
    ~RecipeControllers();

    RecipeControllers(const RecipeControllers&) = delete;
    RecipeControllers& operator=(const RecipeControllers&) = delete;

    void Update(ECS::World& world, Engine::Renderer::RenderServices* renderServices,
                UndoRedoService* undo, float32 deltaSeconds);

    // Destroys every generated entity and releases every registered mesh. The
    // generators cache handles to the entities they spawn, and those handles are
    // scoped to the open scene: a swap clears the world and restarts entity
    // versions, after which a surviving handle validates against — and the next
    // retire sweep destroys — whichever incoming entity recycled its index. The
    // ground authority's per-entity records are dropped for the same reason. Run
    // it against the OUTGOING world.
    void ReleaseSceneScoped(ECS::World& world, Engine::Renderer::RenderServices* renderServices);

private:
    std::unique_ptr<SplinePlacementController> m_Placement;
    std::unique_ptr<SplineFenceController> m_Fence;
    std::unique_ptr<SplineExtrudeController> m_Extrude;
    std::unique_ptr<SplineWallController> m_Wall;
    std::unique_ptr<SplineGroundAuthorityController> m_GroundAuthority;
};

} // namespace GameEngine::Editor
