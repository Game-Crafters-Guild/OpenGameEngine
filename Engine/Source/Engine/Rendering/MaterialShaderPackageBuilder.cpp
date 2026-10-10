#include "Engine/Rendering/MaterialShaderPackageBuilder.h"

#include "Engine/Rendering/ShaderGraphMaterial.h"

#include <utility>

namespace GameEngine::Engine::Renderer
{
MaterialShaderPackageBuilder::MaterialShaderPackageBuilder(
    MaterialDocument document, std::filesystem::path materialPath,
    std::string debugName, Rendering::MaterialBuildContext& context)
    : m_Document(std::move(document)), m_MaterialPath(std::move(materialPath)),
      m_DebugName(std::move(debugName)), m_Context(context)
{
    m_Prepared = PrepareMaterialDocumentForShaderPackage(
        m_Document, m_MaterialPath, context, m_Errors, &m_GraphSourcePath);
}

Rendering::MaterialBuildResult MaterialShaderPackageBuilder::Build(
    Rendering::ShaderSourceKind kind, Rendering::MaterialKeyword pipelineKeywords,
    Rendering::VertexAttributeFlags meshVertexFlags) const
{
    if (!m_Prepared)
    {
        Rendering::MaterialBuildResult result;
        result.errors = m_Errors;
        return result;
    }
    return Rendering::BuildMaterialToShaderPackage(
        m_Document, m_MaterialPath, m_DebugName, m_Context, kind,
        pipelineKeywords, meshVertexFlags);
}
} // namespace GameEngine::Engine::Renderer
