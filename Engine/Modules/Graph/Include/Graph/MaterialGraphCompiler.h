#pragma once

#include "Graph/GraphModel.h"

#include "Rendering/ShaderGraph/SgTypes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{

struct MaterialGraphCompileResult
{
    bool success = false;
    std::string glslSource;
    std::string vertexModifierSource;
    // Node-tagged: a diagnostic carrying a NodeId can be pointed at in the editor.
    std::vector<ShaderGraph::SgDiagnostic> errors;
};

// Compiles a material-kind Graph::Model into a GLSL surface shader implementing
// SurfaceOutput EvaluateSurface(SurfaceInput sIn). Output is compatible with
// Engine/Modules/Rendering/Shaders/Includes/surface_io.glsl.
class MaterialGraphCompiler
{
  public:
    static MaterialGraphCompileResult Compile(const Graph::Model& model);

    static bool WriteCompiledSurfaceToFile(const std::string& glslSource,
                                           const std::filesystem::path& outputPath);
    static bool WriteCompiledVertexModifierToFile(const std::string& glslSource,
                                                  const std::filesystem::path& outputPath);

    // Load a .graph JSON file and compile. Convenience for MaterialCompiler.
    static MaterialGraphCompileResult CompileFromFile(const std::filesystem::path& graphPath);
};

} // namespace GameEngine
