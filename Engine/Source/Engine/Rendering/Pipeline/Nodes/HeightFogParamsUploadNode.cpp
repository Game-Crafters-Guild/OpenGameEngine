#include "Engine/Rendering/Pipeline/Nodes/HeightFogParamsUploadNode.h"

#include "Engine/Rendering/FogSunLightingResolver.h"
#include "Engine/Rendering/HeightFogParamsUBO.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"

#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool HeightFogParamsUploadNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
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

void HeightFogParamsUploadNode::DeclareForView(ViewDeclare& d)
{
    // Dissolved into the upload ring: settings resolved + written at
    // declaration; no pass, no graph declaration (host-coherent).
    auto alloc = d.Frame.AllocUpload<HeightFogParamsUBO>();
    if (!alloc.Valid())
        return;

    const PostProcessSettings& pp =
        d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    HeightFogParamsUBO ubo{};
    FillHeightFogParamsUBO(pp, ubo);
    ubo.noiseTime = d.Services.GetShaderAnimationTimeSeconds();

    if (pp.HeightFogTrackDirectionalLight)
    {
        const float fallbackDir[3] = {pp.HeightFogSunDirX, pp.HeightFogSunDirY, pp.HeightFogSunDirZ};
        const float fallbackColor[3] = {pp.HeightFogSunColorR, pp.HeightFogSunColorG,
                                        pp.HeightFogSunColorB};
        const ResolvedFogSun sun = ResolveHeightFogSunLighting(
            d.Services, d.View.id, pp.HeightFogTrackDirectionalLight != 0,
            pp.HeightFogSunIntensityScale, fallbackDir, fallbackColor, pp.HeightFogSunIntensity);
        ubo.sunDirX = sun.direction[0];
        ubo.sunDirY = sun.direction[1];
        ubo.sunDirZ = sun.direction[2];
        ubo.sunColorR = sun.color[0];
        ubo.sunColorG = sun.color[1];
        ubo.sunColorB = sun.color[2];
        ubo.sunIntensity = sun.intensity;
    }

    *alloc.Ptr = ubo;
    d.PublishBuffer(m_BufferRef,
                    {alloc.Buffer, alloc.Offset, sizeof(HeightFogParamsUBO), /*Graph*/ {}});
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
