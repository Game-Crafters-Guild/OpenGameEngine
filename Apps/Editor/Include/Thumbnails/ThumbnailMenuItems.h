#pragma once

namespace GameEngine
{
class IThumbnailProvider;

namespace Editor
{

// Registers the thumbnails' items in the editor menus: "Generate Thumbnails" in
// the Assets panel's folder context menu. `provider` must outlive the editor's
// menus (the editor owns both for its whole run).
void RegisterThumbnailMenuItems(IThumbnailProvider& provider);

} // namespace Editor
} // namespace GameEngine
