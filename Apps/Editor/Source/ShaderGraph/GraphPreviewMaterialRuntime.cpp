#include "ShaderGraph/GraphPreviewMaterialRuntime.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "EditorContext.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Thumbnails/ModelThumbnailHandler.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>

namespace GameEngine {
namespace Editor {
namespace {

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

GraphPreviewCacheSource ResolveGraphPreviewCacheSource(const EditorContext* ctx)
{
    if (!ctx || !ctx->Assets)
        return {ctx ? ctx->AssetsRoot : std::filesystem::path{}, {}};

    GraphPreviewCacheSource firstSource;
    for (const auto& source : ctx->Assets->GetRegisteredSources())
    {
        const std::filesystem::path root = source.Root.lexically_normal();
        if (root.empty())
            continue;

        if (firstSource.Root.empty())
            firstSource = {root, source.Alias};

        if (LowerAscii(source.Alias) == "editor"
            || LowerAscii(root.filename().string()) == "editorassets")
            return {root, source.Alias};
    }

    if (!firstSource.Root.empty())
        return firstSource;

    return {ctx->AssetsRoot, {}};
}

GUID SyncGraphPreviewMaterial(const EditorContext* ctx, const std::filesystem::path& matPath,
                              const MaterialDocument& doc, GraphPreviewCompile compile,
                              const std::string& preferredSourceAlias)
{
    if (!ctx || !ctx->Assets || matPath.empty())
        return {};

    AssetManager& assets = *ctx->Assets;
    AssetRegistry& registry = assets.GetRegistry();

    if (registry.GetAssetGUID(matPath).IsNull() && std::filesystem::exists(matPath))
    {
        const bool registered = preferredSourceAlias.empty()
                                    ? registry.RegisterAsset(matPath)
                                    : registry.RegisterAsset(matPath, preferredSourceAlias);
        if (!registered && !preferredSourceAlias.empty())
            registry.RegisterAsset(matPath);
    }

    const GUID matGuid = registry.GetAssetGUID(matPath);
    if (matGuid.IsNull())
    {
        Logger::Log::Warning("GraphPreview: failed to resolve material GUID for '{}'",
                             matPath.string());
        return {};
    }

    const bool propertyOnly = compile == GraphPreviewCompile::PropertiesOnly;
    if (!propertyOnly)
        assets.ReloadAssetNow(matGuid);

    if (ctx->RenderServices)
    {
        auto& materials = ctx->RenderServices->Materials();
        if (propertyOnly)
        {
            if (!materials.Registry().Find(matGuid))
                materials.RegisterMaterialFromDocument(matGuid, doc);
            else
                materials.Compiler().PushMaterialDocumentPropertiesToRuntime(matGuid, doc,
                                                                            *ctx->RenderServices);
        }
        else
        {
            materials.RegisterMaterialFromDocument(
                matGuid, doc, Rendering::MaterialKeyword::None,
                Engine::Renderer::MaterialSystem::BaseCompileMode::Async);
            /* Register alone is not enough on a re-registration: it rebuilds only
               when the compile SPEC moved, and a regenerated preview surface
               keeps its path while its contents change. */
            materials.RecompileMaterialPipeline(matGuid, doc);
        }
    }

    ModelThumbnailHandler::InvalidateMaterialThumbnail(matGuid, propertyOnly);
    return matGuid;
}

bool WriteGraphPreviewMaterialFile(const std::filesystem::path& matPath, const MaterialDocument& doc)
{
    const std::string text = SerializeMaterialDocument(doc).dump(2);
    GameEngine::String existing;
    if (GameEngine::ReadFileTextShared(matPath, existing) && std::string_view(existing) == text)
        return true;
    std::ofstream out(matPath);
    if (!out.is_open())
        return false;
    out << text;
    return static_cast<bool>(out);
}

} // namespace Editor
} // namespace GameEngine
