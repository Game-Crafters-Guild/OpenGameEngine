#include "Engine/Rendering/SceneProbeRecaptureSchedule.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderServices.h"

namespace GameEngine
{
namespace Engine::Renderer
{

SceneProbeWorldEpochs SceneProbeWorldEpochs::Read(const RenderServices& services, uint64_t worldId)
{
    SceneProbeWorldEpochs epochs{};
    epochs.RenderContent = services.RenderContentVersion(worldId);
    epochs.ShadowCasters = services.ShadowCasterContentVersion(worldId);
    epochs.Lights = services.WorldLightListVersion(worldId);
    epochs.DynamicDepth = services.GetIdleElisionFrameState().DepthDynamicEpoch;
    epochs.Materials = Material::GetGlobalContentEpoch();
    return epochs;
}

bool SceneProbeRecaptureSchedule::Due(uint64_t inputDigest, const SceneProbeWorldEpochs& epochs,
                                      float realtimeInterval) const
{
    if (inputDigest == 0)
        return false;
    if (inputDigest != m_LastInputDigest)
        return true;
    if (realtimeInterval < 0.0f)
        return m_ConvergenceBakesRemaining > 0;
    if (realtimeInterval == 0.0f)
        return true;
    return m_SecondsSinceBake >= realtimeInterval &&
           (m_ConvergenceBakesRemaining > 0 || epochs != m_LastEpochs);
}

void SceneProbeRecaptureSchedule::ConsumeBake(uint64_t inputDigest, const SceneProbeWorldEpochs& epochs,
                                              bool realtime)
{
    const bool changed = inputDigest != m_LastInputDigest || (realtime && epochs != m_LastEpochs);
    if (changed)
        m_ConvergenceBakesRemaining = kConvergenceBakes;
    else if (m_ConvergenceBakesRemaining > 0)
        --m_ConvergenceBakesRemaining;
    m_LastInputDigest = inputDigest;
    m_LastEpochs = epochs;
    m_SecondsSinceBake = 0.0f;
}

} // namespace Engine::Renderer
} // namespace GameEngine
