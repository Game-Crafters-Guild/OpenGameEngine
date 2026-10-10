#include "Engine/Rendering/Pipeline/Nodes/CpuTextureInputNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include <cstring>
#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
bool CpuTextureInputNode::Initialize(std::string, std::string nodeJson, std::string* outError)
{
    try
    {
        const auto json = nlohmann::json::parse(nodeJson);
        const auto source = json.at("source").get<std::string>();
        m_Output = json.at("output").get<std::string>();
        m_Parameters = json.value("parameters", std::string{});
        if (source.empty() || m_Output.empty() || m_Output == m_Parameters)
            throw std::invalid_argument("source/output must be nonempty and outputs distinct");
        m_Source = HashStringId(source.c_str());
        const auto purpose = json.value("viewPurpose", std::string("Game"));
        if (purpose == "Game")
            m_Purpose = Rendering::ViewPurpose::Game;
        else if (purpose == "EditorScene")
            m_Purpose = Rendering::ViewPurpose::EditorScene;
        else if (purpose == "EditorPreview")
            m_Purpose = Rendering::ViewPurpose::EditorPreview;
        else if (purpose == "UtilityCapture")
            m_Purpose = Rendering::ViewPurpose::UtilityCapture;
        else
            throw std::invalid_argument("unknown viewPurpose");
        return true;
    }
    catch (const std::exception& error)
    {
        if (outError)
            *outError = error.what();
        return false;
    }
}

void CpuTextureInputNode::DeclareForView(ViewDeclare& d)
{
    if (!d.View.worldId || d.View.purpose != m_Purpose)
        return;
    const auto binding = d.Services.Textures().GetCpuTextureForView(d.View.worldId, m_Source);
    if (!binding.Texture.IsValid() ||
        (m_Parameters.empty() != (binding.ParameterBytes == 0)))
        return;

    Rendering::RenderGraph::RGUploadRing::Alloc parameters{};
    if (binding.ParameterBytes)
    {
        parameters = d.Frame.AllocUpload(binding.ParameterBytes);
        if (!parameters.Ptr)
            return;
        std::memcpy(parameters.Ptr, binding.Parameters.data(), binding.ParameterBytes);
    }
    const auto texture = d.Frame.ImportExternalTexture(m_Output.c_str(), binding.Texture,
                                                       Rendering::ResourceState::ShaderResource, binding.Format);
    if (!texture.IsValid())
        return;
    // Resource validation/preparation precedes both publications. Allocation
    // exceptions from the existing blackboard insertions abort declaration.
    // Neither handle is retained in this node across frames.
    d.PublishTexture(m_Output, texture);
    if (binding.ParameterBytes)
        d.PublishBuffer(m_Parameters,
                        {parameters.Buffer, parameters.Offset, binding.ParameterBytes, {}});
}
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
