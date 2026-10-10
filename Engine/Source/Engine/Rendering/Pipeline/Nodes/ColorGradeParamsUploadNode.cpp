#include "Engine/Rendering/Pipeline/Nodes/ColorGradeParamsUploadNode.h"

#include "Engine/Rendering/ColorGradeParamsUBO.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"

#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool ColorGradeParamsUploadNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
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

void ColorGradeParamsUploadNode::DeclareForView(ViewDeclare& d)
{
    // Dissolved into the upload ring: settings resolved + written at declaration;
    // no pass, no graph declaration (host-coherent). Matches HeightFogParamsUpload.
    auto alloc = d.Frame.AllocUpload<ColorGradeParamsUBO>();
    if (!alloc.Valid())
        return;

    const PostProcessSettings& pp =
        d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    ColorGradeParamsUBO ubo{};
    FillColorGradeParamsUBO(pp, ubo);

    *alloc.Ptr = ubo;
    d.PublishBuffer(m_BufferRef,
                    {alloc.Buffer, alloc.Offset, sizeof(ColorGradeParamsUBO), /*Graph*/ {}});
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
