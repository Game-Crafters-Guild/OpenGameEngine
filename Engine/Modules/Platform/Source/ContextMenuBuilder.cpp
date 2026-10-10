#include "Platform/ContextMenu.h"

#include <algorithm>
#include <unordered_map>

namespace GameEngine {

namespace {

struct PathParts {
    std::vector<std::string> segments;
};

static PathParts SplitPath(const std::string& path) {
    PathParts result;
    std::string current;
    for (char c : path) {
        if (c == '/') {
            if (!current.empty()) {
                result.segments.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        result.segments.push_back(current);
    }
    return result;
}

} // namespace

void BuildContextMenuFromPaths(
    INativeContextMenu* menu,
    const std::vector<ContextMenuItemDesc>& items,
    bool SortByPriorityThenPath)
{
    if (!menu) return;

    std::vector<ContextMenuItemDesc> sorted = items;
    if (SortByPriorityThenPath) {
        std::sort(sorted.begin(), sorted.end(), [](const ContextMenuItemDesc& a, const ContextMenuItemDesc& b) {
            if (a.Priority != b.Priority) return a.Priority < b.Priority;
            return a.Path < b.Path;
        });
    }

    std::unordered_map<std::string, uint32_t> prefixToMenuId;

    for (const ContextMenuItemDesc& desc : sorted) {
        if (desc.Path.empty()) {
            continue;
        }

        PathParts parts = SplitPath(desc.Path);
        if (parts.segments.empty()) continue;

        std::string prefix;
        uint32_t parentId = 0;

        for (size_t i = 0; i < parts.segments.size(); ++i) {
            const std::string& segment = parts.segments[i];
            if (segment.empty()) continue;

            if (!prefix.empty()) prefix += '/';
            prefix += segment;
            const bool isLeaf = (i + 1 == parts.segments.size());

            auto it = prefixToMenuId.find(prefix);
            if (!isLeaf) {
                uint32_t menuId = 0;
                if (it == prefixToMenuId.end()) {
                    menuId = menu->AddSubMenu(parentId, segment);
                    prefixToMenuId.emplace(prefix, menuId);
                } else {
                    menuId = it->second;
                }
                parentId = menuId;
            } else {
                // Special leaf: separators can be injected by using a segment of "-" or "---*".
                // This keeps path-based menus simple while allowing callers to add dividers
                // within a submenu.
                if (segment == "-" || segment.rfind("---", 0) == 0)
                {
                    menu->AddSeparator(parentId);
                    continue;
                }
                // Leaf: either a command item or an explicit non-clickable node.
                if (desc.CommandId != 0) {
                    menu->AddItem(parentId, segment, desc.CommandId, desc.Flags);
                    if (!desc.IconPath.empty()) {
                        menu->SetItemIcon(desc.CommandId, desc.IconPath);
                    }
                } else {
                    // Ensure the submenu exists, ignore flags for now.
                    uint32_t menuId = 0;
                    if (it == prefixToMenuId.end()) {
                        menuId = menu->AddSubMenu(parentId, segment);
                        prefixToMenuId.emplace(prefix, menuId);
                    }
                    else {
                        menuId = it->second;
                    }
                    if (!desc.IconPath.empty()) {
                        menu->SetSubMenuIcon(menuId, desc.IconPath);
                    }
                }
            }
        }
    }
}

} // namespace GameEngine
