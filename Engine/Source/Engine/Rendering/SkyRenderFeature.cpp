#include "Engine/Rendering/SkyRenderFeature.h"

#include "Core/Application.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

bool SkyRenderFeature::Initialize(Rendering::IDevice* device)
{
    if (m_Initialized)
        return true;

    // The renderer loads engine-owned sky art (the moon) from the staged asset tree.
    // Resolving the install layout is the host's job: the rendering module has no
    // dependency that can answer where this process was installed.
    SkyRenderer::Config config{};
    config.installAssetsRoot = PathUtils::GetInstallAssetsRoot();

    if (!m_Renderer.Initialize(device, config))
        return false;

    m_Initialized = true;
    return true;
}

void SkyRenderFeature::SetSettings(const Rendering::SkySettings& settings,
                                   const Rendering::SkySystemState& state)
{
    m_Settings = settings;
    m_State = state;
    m_HasSettings = true;
}

void SkyRenderFeature::ClearActiveSettings()
{
    m_HasSettings = false;
}

} // namespace Engine::Renderer
} // namespace GameEngine
