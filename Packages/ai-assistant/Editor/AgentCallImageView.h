#pragma once

#include "AgentCallMedia.h"

#include "UI/UIElement.h"

#include <memory>
#include <optional>
#include <string>

namespace GameEngine
{
class ContextMenuManipulator;

/// The image a call row shows inline (AgentCallRowModel::Image): the PNG at its own aspect,
/// scaled down to the row's width and the height cap of `.agent-call-image` (max-height in
/// PromptPanel.css), never up. The picture is the thumbnail service's downscaled copy of the
/// PNG (EditorAssetActions::ShowThumbnail), asked for while the image is in the panel and
/// released (the UI's texture evicted) when it leaves, its row's removal included. A click, Enter or Space opens
/// it in the Asset View (EditorAssetActions::Preview), at the panel's size; a right-click offers
/// the file's menu (AgentCallFileMenu). The tooltip names the file.
class AgentCallImageView final : public UIElement
{
public:
    /// `idSuffix` makes the element's id unique ("<message>:<call>").
    explicit AgentCallImageView(const std::string& idSuffix);

    void Show(const AgentCallImage& image);
    /// Sizes the image, its hairline included, to fit within `maxWidth` x `maxHeight` (the
    /// row's width and the CSS height cap). Returns true when its size moved by more than half a pixel, so the panel
    /// can wait for its rows to settle.
    bool Fit(float maxWidth, float maxHeight);

private:
    /// Asks the editor for the picture, unless the image already shows one.
    void RequestPicture();
    /// Drops the picture and evicts the UI's texture for it.
    void ReleasePicture();
    void OpenInAssetView();
    void OnMouseUp(UIEvent& event);
    void OnKeyDown(UIEvent& event);

    std::optional<AgentCallImage> m_Image;
    /// The file's right-click menu, replaced with the image.
    std::shared_ptr<ContextMenuManipulator> m_Menu;
};
} // namespace GameEngine
