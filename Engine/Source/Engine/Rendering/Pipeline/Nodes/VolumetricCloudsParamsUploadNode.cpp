#include "Engine/Rendering/Pipeline/Nodes/VolumetricCloudsParamsUploadNode.h"

#include "Engine/Rendering/FogSunLightingResolver.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/VolumetricCloudsParamsUBO.h"

#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool VolumetricCloudsParamsUploadNode::Initialize(std::string nodeId, std::string nodeJson,
                                                  std::string* outError)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);

    try
    {
        auto j = nlohmann::json::parse(m_Json);
        if (j.is_object() && j.contains("buffer") && j["buffer"].is_string())
            m_BufferRef = j["buffer"].get<std::string>();
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("JSON parse failed: ") + e.what();
        return false;
    }

    return true;
}

void VolumetricCloudsParamsUploadNode::DeclareForView(ViewDeclare& d)
{
    const PostProcessSettings& pp =
        d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    // No clouds: no ring allocation and no light resolve. Every consumer of
    // this buffer carries the same gate, so publishing nothing is inert.
    if (!pp.IsVolumetricCloudsActive())
        return;

    // Dissolved into the upload ring: settings resolved + written at
    // declaration; no pass, no graph declaration (host-coherent).
    auto alloc = d.Frame.AllocUpload<VolumetricCloudsParamsUBO>();
    if (!alloc.Valid())
        return;

    VolumetricCloudsParamsUBO ubo{};
    FillVolumetricCloudsParamsUBO(pp, ubo);

    // The cloud sun always tracks the primary directional light, with the sky
    // sun as fallback (ResolveHeightFogSunLighting's chain). The resolved
    // direction points TOWARD the sun — the shader's light-march direction.
    const float fallbackDir[3] = {ubo.sunDirX, ubo.sunDirY, ubo.sunDirZ};
    const float fallbackColor[3] = {ubo.sunColorR, ubo.sunColorG, ubo.sunColorB};
    const ResolvedFogSun sun = ResolveHeightFogSunLighting(
        d.Services, d.View.id, /*trackDirectionalLight*/ true,
        /*sunIntensityScale*/ 1.0f, fallbackDir, fallbackColor, ubo.sunIntensity);
    ubo.sunDirX = sun.direction[0];
    ubo.sunDirY = sun.direction[1];
    ubo.sunDirZ = sun.direction[2];
    ubo.sunColorR = sun.color[0];
    ubo.sunColorG = sun.color[1];
    ubo.sunColorB = sun.color[2];
    ubo.sunIntensity = sun.intensity;

    *alloc.Ptr = ubo;
    d.PublishBuffer(m_BufferRef,
                    {alloc.Buffer, alloc.Offset, sizeof(VolumetricCloudsParamsUBO), /*Graph*/ {}});
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
