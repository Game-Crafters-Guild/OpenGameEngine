#include "Placement/RecipeControllers.h"

#include "Core/CpuProfiler.h"
#include "Placement/SplineExtrudeController.h"
#include "Placement/SplineFenceController.h"
#include "Placement/SplineGroundAuthority.h"
#include "Placement/SplinePlacementController.h"
#include "Placement/SplineWallController.h"

namespace GameEngine::Editor
{

RecipeControllers::RecipeControllers()
    : m_Placement(std::make_unique<SplinePlacementController>()),
      m_Fence(std::make_unique<SplineFenceController>()),
      m_Extrude(std::make_unique<SplineExtrudeController>()),
      m_Wall(std::make_unique<SplineWallController>()),
      m_GroundAuthority(std::make_unique<SplineGroundAuthorityController>())
{
}

RecipeControllers::~RecipeControllers() = default;

void RecipeControllers::Update(ECS::World& world, Engine::Renderer::RenderServices* renderServices,
                               UndoRedoService* undo, float32 deltaSeconds)
{
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplinePlacement");
        m_Placement->Update(world, renderServices, deltaSeconds);
    }
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplineFence");
        m_Fence->Update(world, renderServices, deltaSeconds);
    }
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplineExtrude");
        m_Extrude->Update(world, renderServices, deltaSeconds);
    }
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplineWall");
        m_Wall->Update(world, renderServices, deltaSeconds);
    }
    {
        GE_CPU_PROFILE_SCOPE("RecipeControllers.SplineGroundAuthority");
        m_GroundAuthority->Update(world, undo);
    }
}

void RecipeControllers::ReleaseSceneScoped(ECS::World& world,
                                           Engine::Renderer::RenderServices* renderServices)
{
    m_Placement->Shutdown(world);
    m_Fence->Shutdown(world, renderServices);
    m_Extrude->Shutdown(world, renderServices);
    m_Wall->Shutdown(world, renderServices);
    m_GroundAuthority->Shutdown();
}

} // namespace GameEngine::Editor
