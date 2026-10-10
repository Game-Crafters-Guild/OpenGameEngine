// Leaf-dep stub for PanelDeferredBindLatchTests.
//
// WebPanel reads the editor's cross-panel asset-drag state to accept a file dropped onto
// the embedded web view. The global lives in BookmarksPanel.cpp, which carries the whole
// bookmarks UI; the latch tests never dispatch a drop, so a definition is all the link
// needs. The struct is restated here because that is how the production TUs share it —
// WebPanel.cpp and BookmarksPanel.cpp each declare their own copy.

#include <filesystem>
#include <string>

namespace GameEngine
{

struct DragState
{
    bool active = false;
    std::filesystem::path assetPath;
    std::string scenePath;
    std::string entityId;
};

DragState g_DragState;

} // namespace GameEngine
