#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::Editor
{

/// The class ShowThumbnail gives an element while its picture is not ready (a material or model
/// whose thumbnail is still rendering, an image being downscaled): the surface styles it as
/// loading. Removed when the picture is shown.
inline constexpr const char* kThumbnailPendingClass = "thumbnail-pending";
/// The class ShowThumbnail gives an element whose picture did not come: a material or model the
/// service could not cache and whose live render is not drawing (the Asset View's preview holds
/// it). The surface styles it as having no preview.
inline constexpr const char* kThumbnailUnavailableClass = "thumbnail-unavailable";

/// What the editor does with an asset for a surface that has no asset browser of its own (a
/// package's panel): the editor's thumbnail service, its one open-asset policy and the Asset View
/// preview. Set by the editor at startup (InstallEditorAssetActions); empty actions, which do
/// nothing, where no editor set them.
struct EditorAssetActions
{
    /// Shows `asset`'s thumbnail (an asset, or an image file anywhere on disk), about `sizePx` on its
    /// long edge, as `element`'s background: at once when the thumbnail service has a picture,
    /// otherwise when it has one, on the element's UI thread, provided the element still exists
    /// then; meanwhile the element carries kThumbnailPendingClass. A material or model whose
    /// picture is still rendering waits up to 30 seconds for the cached picture, then shows
    /// the live render if it is drawing, or carries kThumbnailUnavailableClass. An image file
    /// larger than `sizePx` is shown as the service's downscaled copy, never loaded at full size.
    std::function<void(UIElement& element, const std::filesystem::path& asset, int sizePx)> ShowThumbnail;
    /// Shows `image` (an image file the editor wrote, a capture) as `element`'s background at about
    /// its display size: the thumbnail service's copy at most 1024 px on its long edge, or the file
    /// itself when it is no larger, on the element's UI thread, provided the element still exists
    /// then; meanwhile the element carries kThumbnailPendingClass, and kThumbnailUnavailableClass
    /// when there is no picture.
    std::function<void(UIElement& element, const std::filesystem::path& image)> ShowImage;
    /// Opens `asset` the way an Assets browser double-click does (a material is selected with the
    /// Inspector brought forward, a graph opens in its graph panel, a file nothing in the editor
    /// takes goes to the operating system's application).
    std::function<void(const std::filesystem::path& asset)> Open;
    /// Shows `image` (an asset, or an image file anywhere on disk) in the Asset View panel, fitted to
    /// the panel, bringing the panel forward.
    std::function<void(const std::filesystem::path& image)> Preview;
    /// Shows `file` selected in the operating system's file manager, as the Assets panel's
    /// context menu does (its item reads ShowInFileManagerLabel()).
    std::function<void(const std::filesystem::path& file)> ShowInFileManager;
    /// Puts `file`'s full path on the clipboard, as the Assets panel's Copy Full Path does.
    std::function<void(const std::filesystem::path& file)> CopyPath;
};

/// The Assets panel's wording for revealing a file: "Show in Explorer" on Windows, "Show in
/// Finder" on macOS, "Show in File Manager" elsewhere.
std::string ShowInFileManagerLabel();

const EditorAssetActions& GetEditorAssetActions();
void SetEditorAssetActions(EditorAssetActions actions);

/// Sets `element`'s background to an image the thumbnail service returned: "engine:<name>" names a
/// live engine render target, anything else is an image file path; empty clears the background.
void ApplyThumbnailImage(UIElement& element, const std::string& image);

} // namespace GameEngine::Editor
