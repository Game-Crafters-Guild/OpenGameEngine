#pragma once

#include "Panels/BookmarksPanel.h"
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine::Editor
{
// Common editor drag payloads shared across panels/windows.
//
// IMPORTANT: These types must be shared definitions (not duplicated in .cpp files) so that
// UI::Interaction::GetPayloadTypeId<T>() remains consistent across the program.

struct AssetPathsDragPayload
{
    std::vector<std::filesystem::path> paths; // absolute paths for now
    std::string displayLabel;                // for ghost UI (“Foo + 3”)
    bool copyOnly = false;                   // force copy semantics (used by native OS file drops)
};

// Hierarchy entity drag payload: TreeIds are provider-defined stable ids (Hierarchy uses packed EntityHandle).
struct HierarchyEntityDragPayload
{
    std::vector<std::uint64_t> treeIds;
    std::string displayLabel;
};

// Online asset drag payload: Polyhaven or other online sources.
// Used for mixed selections where some items are not yet downloaded.
struct OnlineAssetEntry
{
    std::string slug;        // e.g. "rock_wall_04"
    std::string name;        // human-readable display name
};

struct OnlineAssetDragPayload
{
    std::string slug;        // primary slug (back-compat for single-item)
    std::string name;        // primary display name
    std::string type;        // "hdris" | "textures" | "models"
    std::string displayLabel;
    std::vector<OnlineAssetEntry> entries; // all items (may include downloaded + not-downloaded)
};

// Bookmark list reorder: drag a bookmark row to a new index.
struct BookmarkReorderPayload
{
    size_t bookmarkIndex = 0;
    std::string displayLabel;
};

// Bookmark drag: full bookmark data for reorder (on BookmarksPanel) or navigate (e.g. drop on Hierarchy).
struct BookmarkDragPayload
{
    GameEngine::Bookmark bookmark;
    size_t bookmarkIndex = 0;
    std::string displayLabel;
};
// Render pipeline pass reorder: drag a pass row to a new position.
struct RenderPassReorderPayload
{
    size_t passIndex = 0;
    std::string displayLabel;
};

} // namespace GameEngine::Editor
