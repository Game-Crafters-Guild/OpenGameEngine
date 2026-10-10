#include "Engine/Build/PackagedGameLayout.h"

#include <system_error>

namespace GameEngine {

PackagedGameLayout DetectPackagedGameLayout(const std::filesystem::path& contentRoot,
                                            const std::string& scriptAssemblyPath)
{
    PackagedGameLayout layout;
    layout.ContentRoot = contentRoot;

    std::error_code ec;
    const bool hasGameConfig = std::filesystem::exists(contentRoot / "game.config", ec);
    const bool hasAssetManifest =
        std::filesystem::exists(contentRoot / "Assets" / ".assetmanifest", ec);
    layout.IsPackaged = hasGameConfig && hasAssetManifest;

    if (!scriptAssemblyPath.empty())
    {
        std::filesystem::path configured = scriptAssemblyPath;
        if (configured.is_relative())
            configured = contentRoot / configured;
        layout.PrebuiltScriptAssembly = configured.lexically_normal();
        layout.PrebuiltScriptAssemblyExists =
            std::filesystem::exists(layout.PrebuiltScriptAssembly, ec);
    }

    return layout;
}

ScriptsConfig BuildPlayerScriptsConfig(const PackagedGameLayout& layout, bool useNativeAOT)
{
    ScriptsConfig config;
    config.packagedMode = layout.IsPackaged;
    config.enableHotReload = false;
    config.enableAsyncHotReload = false;
    config.deferInitialLoad = false;
    config.enableAutoProjectGeneration = false;

    // A configured-but-missing assembly stays out of the config: the CLR then
    // stays off (incomplete package) instead of ScriptManager failing the load.
    // The caller owns the loud error for that case.
    if (!useNativeAOT && layout.PrebuiltScriptAssemblyExists)
        config.prebuiltAssemblyPath = layout.PrebuiltScriptAssembly;

    config.disableClr = useNativeAOT || config.prebuiltAssemblyPath.empty();
    return config;
}

bool PlayerNeedsManagedSystemBridge(const ScriptsConfig& config, bool useNativeAOT)
{
    return useNativeAOT || !config.disableClr;
}

std::optional<AssetSourceDesc> BuildPlayerEditorSourceDesc(
    const std::filesystem::path& projectAssetsRoot,
    const std::filesystem::path& stagedAssetsRoot)
{
    const auto canonicalAssetRoot = [](const std::filesystem::path& root) {
        std::error_code canonEc;
        return std::filesystem::weakly_canonical(root, canonEc).lexically_normal();
    };

    std::error_code ec;
    const std::filesystem::path editorRoot =
        std::filesystem::is_directory(stagedAssetsRoot, ec) ? stagedAssetsRoot : projectAssetsRoot;

    AssetSourceDesc editorSource;
    editorSource.Alias = std::string(kAssetSourceAliasEditor);
    editorSource.Root = editorRoot;
    editorSource.DerivedIdentity = true;
    editorSource.Priority = 50;
    editorSource.RegisterFileWatcher = false;

    if (canonicalAssetRoot(editorRoot) != canonicalAssetRoot(projectAssetsRoot))
        return editorSource;

    // Packaged layout: the manifest mount at the same root rejects overlapping
    // sources, and editor-owned content was fused flat into Assets/ at build
    // time anyway — there is nothing distinct to mount.
    if (std::filesystem::exists(projectAssetsRoot / ".assetmanifest", ec))
        return std::nullopt;

    // Same folder on disk (dev run without a staged root). Render pipelines
    // still resolve `editor:` shader paths, so keep the alias but skip the
    // scan and watcher — registering the root twice starts duplicate scans
    // and file watchers, which thrashes the registry after the first frames.
    editorSource.RequiresScan = false;
    return editorSource;
}

} // namespace GameEngine
