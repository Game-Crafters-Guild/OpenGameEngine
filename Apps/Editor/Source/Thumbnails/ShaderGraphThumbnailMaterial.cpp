#include "Thumbnails/ShaderGraphThumbnailMaterial.h"

#include "Assets/AssetManager.h"
#include "Assets/MaterialAsset.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Logger/Logger.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Thumbnails/ModelThumbnailHandler.h"

#include <fstream>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <nlohmann/json.hpp>

namespace GameEngine::ShaderGraphThumbnailMaterial
{
namespace
{

std::mutex g_BuildMutex;

struct ThumbnailPaths
{
    std::filesystem::path MaterialPath;
    std::filesystem::path BuiltGlslPath;
};

bool ResolvePaths(const std::filesystem::path& assetsRoot,
                  const std::filesystem::path& graphGlslPath,
                  ThumbnailPaths& out)
{
    if (assetsRoot.empty() || graphGlslPath.empty())
        return false;

    std::filesystem::path rel = graphGlslPath;
    std::error_code ec;
    if (graphGlslPath.is_absolute())
    {
        rel = std::filesystem::relative(graphGlslPath, assetsRoot, ec);
        if (ec || rel.empty())
            rel = graphGlslPath.filename();
    }

    const std::string stem = rel.stem().string();
    const std::filesystem::path thumbDir =
        assetsRoot / "Generated" / "ShaderGraphThumbnails" / rel.parent_path();

    out.MaterialPath = thumbDir / (stem + ".thumb.material");
    out.BuiltGlslPath = thumbDir / (stem + ".thumb_built.glsl");
    return true;
}

bool NeedsRebuild(const std::filesystem::path& graphGlslPath, const ThumbnailPaths& paths)
{
    std::error_code ec;
    if (!std::filesystem::exists(paths.MaterialPath, ec) || !std::filesystem::exists(paths.BuiltGlslPath, ec))
        return true;

    const auto graphTime = std::filesystem::last_write_time(graphGlslPath, ec);
    if (ec)
        return true;

    const auto matTime = std::filesystem::last_write_time(paths.MaterialPath, ec);
    if (ec || graphTime > matTime)
        return true;

    const auto builtTime = std::filesystem::last_write_time(paths.BuiltGlslPath, ec);
    return ec || graphTime > builtTime;
}

bool WriteMaterialFile(const std::filesystem::path& path, const MaterialDocument& doc)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path);
    if (!out.is_open())
        return false;
    out << SerializeMaterialDocument(doc).dump(2);
    return static_cast<bool>(out);
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

std::filesystem::path ResolveMaterialPath(const std::filesystem::path& assetsRoot,
                                          const std::filesystem::path& graphGlslPath)
{
    ThumbnailPaths paths;
    if (!ResolvePaths(assetsRoot, graphGlslPath, paths))
        return {};
    return paths.MaterialPath;
}

bool IsDerivedThumbnailMaterialPath(const std::filesystem::path& materialPath)
{
    if (materialPath.empty())
        return false;
    const std::string filename = LowerAscii(materialPath.filename().string());
    if (filename.size() < 15 || filename.rfind(".thumb.material") != filename.size() - 15)
        return false;
    return IsDerivedThumbnailAssetPath(materialPath);
}

bool IsDerivedThumbnailAssetPath(const std::filesystem::path& assetPath)
{
    if (assetPath.empty())
        return false;
    const std::string path = LowerAscii(assetPath.generic_string());
    return path.find("generated/shadergraphthumbnails/") != std::string::npos ||
           path.find("generated\\shadergraphthumbnails\\") != std::string::npos;
}

std::filesystem::path EnsureMaterial(AssetManager& assets, const std::filesystem::path& graphGlslPath)
{
    if (graphGlslPath.empty())
        return {};

    std::filesystem::path resolvedGraph = graphGlslPath;
    if (!resolvedGraph.is_absolute())
        resolvedGraph = assets.ResolveAssetPath(resolvedGraph);

    std::error_code ec;
    if (!std::filesystem::exists(resolvedGraph, ec))
    {
        Logger::Log::Warning("ShaderGraphThumbnail: graph file not found '{}'", resolvedGraph.string());
        return {};
    }

    const std::filesystem::path& assetsRoot = assets.GetAssetRoot();
    ThumbnailPaths paths;
    if (!ResolvePaths(assetsRoot, resolvedGraph, paths))
        return {};

    std::lock_guard<std::mutex> lock(g_BuildMutex);

    const bool rebuild = NeedsRebuild(resolvedGraph, paths);
    if (rebuild)
    {
        std::error_code createDirError;
        std::filesystem::create_directories(paths.BuiltGlslPath.parent_path(), createDirError);
        if (createDirError)
        {
            Logger::Log::Warning("ShaderGraphThumbnail: failed to create '{}'",
                                 paths.BuiltGlslPath.parent_path().string());
            return {};
        }

        std::vector<std::string> errors;
        if (!Engine::Renderer::WriteMaterializedShaderGraphSurfaceFromFile(
                resolvedGraph, paths.BuiltGlslPath, errors))
        {
            if (!errors.empty())
                Logger::Log::Warning("ShaderGraphThumbnail: {}", errors.front());
            else
                Logger::Log::Warning("ShaderGraphThumbnail: failed to build '{}'", paths.BuiltGlslPath.string());
            return {};
        }

        MaterialDocument matDoc = MaterialDocument::CreateDefaultPBR(resolvedGraph.stem().string());
        matDoc.surfaceGraph.clear();
        matDoc.surfaceGraphGuid.clear();
        matDoc.surfaceShader = paths.BuiltGlslPath.string();
        Engine::Renderer::MergeShaderGraphTagsIntoDocument(resolvedGraph, matDoc);

        if (!WriteMaterialFile(paths.MaterialPath, matDoc))
        {
            Logger::Log::Warning("ShaderGraphThumbnail: failed to write '{}'", paths.MaterialPath.string());
            return {};
        }
    }

    // Single register-and-resolve entry point; the RegisterAsset + raw
    // GetAssetGUID pair it replaces performed the same sequence.
    const GUID matGuid = assets.ResolveAssetGuid(paths.MaterialPath);
    if (matGuid.IsNull())
        return {};

    if (rebuild)
    {
        assets.ReloadAssetNow(matGuid);
        ModelThumbnailHandler::InvalidateMaterialThumbnail(matGuid, false);
    }

    return paths.MaterialPath;
}

} // namespace GameEngine::ShaderGraphThumbnailMaterial
