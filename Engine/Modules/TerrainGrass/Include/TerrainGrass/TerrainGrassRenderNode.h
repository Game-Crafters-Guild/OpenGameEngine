#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::TerrainGrass
{

class TerrainGrassRenderNode final : public Engine::Renderer::Pipeline::IRenderPipelineNode
{
public:
    const char* GetTypeName() const override { return "TerrainGrass"; }
    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d) override;

private:
    std::string m_Id;
    std::string m_Json;
};

} // namespace GameEngine::TerrainGrass
