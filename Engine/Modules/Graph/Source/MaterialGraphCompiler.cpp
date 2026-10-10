#include "Graph/MaterialGraphCompiler.h"

#include "AssetCore/SharedFileRead.h"
#include "Graph/GraphGlslAuthoring.h"
#include "Graph/SgGraphModelBridge.h"

#include "Rendering/ShaderGraph/SgGraphCompiler.h"
#include "Rendering/ShaderGraph/SgGraphFileIO.h"
#include "Rendering/ShaderGraph/SgNodeReflector.h"
#include "Rendering/ShaderGraph/SgTagParser.h"

#include <fstream>

namespace GameEngine
{
namespace
{

const ShaderGraph::SgNodeLibraryIndex& BuildLibrary()
{
    return ShaderGraph::GetSharedNodeLibraryIndex();
}

} // namespace

MaterialGraphCompileResult MaterialGraphCompiler::Compile(const Graph::Model& model)
{
    MaterialGraphCompileResult result;
    if (model.KindId != Graph::kKindIdMaterial)
    {
        result.errors.push_back({"MaterialGraphCompiler: graph kind must be 'material'.", {}});
        return result;
    }

    const auto doc = GraphModelToSgDocument(model);
    const auto library = BuildLibrary();
    const auto compiled = ShaderGraph::SgGraphCompiler::Compile(doc, library);
    result.success = compiled.Success;
    result.glslSource = compiled.Body;
    result.errors = compiled.Errors;

    const auto vertexPos = result.glslSource.find("\nvec3 ModifyVertex");
    if (vertexPos != std::string::npos)
    {
        result.vertexModifierSource = result.glslSource.substr(vertexPos + 1);
        result.glslSource = result.glslSource.substr(0, vertexPos + 1);
    }
    return result;
}

bool MaterialGraphCompiler::WriteCompiledSurfaceToFile(const std::string& glslSource,
                                                       const std::filesystem::path& outputPath)
{
    std::ofstream out(outputPath);
    if (!out)
        return false;
    out << glslSource;
    return true;
}

bool MaterialGraphCompiler::WriteCompiledVertexModifierToFile(const std::string& glslSource,
                                                              const std::filesystem::path& outputPath)
{
    return WriteCompiledSurfaceToFile(glslSource, outputPath);
}

MaterialGraphCompileResult MaterialGraphCompiler::CompileFromFile(const std::filesystem::path& graphPath)
{
    MaterialGraphCompileResult result;
    if (graphPath.extension() == ".glsl")
    {
        const auto file = ShaderGraph::LoadGraphFile(graphPath);
        Graph::Model model;
        if (!Graph::LoadModelFromShaderGraphComments(file.TagBlock, model))
        {
            result.errors.push_back({"MaterialGraphCompiler: invalid graph JSON fence.", {}});
            return result;
        }
        return Compile(model);
    }

    Graph::Model model;
    String text;
    if (!ReadFileTextShared(graphPath, text))
    {
        result.errors.push_back({"MaterialGraphCompiler: cannot open graph file.", {}});
        return result;
    }
    if (!Graph::FromJson(text, model))
    {
        result.errors.push_back({"MaterialGraphCompiler: invalid graph JSON.", {}});
        return result;
    }
    return Compile(model);
}

} // namespace GameEngine
