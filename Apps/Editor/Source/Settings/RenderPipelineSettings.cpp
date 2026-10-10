#include "Editor/Settings/RenderPipelineSettings.h"
#include "Editor/Settings/SettingsStore.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"

#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace
{

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
    {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }
    return true;
}

std::string TryMakeAssetRelativePathString(const GameEngine::AssetManager& assetManager,
                                           const std::filesystem::path& absoluteAssetPath)
{
    if (absoluteAssetPath.empty() || !absoluteAssetPath.is_absolute())
        return {};

    auto tryRel = [&](const std::filesystem::path& root) -> std::string
    {
        if (root.empty())
            return {};

        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(absoluteAssetPath, root, ec);
        if (ec)
            return {};

        const std::string s = rel.generic_string();
        if (s.empty() || s == "." || s.rfind("..", 0) == 0)
            return {};

        return s;
    };

    if (std::string s = tryRel(assetManager.GetAssetRoot()); !s.empty())
        return s;

    for (const auto& source : assetManager.GetRegisteredSources())
    {
        if (std::string s = tryRel(source.Root); !s.empty())
            return s;
    }

    return {};
}

std::filesystem::path TryExtractRenderPipelinesTail(const std::filesystem::path& absolutePath)
{
    if (absolutePath.empty())
        return {};

    std::filesystem::path tail;
    bool foundRenderPipelines = false;
    for (const auto& part : absolutePath)
    {
        const std::string segment = part.generic_string();
        if (!foundRenderPipelines)
        {
            if (!EqualsIgnoreCaseAscii(segment, "RenderPipelines"))
                continue;
            foundRenderPipelines = true;
        }

        tail /= part;
    }

    if (!foundRenderPipelines)
        return {};
    return tail.lexically_normal();
}

bool DoesRelativeAssetExist(const GameEngine::AssetManager& assetManager,
                            const std::filesystem::path& relativeAssetPath)
{
    if (relativeAssetPath.empty() || relativeAssetPath.is_absolute())
        return false;

    const std::filesystem::path resolvedPath = assetManager.ResolveAssetPath(relativeAssetPath);
    if (resolvedPath.empty())
        return false;

    std::error_code ec;
    return std::filesystem::exists(resolvedPath, ec);
}

} // namespace

namespace GameEngine::Editor
{

std::filesystem::path NormalizeConfiguredRenderPipelinePath(const std::filesystem::path& configuredPath,
                                                           const AssetManager& assetManager,
                                                           bool& outWasMigrated)
{
    outWasMigrated = false;
    if (configuredPath.empty())
        return {};

    std::filesystem::path normalized = configuredPath.lexically_normal();
    if (!normalized.is_absolute())
    {
        std::string generic = normalized.generic_string();
        constexpr const char* kAssetsPrefix = "Assets/";
        if (generic.rfind(kAssetsPrefix, 0) == 0)
        {
            normalized = std::filesystem::path(generic.substr(std::char_traits<char>::length(kAssetsPrefix))).lexically_normal();
            outWasMigrated = true;
        }

        generic = normalized.generic_string();
        if (generic.empty() || generic == "." || generic.rfind("..", 0) == 0)
            return {};
        return normalized;
    }

    outWasMigrated = true;

    if (const std::string rel = TryMakeAssetRelativePathString(assetManager, normalized); !rel.empty())
        return std::filesystem::path(rel).lexically_normal();

    if (const std::filesystem::path tail = TryExtractRenderPipelinesTail(normalized); !tail.empty() &&
        DoesRelativeAssetExist(assetManager, tail))
    {
        return tail;
    }

    if (!normalized.filename().empty())
    {
        const std::filesystem::path byName = (std::filesystem::path("RenderPipelines") / normalized.filename()).lexically_normal();
        if (DoesRelativeAssetExist(assetManager, byName))
            return byName;
    }

    return {};
}

bool PersistActiveRenderPipelinePathToProjectSettings(SettingsStore& store,
                                                      const std::filesystem::path& pipelinePath,
                                                      const char* contextLabel)
{
    auto& root = store.Json();
    if (!root.is_object())
        root = nlohmann::json::object();

    auto& rendering = root["rendering"];
    if (!rendering.is_object())
        rendering = nlohmann::json::object();

    rendering["activeRenderPipeline"] = pipelinePath.generic_string();

    std::string saveErr;
    if (!store.Save(&saveErr))
    {
        Logger::Log::Warning("Editor: {} failed to persist active render pipeline setting '{}': {}",
                             contextLabel ? contextLabel : "active-render-pipeline-save",
                             pipelinePath.generic_string(),
                             saveErr);
        return false;
    }
    return true;
}

std::filesystem::path LoadActiveRenderPipelinePathFromProjectSettings(const std::filesystem::path& workspaceRoot)
{
    SettingsStore store = OpenProjectSettings(workspaceRoot);
    std::string err;
    (void)store.Load(&err);

    const auto& root = store.Json();
    if (!root.is_object())
        return {};

    const auto itR = root.find("rendering");
    if (itR == root.end() || !itR->is_object())
        return {};

    std::filesystem::path configuredPath;
    const auto& r = *itR;
    try
    {
        const auto itP = r.find("activeRenderPipeline");
        if (itP != r.end() && itP->is_string())
        {
            const std::string s = itP->get<std::string>();
            if (!s.empty())
                configuredPath = std::filesystem::path(s);
        }
    }
    catch (...)
    {
    }

    if (configuredPath.empty())
        return {};

    auto& assetManager = EngineCore::GetInstance().GetAssetManager();
    bool wasMigrated = false;
    const std::filesystem::path normalizedPath =
        NormalizeConfiguredRenderPipelinePath(configuredPath, assetManager, wasMigrated);

    if (normalizedPath.empty())
    {
        if (configuredPath.is_absolute())
        {
            const std::filesystem::path fallback = std::filesystem::path("RenderPipelines/ForwardPlus.rendergraph");
            Logger::Log::Warning(
                "Editor: active render pipeline setting '{}' is absolute and cannot be resolved in current asset roots; "
                "falling back to '{}'.",
                configuredPath.generic_string(),
                fallback.generic_string());
            (void)PersistActiveRenderPipelinePathToProjectSettings(store, fallback, "active-render-pipeline-fallback");
            return fallback;
        }

        Logger::Log::Warning("Editor: ignoring invalid active render pipeline setting '{}'.",
                             configuredPath.generic_string());
        return {};
    }

    const std::string configuredValue = configuredPath.lexically_normal().generic_string();
    const std::string normalizedValue = normalizedPath.generic_string();
    if (wasMigrated || configuredValue != normalizedValue)
    {
        Logger::Log::Info("Editor: normalized active render pipeline setting '{}' -> '{}'.",
                          configuredValue,
                          normalizedValue);
        (void)PersistActiveRenderPipelinePathToProjectSettings(store, normalizedPath, "active-render-pipeline-migrate");
    }

    return normalizedPath;
}

} // namespace GameEngine::Editor
