#pragma once

#include "UI/SmartFolder/SmartFolderManager.h"

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{

class AssetRegistry;

/// Editor-session smart-folder state, independent of the Assets browser UI.
/// Owns the SmartFolderManager (the persistence/CRUD model, shared with the
/// inspector and undo commands) and layers the session-only concerns on top:
/// query evaluation with result caching, and the current selection.
class SmartFolderController
{
public:
    /// Notified with (smartFolderId, manager) on selection, ("", nullptr) on clear.
    using SelectionCallback = std::function<void(const std::string&, SmartFolderManager*)>;

    SmartFolderManager& GetManager() { return m_Manager; }
    const SmartFolderManager& GetManager() const { return m_Manager; }

    void SetAssetsRoot(std::filesystem::path assetsRoot);
    void InvalidateResults();

    /// Returns null when the folder does not exist. The returned vector remains
    /// valid until the next Evaluate or InvalidateResults call.
    const std::vector<std::filesystem::path>* Evaluate(
        const std::string& smartFolderId,
        AssetRegistry* registry);

    // Selection ----------------------------------------------------------

    const std::string& GetSelectedId() const { return m_SelectedId; }
    bool HasSelection() const { return !m_SelectedId.empty(); }

    /// Select a smart folder and notify the selection callback. An empty id
    /// only stores the cleared state; use ClearSelection to also notify.
    void SetSelected(const std::string& smartFolderId);

    /// Clear the selection and notify the callback with ("", nullptr).
    void ClearSelection();

    void SetOnSelectionChanged(SelectionCallback cb) { m_OnSelectionChanged = std::move(cb); }

private:
    static bool CanCacheResults(const SmartFolder& folder);

    SmartFolderManager m_Manager;
    std::filesystem::path m_AssetsRoot;
    std::unordered_map<std::string, std::vector<std::filesystem::path>> m_ResultCache;
    std::vector<std::filesystem::path> m_TransientResults;
    std::string m_SelectedId;
    SelectionCallback m_OnSelectionChanged;
};

} // namespace GameEngine
