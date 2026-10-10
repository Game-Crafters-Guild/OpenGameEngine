#include "Engine/Rendering/SceneBuildPump.h"

#include "Engine/Rendering/ModelRenderSetup.h"

namespace GameEngine
{
namespace Engine::Renderer
{

namespace
{
// Editor scene-open builds in flight. Only SceneBuildPump changes it.
int s_ActiveSceneBuilds = 0;
}

bool IsSceneBuildPumpActive()
{
    return s_ActiveSceneBuilds > 0;
}

SceneBuildPump::~SceneBuildPump()
{
    Reset();
}

void SceneBuildPump::Reset()
{
    if (m_CountedAsActive && s_ActiveSceneBuilds > 0)
        --s_ActiveSceneBuilds;
    m_CountedAsActive = false;
    m_Begun = false;
    m_Batch = SceneResolveBatch::None;
    m_Resolves.Reset();
}

void SceneBuildPump::Begin(ECS::World& world, RenderServices& renderServices)
{
    Reset();
    m_Batch = m_Resolves.EnqueueWorld(world, renderServices);
    m_Begun = true;
    if (!m_Resolves.IsBatchComplete(m_Batch))
    {
        m_CountedAsActive = true;
        ++s_ActiveSceneBuilds;
    }
}

void SceneBuildPump::ReleaseActiveCountWhenComplete()
{
    if (!m_CountedAsActive || !m_Resolves.IsBatchComplete(m_Batch))
        return;
    m_CountedAsActive = false;
    if (s_ActiveSceneBuilds > 0)
        --s_ActiveSceneBuilds;
}

std::size_t SceneBuildPump::StepBudgeted(ECS::World& world, RenderServices& renderServices,
                                         std::chrono::milliseconds budget)
{
    if (!IsActive())
        return 0;
    const std::size_t taken = m_Resolves.Step(world, renderServices, budget);
    ReleaseActiveCountWhenComplete();
    return taken;
}

std::size_t SceneBuildPump::DrainNow(ECS::World& world, RenderServices& renderServices)
{
    if (!IsActive())
        return 0;
    const std::size_t taken = m_Resolves.DrainNow(world, renderServices);
    ReleaseActiveCountWhenComplete();
    return taken;
}

std::size_t SceneBuildPump::TotalItems() const
{
    return m_Resolves.BatchProgress(m_Batch).Total;
}

std::size_t SceneBuildPump::ProcessedItems() const
{
    return m_Resolves.BatchProgress(m_Batch).Done;
}

} // namespace Engine::Renderer
} // namespace GameEngine
