// Measurement body shared by the two probe translation units. Included AFTER
// each one has forced its NDEBUG state, so the identical source is compiled
// twice under the two states a host and the engine can disagree about.
//
// GE_ABI_LAYOUT_PROBE_FUNCTION names the function this copy defines.

#include "EngineHostAbiLayoutProbe.h"

#include "Core/Application.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/RenderServices.h"
#include "JobSystem/TaskDependencyGraph.h"
#include "JobSystem/WorkStealingThreadPool.h"

namespace GameEngine::Testing
{

EnginePublicTypeLayout GE_ABI_LAYOUT_PROBE_FUNCTION()
{
    EnginePublicTypeLayout layout{};
#if defined(NDEBUG)
    layout.NDebugDefined = true;
#else
    layout.NDebugDefined = false;
#endif
    layout.RenderServices          = sizeof(::GameEngine::Engine::Renderer::RenderServices);
    layout.MeshGPURegistry         = sizeof(::GameEngine::Rendering::MeshGPURegistry);
    layout.MaterialBinder          = sizeof(::GameEngine::Engine::Renderer::MaterialBinder);
    layout.PipelineVariantCache    = sizeof(::GameEngine::Engine::Renderer::PipelineVariantCache);
    layout.RenderExtractionSystem  = sizeof(::GameEngine::Engine::Renderer::RenderExtractionSystem);
    layout.WorkStealingThreadPool  = sizeof(::JobSystem::WorkStealingThreadPool);
    layout.TaskDependencyGraph     = sizeof(::JobSystem::TaskDependencyGraph);
    layout.ApplicationConfig       = sizeof(::GameEngine::ApplicationConfig);
    return layout;
}

} // namespace GameEngine::Testing
