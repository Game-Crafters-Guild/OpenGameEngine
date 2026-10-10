#include "Engine/Rendering/ShaderGraphMaterial.h"

#include "Graph/SgGraphModelBridge.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/ShaderGraph/SgGraphCompiler.h"
#include "Rendering/ShaderGraph/SgGraphFileIO.h"
#include "Rendering/ShaderGraph/SgPropertyBinding.h"
#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include "Rendering/ShaderGraph/SgPseudoNodes.h"
#include "Rendering/ShaderGraph/SgTagParser.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace GameEngine::Engine::Renderer
{
namespace
{

bool ParseFloat(const std::string& text, float& out)
{
    try
    {
        out = std::stof(text);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::vector<float> ParseVec(const std::string& text)
{
    std::vector<float> values;
    std::string cleaned = text;
    if (!cleaned.empty() && cleaned.front() == '(')
        cleaned.erase(cleaned.begin());
    if (!cleaned.empty() && cleaned.back() == ')')
        cleaned.pop_back();
    std::istringstream iss(cleaned);
    std::string token;
    while (std::getline(iss, token, ','))
    {
        float v = 0.f;
        if (ParseFloat(token, v))
            values.push_back(v);
    }
    return values;
}

std::filesystem::path FindProjectNodesRoot(const std::filesystem::path& graphGlslPath)
{
    std::filesystem::path dir = graphGlslPath.parent_path();
    for (int depth = 0; depth < 8 && !dir.empty(); ++depth)
    {
        if (dir.filename() == "Assets")
            return dir;
        dir = dir.parent_path();
    }
    return {};
}

void CollectNodeIncludeFiles(const ShaderGraph::SgGraphDocument& doc,
                             const ShaderGraph::SgNodeLibraryIndex& library,
                             std::vector<std::string>& outIncludes)
{
    std::unordered_set<std::string> seen;
    for (const auto& node : doc.Nodes)
    {
        const std::string typeId = ShaderGraph::ResolveNodeTypeAlias(node.TypeId);
        const auto it = library.NodesByType.find(typeId);
        if (it == library.NodesByType.end())
            continue;
        const std::string& sourceFile = it->second.SourceFile;
        if (sourceFile.empty() || !seen.insert(sourceFile).second)
            continue;
        outIncludes.push_back(sourceFile);
    }
    std::sort(outIncludes.begin(), outIncludes.end());
}

std::string IncludeLineForSourceFile(const std::string& sourceFile)
{
    const std::filesystem::path nodesRoot = ShaderGraph::GetEngineGraphNodesRoot();
    const std::filesystem::path sourcePath(sourceFile);
    std::error_code ec;
    const std::filesystem::path relativePath = std::filesystem::relative(sourcePath, nodesRoot, ec);
    if (!ec && !relativePath.empty())
        return "#include \"" + relativePath.generic_string() + "\"\n";
    return "#include \"" + sourcePath.generic_string() + "\"\n";
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string BuildMaterializedBody(const ShaderGraph::SgGraphDocument& doc,
                                  const ShaderGraph::SgNodeLibraryIndex& library,
                                  const std::string& compiledBody,
                                  [[maybe_unused]] std::vector<std::string>& outErrors)
{
    std::vector<std::string> includes;
    CollectNodeIncludeFiles(doc, library, includes);

    std::ostringstream body;
    for (const std::string& includePath : includes)
        body << IncludeLineForSourceFile(includePath);
    if (!includes.empty())
        body << '\n';
    body << compiledBody;
    return body.str();
}

// Subdirectory of the shader cache root that holds every materialized graph
// surface. Derived data, never project content.
constexpr const char* kGeneratedSurfaceDirName = "Generated";

// Generated surfaces are derived data outside watched project content. The
// name reaches the composed include and so the shader-package key, so it hashes
// the material's identity in the asset system's derived-identity spelling,
// "<alias>/<path under the root>", taken from the deepest mounted source that
// holds the material. Sources sharing one root spelling tie; the one listed
// first wins, and AssetSourceRoots lists sources in mount priority order, so a
// Player development run that mounts the project and the editor at one folder
// names it "project", as the cook does. Moving the project and its cache, or
// mounting a package or the editor assets from another directory, keeps every
// name; a project material and a package material with the same relative path
// get two.
std::filesystem::path MaterializedSurfacePath(
    const std::filesystem::path& materialPath,
    const ::GameEngine::Rendering::MaterialBuildContext& context)
{
    namespace fs = std::filesystem;
    const fs::path material = fs::absolute(materialPath).lexically_normal();
    fs::path sourceIdentity;
    size_t matchedRootLength = 0;
    for (const auto& source : context.AssetSourceRoots)
    {
        const fs::path root = fs::absolute(source.Root).lexically_normal();
        const fs::path relative = material.lexically_relative(root);
        const size_t rootLength = root.native().size();
        if (relative.empty() || *relative.begin() == ".." || rootLength <= matchedRootLength)
            continue;
        sourceIdentity = fs::path(source.Alias) / relative;
        matchedRootLength = rootLength;
    }
    if (sourceIdentity.empty())
        return {};

    std::ostringstream name;
    name << materialPath.stem().string() << '_' << std::hex << std::setw(16) << std::setfill('0')
         << Hashing::Fnv1a64(sourceIdentity.generic_string()) << "_built.glsl";
    return context.CacheRoot / kGeneratedSurfaceDirName / name.str();
}

bool MaterializeShaderGraphSurface(const ShaderGraph::SgGraphDocument& doc,
                                   const std::string& graphName,
                                   const ShaderGraph::SgNodeLibraryIndex& library,
                                   std::string& outSource,
                                   std::vector<std::string>& outErrors)
{
    const auto compiled = ShaderGraph::SgGraphCompiler::Compile(doc, library);
    if (!compiled.Success)
    {
        outErrors.reserve(outErrors.size() + compiled.Errors.size());
        for (const ShaderGraph::SgDiagnostic& diagnostic : compiled.Errors)
            outErrors.push_back(diagnostic.Message);
        return false;
    }

    ShaderGraph::SgGraphDocument tagsDoc = doc;
    if (tagsDoc.GraphName.empty())
        tagsDoc.GraphName = graphName;
    tagsDoc.VariantDefines = compiled.VariantDefines;
    /* A materialized surface is a build artifact, and only its semantic tags are
       ever read back (@sg-variant by ShaderComposer and the vertex-attribute
       inference, @sg-property/@sg-texture/@sg-lighting from the authoring graph
       that produced it). Node layout, wiring and viewport are authoring state:
       carrying them here means dragging a node or panning the canvas rewrites
       every generated surface, misses the shader cache on content that did not
       change, and recompiles the material. */
    tagsDoc.Materialized = true;
    tagsDoc.Nodes.clear();
    tagsDoc.Edges.clear();
    tagsDoc.ViewportPanX = 0.f;
    tagsDoc.ViewportPanY = 0.f;
    tagsDoc.ViewportZoom = 1.f;
    /* A property's name and type are what the body compiled against; its
       default, range and hint are authoring metadata, and they are read from the
       authoring graph (ResolveSurfaceShaderFromGraph merges tags from the source
       and never from an already-materialized surface). Emitting them here would
       put a value in a file nothing reads, moving on every scrub. */
    for (ShaderGraph::SgGraphProperty& property : tagsDoc.Properties)
    {
        property.DefaultValue.clear();
        property.RangeMin.clear();
        property.RangeMax.clear();
        property.Hint.clear();
        property.IsPublic = false;
    }

    const std::string body = BuildMaterializedBody(doc, library, compiled.Body, outErrors);
    outSource = ShaderGraph::SerializeGraphFileText(tagsDoc, body);
    return true;
}

bool WriteMaterializedShaderGraphSurface(const ShaderGraph::SgGraphDocument& doc,
                                         const std::string& graphName,
                                         const std::filesystem::path& outputGlslPath,
                                         const ShaderGraph::SgNodeLibraryIndex& library,
                                         std::vector<std::string>& outErrors)
{
    std::string source;
    if (!MaterializeShaderGraphSurface(doc, graphName, library, source, outErrors))
        return false;

    if (!ShaderGraph::WriteGraphFileTextIfChanged(outputGlslPath, source))
    {
        outErrors.push_back("ShaderGraphMaterial: failed to write materialized surface to '" +
                            outputGlslPath.string() + "'.");
        return false;
    }
    return true;
}

} // namespace

bool IsGraphLivePreviewMaterialPath(const std::filesystem::path& materialAssetPath)
{
    if (materialAssetPath.empty())
        return false;

    bool sawGenerated = false;
    for (const auto& part : materialAssetPath)
    {
        const std::string lower = LowerAscii(part.string());
        if (lower == "generated")
        {
            sawGenerated = true;
            continue;
        }
        if (sawGenerated && lower == "graphpreview")
            return true;
        sawGenerated = false;
    }

    const std::string stem = LowerAscii(materialAssetPath.stem().string());
    return stem.rfind("live_preview", 0) == 0;
}

GameEngine::Rendering::VertexAttributeFlags InferVertexAttributeFlagsFromDocument(
    const MaterialDocument& doc,
    const std::filesystem::path& materialAssetPath)
{
    using GameEngine::Rendering::VertexAttributeFlags;
    if (materialAssetPath.empty())
        return VertexAttributeFlags::StandardMesh;

    if (IsGraphLivePreviewMaterialPath(materialAssetPath))
        return VertexAttributeFlags::StandardMeshWithTangent;

    if (doc.surfaceShader.empty())
        return VertexAttributeFlags::StandardMesh;

    const std::filesystem::path surfacePath =
        (materialAssetPath.parent_path() / doc.surfaceShader).lexically_normal();
    std::error_code ec;
    if (!std::filesystem::exists(surfacePath, ec))
        return VertexAttributeFlags::StandardMesh;

    std::string content;
    if (!ReadFileTextShared(surfacePath, content))
        return VertexAttributeFlags::StandardMesh;

    if (!ShaderGraph::IsShaderGraphSource(content))
        return VertexAttributeFlags::StandardMesh;

    const auto parsed = ShaderGraph::ParseShaderGraphSource(content);
    const auto graphDoc = ShaderGraph::ParseGraphDocumentFromTags(parsed.TagBlock);
    for (const std::string& def : graphDoc.VariantDefines)
    {
        if (def == "HAS_TANGENT")
            return VertexAttributeFlags::StandardMeshWithTangent;
    }
    return VertexAttributeFlags::StandardMesh;
}

bool IsShaderGraphSurfacePath(const std::filesystem::path& surfaceShaderPath)
{
    if (surfaceShaderPath.empty() || !std::filesystem::exists(surfaceShaderPath))
        return false;
    std::string source;
    if (!ReadFileTextShared(surfaceShaderPath, source))
        return false;
    return ShaderGraph::IsShaderGraphSource(source);
}

bool IsAlreadyMaterializedGraphSurface(const std::filesystem::path& surfaceShaderPath)
{
    if (surfaceShaderPath.empty() || !std::filesystem::exists(surfaceShaderPath))
        return false;
    std::string source;
    if (!ReadFileTextShared(surfaceShaderPath, source))
        return false;
    if (ShaderGraph::IsShaderGraphSource(source)
        && ShaderGraph::ParseGraphDocumentFromTags(ShaderGraph::ParseShaderGraphSource(source).TagBlock).Materialized)
        return true;
    // Surfaces materialized before the tag existed: a built body with the node
    // helper includes the compiler emitted for it.
    return source.find("SurfaceOutput EvaluateSurface") != std::string::npos
        && source.find("#include \"") != std::string::npos;
}

bool ResolveSurfaceShaderFromGraph(MaterialDocument& doc,
                                   const std::filesystem::path& materialPath,
                                   std::filesystem::path& outGraphPath,
                                   std::vector<std::string>& outErrors)
{
    outGraphPath.clear();

    if (!doc.surfaceGraphGuid.empty() || !doc.surfaceGraph.empty())
    {
        std::filesystem::path graphPath = doc.surfaceGraph;
        if (graphPath.empty())
        {
            outErrors.push_back("ShaderGraphMaterial: surfaceGraphGuid set but path could not be resolved.");
            return false;
        }
        if (!graphPath.is_absolute())
            graphPath = (materialPath.parent_path() / graphPath).lexically_normal();

        outGraphPath = graphPath;

        doc.surfaceShader = std::filesystem::relative(graphPath, materialPath.parent_path()).generic_string();
        doc.surfaceGraph.clear();
        doc.surfaceGraphGuid.clear();
    }

    if (doc.surfaceShader.empty())
        return true;

    std::filesystem::path surfacePath = doc.surfaceShader;
    if (!surfacePath.is_absolute())
        surfacePath = (materialPath.parent_path() / surfacePath).lexically_normal();

    if (!IsShaderGraphSurfacePath(surfacePath))
        return true;

    if (IsAlreadyMaterializedGraphSurface(surfacePath))
        return true;

    outGraphPath = surfacePath;
    MergeShaderGraphTagsIntoDocument(surfacePath, doc);
    return true;
}

bool PrepareMaterialDocumentForShaderPackage(
    MaterialDocument& doc,
    const std::filesystem::path& materialPath,
    ::GameEngine::Rendering::MaterialBuildContext& context,
    std::vector<std::string>& outErrors,
    std::filesystem::path* outGraphSourcePath)
{
    outErrors.clear();
    if (outGraphSourcePath)
        outGraphSourcePath->clear();
    if (materialPath.empty())
    {
        outErrors.push_back("ShaderGraphMaterial: empty material path.");
        return false;
    }

    std::filesystem::path graphPath;
    if (!ResolveSurfaceShaderFromGraph(doc, materialPath, graphPath, outErrors))
        return false;

    const auto nodesRoot = ShaderGraph::GetEngineGraphNodesRoot();
    if (nodesRoot.empty())
    {
        // Without this include dir every node helper include fails, so the
        // compile error that follows would name a missing .glsl rather than the
        // missing staging that caused it.
        Logger::Log::Warning(
            "ShaderGraphMaterial: no shader graph nodes root beside this executable "
            "(expected '<exe dir>/Assets/Shaders/Graph/Nodes') — graph material '{}' will fail to "
            "resolve its node helper includes. The tree is staged next to the runtime by the "
            "build (ge_stage_shader_graph_nodes).",
            materialPath.string());
    }
    else
    {
        const auto it = std::find(context.IncludeDirs.begin(), context.IncludeDirs.end(), nodesRoot);
        if (it == context.IncludeDirs.end())
            context.IncludeDirs.push_back(nodesRoot);
    }

    if (graphPath.empty())
        return true;

    if (outGraphSourcePath)
        *outGraphSourcePath = graphPath;

    if (context.CacheRoot.empty())
    {
        outErrors.push_back(
            "ShaderGraphMaterial: no shader cache root — the generated surface for '" +
            materialPath.generic_string() + "' has nowhere to go.");
        return false;
    }

    const std::filesystem::path materializedPath =
        MaterializedSurfacePath(materialPath, context);
    if (materializedPath.empty())
    {
        outErrors.push_back("ShaderGraphMaterial: graph material '" + materialPath.generic_string() +
                            "' is outside every mounted asset source, so its generated surface "
                            "has no stable name. Move it into the project's assets or a package; "
                            "an offline cook names extra roots with --scan.");
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(materializedPath.parent_path(), ec);
    if (ec)
    {
        outErrors.push_back("ShaderGraphMaterial: could not create generated-surface directory '" +
                            materializedPath.parent_path().generic_string() + "': " + ec.message());
        return false;
    }

    if (!WriteMaterializedShaderGraphSurfaceFromFile(graphPath, materializedPath, outErrors))
        return false;

    // The document identifies the physical file; composition spells its include
    // relative to the cache root, keeping host paths out of package keys and WGSL.
    if (std::find(context.IncludeDirs.begin(), context.IncludeDirs.end(), context.CacheRoot)
        == context.IncludeDirs.end())
        context.IncludeDirs.push_back(context.CacheRoot);
    doc.surfaceShader = materializedPath.generic_string();
    return true;
}

bool MergeShaderGraphTagsIntoDocument(const std::filesystem::path& graphGlslPath, MaterialDocument& doc)
{
    const auto file = ShaderGraph::ParseShaderGraphFile(graphGlslPath);
    const auto graphDoc = ShaderGraph::ParseGraphDocumentFromTags(file.TagBlock);

    if (!graphDoc.LightingModel.empty())
        doc.lightingModel = graphDoc.LightingModel;

    ShaderGraph::ApplyShaderGraphPropertiesToMaterialDocument(graphDoc.Properties, doc);

    for (const auto& tex : graphDoc.Textures)
    {
        if (doc.textures.count(tex.Name))
            continue;
        doc.textures[tex.Name] = tex.Guid;
    }

    return true;
}

bool MaterializeShaderGraphSurfaceFromModel(const Graph::Model& model,
                                            const std::string& graphName,
                                            const std::filesystem::path& outputGlslPath,
                                            std::string& outSource,
                                            std::vector<std::string>& outErrors)
{
    outErrors.clear();
    outSource.clear();
    const auto doc = GraphModelToSgDocument(model, graphName);
    const auto& library =
        ShaderGraph::GetSharedNodeLibraryIndex(FindProjectNodesRoot(outputGlslPath));
    return MaterializeShaderGraphSurface(doc, graphName, library, outSource, outErrors);
}

bool WriteMaterializedShaderGraphSurfaceFromModel(const Graph::Model& model,
                                                  const std::string& graphName,
                                                  const std::filesystem::path& outputGlslPath,
                                                  std::vector<std::string>& outErrors)
{
    std::string source;
    if (!MaterializeShaderGraphSurfaceFromModel(model, graphName, outputGlslPath, source, outErrors))
        return false;

    if (!ShaderGraph::WriteGraphFileTextIfChanged(outputGlslPath, source))
    {
        outErrors.push_back("ShaderGraphMaterial: failed to write materialized surface to '" +
                            outputGlslPath.string() + "'.");
        return false;
    }
    return true;
}

bool WriteMaterializedShaderGraphSurfaceFromFile(const std::filesystem::path& graphGlslPath,
                                                 const std::filesystem::path& outputGlslPath,
                                                 std::vector<std::string>& outErrors)
{
    outErrors.clear();
    if (graphGlslPath.empty() || outputGlslPath.empty())
    {
        outErrors.push_back("ShaderGraphMaterial: empty graph or output path.");
        return false;
    }

    const auto file = ShaderGraph::LoadGraphFile(graphGlslPath);
    const auto& library =
        ShaderGraph::GetSharedNodeLibraryIndex(FindProjectNodesRoot(graphGlslPath));
    const std::string graphName = file.Document.GraphName.empty()
                                      ? graphGlslPath.stem().string()
                                      : file.Document.GraphName;
    return WriteMaterializedShaderGraphSurface(file.Document, graphName, outputGlslPath, library,
                                               outErrors);
}

} // namespace GameEngine::Engine::Renderer
