#include "Editor/Settings/SvgRasterSettings.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/SvgRasterizer.h"
#include "Core/Engine.h"
#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr int kMinRasterExp = 4;
constexpr int kMaxRasterExp = 14;
constexpr int kDefaultRasterExp = 8;
constexpr const char* kUiSvgRasterExpPref = "graphics.uiSvgRasterExp";
constexpr const char* kTextureSvgRasterExpPref = "import.svgTextureRasterExp";
constexpr const char* kLegacySvgRasterExpPref = "graphics.svgRasterExp";

int PixelsToExponent(uint32_t pixels)
{
    pixels = std::clamp(pixels, kMinSvgRasterSize, kMaxSvgRasterSize);
    int exponent = kMinRasterExp;
    while (exponent < kMaxRasterExp && (1u << exponent) < pixels)
        ++exponent;
    return exponent;
}

void ApplyValue(SvgRasterSettingsScope scope, uint32_t pixels)
{
    if (scope == SvgRasterSettingsScope::EditorUi)
        SetSvgRasterizerUserScale(static_cast<float32>(pixels));
    else
        SetSvgTextureRasterizerDefaultSize(static_cast<float32>(pixels));
}

void ReloadAffectedSvgAssets(SvgRasterSettingsScope scope)
{
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    auto& registry = assets.GetRegistry();
    for (const AssetSourceDesc& source : assets.GetRegisteredSources())
    {
        const bool sourceUsesUiDefault = source.Alias == kAssetSourceAliasEditor;
        if (sourceUsesUiDefault != (scope == SvgRasterSettingsScope::EditorUi))
            continue;

        for (const GUID& guid : registry.GetAssetsForSource(source.Alias))
        {
            AssetMetadata metadata{};
            if (!registry.TryGetAssetMetadata(guid, metadata))
                continue;
            std::string extension = metadata.Path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension == ".svg")
                assets.RequestAsyncReload(guid);
        }
    }
}
} // namespace

uint32_t SvgRasterSettings::Load(SvgRasterSettingsScope scope,
                                 const std::filesystem::path& workspaceRoot,
                                 std::string* outError)
{
    double storedExponent = static_cast<double>(kDefaultRasterExp);
    std::string error;

    if (scope == SvgRasterSettingsScope::EditorUi)
    {
        SettingsStore preferences = OpenEditorPreferences();
        preferences.Load(&error);
        if (!preferences.TryGetDouble(kUiSvgRasterExpPref, storedExponent))
            preferences.TryGetDouble(kLegacySvgRasterExpPref, storedExponent);
    }
    else
    {
        SettingsStore project = OpenProjectSettings(workspaceRoot);
        project.Load(&error);
        if (!project.TryGetDouble(kTextureSvgRasterExpPref, storedExponent))
        {
            // Preserve the former combined preference until this project saves
            // an independent texture-import default for the first time.
            SettingsStore preferences = OpenEditorPreferences();
            std::string preferencesError;
            preferences.Load(&preferencesError);
            preferences.TryGetDouble(kLegacySvgRasterExpPref, storedExponent);
            if (error.empty())
                error = std::move(preferencesError);
        }
    }

    if (outError)
        *outError = error;
    const int exponent = std::clamp(static_cast<int>(std::round(storedExponent)),
                                    kMinRasterExp, kMaxRasterExp);
    return 1u << exponent;
}

bool SvgRasterSettings::SaveAndApply(SvgRasterSettingsScope scope,
                                     const std::filesystem::path& workspaceRoot,
                                     uint32_t pixels,
                                     bool reloadAssets,
                                     std::string* outError)
{
    const int exponent = PixelsToExponent(pixels);
    const uint32_t normalizedPixels = 1u << exponent;
    SettingsStore settings = scope == SvgRasterSettingsScope::EditorUi
                                 ? OpenEditorPreferences()
                                 : OpenProjectSettings(workspaceRoot);
    std::string error;
    settings.Load(&error);
    settings.SetDouble(scope == SvgRasterSettingsScope::EditorUi
                           ? kUiSvgRasterExpPref
                           : kTextureSvgRasterExpPref,
                       static_cast<double>(exponent));
    const bool saved = settings.Save(&error);
    if (!saved)
    {
        if (outError)
            *outError = error;
        return false;
    }

    ApplyValue(scope, normalizedPixels);
    if (reloadAssets)
        ReloadAffectedSvgAssets(scope);
    if (outError)
        outError->clear();
    return true;
}

void SvgRasterSettings::ApplySavedDefaults(const std::filesystem::path& workspaceRoot)
{
    std::string uiError;
    std::string textureError;
    ApplyValue(SvgRasterSettingsScope::EditorUi,
               Load(SvgRasterSettingsScope::EditorUi, workspaceRoot, &uiError));
    ApplyValue(SvgRasterSettingsScope::TextureImport,
               Load(SvgRasterSettingsScope::TextureImport, workspaceRoot, &textureError));
    if (!uiError.empty())
        Logger::Log::Warning("SvgRasterSettings: failed to load editor preference: {}", uiError);
    if (!textureError.empty())
        Logger::Log::Warning("SvgRasterSettings: failed to load project setting: {}", textureError);
}

} // namespace GameEngine::Editor
