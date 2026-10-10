#pragma once

#include "Rendering/Materials/MaterialBuildService.h"

namespace GameEngine::Engine::Renderer
{
/** Prepares one authored material and builds its shader-package variants.
    The caller owns context for the builder's lifetime. Preparation extends its
    include roots; multiple builders in a serial cook share that context. */
class MaterialShaderPackageBuilder
{
public:
    /** Prepares `document`, the material at `materialPath`: a graph material's
        surface is generated under the context's cache root and the include
        roots it needs are added to `context`. A failed preparation is recorded,
        not thrown; check IsPrepared. `debugName` labels the compile logs. */
    MaterialShaderPackageBuilder(MaterialDocument document,
                                 std::filesystem::path materialPath,
                                 std::string debugName,
                                 Rendering::MaterialBuildContext& context);

    /** Whether preparation succeeded. Build compiles nothing until it has. */
    bool IsPrepared() const { return m_Prepared; }
    /** Why preparation failed when IsPrepared is false. */
    const std::vector<std::string>& GetErrors() const { return m_Errors; }
    /** The prepared document; a graph material's surface names its generated file. */
    const MaterialDocument& GetDocument() const { return m_Document; }
    /** The shader graph the surface was generated from; empty when the material
        has no graph to generate. */
    const std::filesystem::path& GetGraphSourcePath() const { return m_GraphSourcePath; }

    /** Builds the shader package for one variant of the prepared document. On a
        builder whose preparation failed it compiles nothing and returns a failed
        result carrying the preparation errors. */
    Rendering::MaterialBuildResult Build(
        Rendering::ShaderSourceKind kind,
        Rendering::MaterialKeyword pipelineKeywords = Rendering::MaterialKeyword::None,
        Rendering::VertexAttributeFlags meshVertexFlags = Rendering::VertexAttributeFlags::None) const;

private:
    MaterialDocument m_Document;
    std::filesystem::path m_MaterialPath;
    std::string m_DebugName;
    const Rendering::MaterialBuildContext& m_Context;
    std::filesystem::path m_GraphSourcePath;
    std::vector<std::string> m_Errors;
    bool m_Prepared = false;
};
} // namespace GameEngine::Engine::Renderer
