#pragma once

#include "EditorPanelManager.h"

#include <cstdint>
#include <filesystem>
#include <functional>

namespace GameEngine
{
class AssetViewPanel;
class IThumbnailProvider;

namespace Editor
{

/// What the editor's asset actions (SetEditorAssetActions) act through.
struct EditorAssetActionsHost
{
    /// The editor's thumbnail service; null leaves ShowThumbnail empty.
    IThumbnailProvider* Thumbnails = nullptr;
    /// The window the thumbnail service draws live thumbnails for (EditorContext::ThumbnailHostWindowId).
    uint64_t ThumbnailWindowId = 0;
    /// The editor's one open-asset policy.
    std::function<void(const std::filesystem::path&)> Open;
    /// Preview brings the Asset View forward in `Window` through `Panels` and shows the image in
    /// `AssetView`; a null AssetView leaves Preview doing nothing.
    EditorPanelManager* Panels = nullptr;
    EditorPanelManager::EditorWindowContext* Window = nullptr;
    AssetViewPanel* AssetView = nullptr;
};

/// Installs the editor's asset actions (SetEditorAssetActions) for surfaces with no asset browser
/// of their own: thumbnails from the thumbnail service, Open through the editor's open-asset
/// policy, Preview in the Asset View panel, and the Assets panel menu's Show in Explorer and Copy
/// Full Path (EditorContextMenu).
void InstallEditorAssetActions(EditorAssetActionsHost host);

/// Removes them (the editor is going away).
void UninstallEditorAssetActions();

} // namespace Editor
} // namespace GameEngine
