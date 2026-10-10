#pragma once

#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"

namespace GameEngine::Graph
{
struct Model;
}

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine::Engine::Renderer
{

bool IsShaderGraphSurfacePath(const std::filesystem::path& surfaceShaderPath);
bool MergeShaderGraphTagsIntoDocument(const std::filesystem::path& graphGlslPath, MaterialDocument& doc);

/** Generated node-graph live preview materials under Generated/GraphPreview/. */
bool IsGraphLivePreviewMaterialPath(const std::filesystem::path& materialAssetPath);

/** Vertex layout + shader defines for graph materials; preview always uses tangents. */
GameEngine::Rendering::VertexAttributeFlags InferVertexAttributeFlagsFromDocument(
    const MaterialDocument& doc,
    const std::filesystem::path& materialAssetPath);

/** The material-buildable .glsl text (node includes + compiled EvaluateSurface
    body + the semantic @sg-* tags) without touching disk. `outputGlslPath` is
    not written — it anchors the project node-library lookup and is where the
    caller intends the text to land.

    Callers that regenerate a surface repeatedly should materialize, hash, and
    write only on a change: writing re-arms the shader-edit watcher and
    invalidates the shader cache entry for that surface. */
bool MaterializeShaderGraphSurfaceFromModel(const GameEngine::Graph::Model& model,
                                            const std::string& graphName,
                                            const std::filesystem::path& outputGlslPath,
                                            std::string& outSource,
                                            std::vector<std::string>& outErrors);

/** MaterializeShaderGraphSurfaceFromModel plus a write-if-changed. */
bool WriteMaterializedShaderGraphSurfaceFromModel(const GameEngine::Graph::Model& model,
                                                  const std::string& graphName,
                                                  const std::filesystem::path& outputGlslPath,
                                                  std::vector<std::string>& outErrors);

bool WriteMaterializedShaderGraphSurfaceFromFile(const std::filesystem::path& graphGlslPath,
                                                 const std::filesystem::path& outputGlslPath,
                                                 std::vector<std::string>& outErrors);

/** Resolves @sg-graph references, materializes graph surfaces, and adds graph include dirs. */
bool PrepareMaterialDocumentForShaderPackage(
    MaterialDocument& doc,
    const std::filesystem::path& materialPath,
    ::GameEngine::Rendering::MaterialBuildContext& context,
    std::vector<std::string>& outErrors,
    std::filesystem::path* outGraphSourcePath = nullptr);

} // namespace GameEngine::Engine::Renderer
