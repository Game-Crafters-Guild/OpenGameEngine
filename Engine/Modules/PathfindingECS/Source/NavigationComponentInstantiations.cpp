// Explicit cross-DLL instantiation for PathfindingECS's public components.
// See ECS/ECSTemplates.h for the rationale (GE_INSTANTIATE_ENGINE_COMPONENT).
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationDebugSettings.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/Components/NavigationMesh.h"
#include "PathfindingECS/Components/NavigationObstacle.h"

#include "ECS/ECSTemplates.h"

namespace GameEngine::ECS
{
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::NavigationGrid);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::NavigationMesh);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::NavigationAgent);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::NavigationObstacle);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::NavigationDebugSettings);
} // namespace GameEngine::ECS
