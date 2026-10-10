#include "Editor/Settings/AssetPreviewSettings.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "Thumbnails/ModelThumbnailHandler.h"

#include <string>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kPrefKeyPreviewScrollZoom = "ui.previewScrollZoom";
} // namespace

void ApplyAssetPreviewSettingsFromPreferences()
{
    auto prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    bool scrollZoom = true;
    prefs.TryGetBool(kPrefKeyPreviewScrollZoom, scrollZoom);
    ModelThumbnailHandler::SetPreviewScrollZoomEnabled(scrollZoom);
}

void RegisterAssetPreviewSettingsCategory()
{
    SettingsCategoryDescriptor assetPreview;
    assetPreview.CategoryId = "assetPreview";
    assetPreview.Title = "Asset Preview";
    assetPreview.Group = SettingsCategoryGroup::UI;
    assetPreview.TreeRowClass = "ui-asset-preview-row";
    assetPreview.SearchKeywords = "asset preview scroll wheel zoom dolly model 3d";

    SettingsFieldDescriptor scrollZoom;
    scrollZoom.Label = "Scroll Wheel Zoom Preview";
    scrollZoom.Tooltip =
        "Plain mouse-wheel scroll over the Asset View 3D preview dollies in and out. "
        "When off, only the bound Asset Preview shortcuts affect the preview.";
    scrollZoom.SearchKeywords = "scroll wheel zoom preview dolly";
    scrollZoom.PrefKey = kPrefKeyPreviewScrollZoom;
    SettingsFieldDescriptor::ToggleField control;
    control.DefaultValue = true;
    control.Set = [](bool enabled)
    { ModelThumbnailHandler::SetPreviewScrollZoomEnabled(enabled); };
    scrollZoom.Control = std::move(control);
    assetPreview.Fields.push_back(std::move(scrollZoom));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(assetPreview));
}

} // namespace GameEngine::Editor
