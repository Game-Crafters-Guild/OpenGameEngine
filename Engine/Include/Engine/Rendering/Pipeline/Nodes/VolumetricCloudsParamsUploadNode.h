#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

// Resolves the volumetric-clouds settings (plus the scene's primary
// directional light as the cloud sun) into the CloudParams UBO each frame and
// publishes it for volumetric_clouds_march.frag. Mirrors
// HeightFogParamsUploadNode: no pass, just an upload-ring write at declaration.
class VolumetricCloudsParamsUploadNode final : public IRenderPipelineNode
{
public:
    const char* GetTypeName() const override { return "VolumetricCloudsParamsUpload"; }
    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

private:
    std::string m_Id;
    std::string m_Json;
    std::string m_BufferRef = "CloudParams";
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
