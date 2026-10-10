#pragma once

#include "AgentCallResourceResolver.h"

#include "UI/UIElement.h"

#include <string>

namespace GameEngine
{
/// An asset a call names, under its row: the asset's thumbnail (the editor's thumbnail
/// service, EditorAssetActions::ShowThumbnail), its file name, its type and Open
/// (EditorAssetActions::Open, the editor's open-asset policy). An asset that is not there is
/// its name and "not found", with no thumbnail and no Open, as of when its row was built; its
/// tooltip says so. A found asset's tile offers the file's right-click menu (AgentCallFileMenu).
class AgentCallResourceTile final : public UIElement
{
public:
    /// The thumbnail's request size in pixels: the tile's 32 px box at twice the density.
    static constexpr int kThumbnailRequestPx = 64;

    /// `idSuffix` makes the tile's element ids unique ("<message>:<call>:<asset>").
    AgentCallResourceTile(const AgentCallResource& resource, const std::string& idSuffix);

    /// Asks the editor for the asset's thumbnail. Call it once the tile is in the panel: an
    /// answer that comes later (a thumbnail still rendering) returns through the tile's UI
    /// thread, which a tile not yet in a panel does not have.
    void RequestThumbnail();

private:
    void Open();
    void OnOpenMouseUp(UIEvent& event);
    void OnOpenKeyDown(UIEvent& event);

    AgentCallResource m_Resource;
    /// The thumbnail's well; null for an asset that is not there.
    UIElement* m_Thumbnail = nullptr;
};
} // namespace GameEngine
