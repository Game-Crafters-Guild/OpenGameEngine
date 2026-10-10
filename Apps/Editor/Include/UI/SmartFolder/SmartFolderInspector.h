#pragma once

#include "UI/SmartFolder/SmartFolder.h"
#include "UI/UIElement.h"
#include <string>
#include <functional>
#include <filesystem>
#include <vector>

namespace GameEngine {

class SmartFolderManager;
class TextField;

namespace Editor {
class UndoRedoService;
}

/// Custom inspector widget for editing SmartFolder settings.
/// Encapsulates all the UI building logic for smart folder configuration.
class SmartFolderInspector : public UIElement
{
public:
    SmartFolderInspector();
    ~SmartFolderInspector() override = default;

    /// Set the smart folder to display/edit
    void SetSmartFolder(const std::string& smartFolderId, SmartFolderManager* manager);
    
    /// Set the undo/redo service for delete operations
    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_Undo = undo; }
    
    /// Callback when the smart folder is deleted
    void SetOnDeleted(std::function<void()> cb) { m_OnDeleted = std::move(cb); }
    void SetOnNameChanging(std::function<void(const std::string&)> cb) { m_OnNameChanging = std::move(cb); }
    void SetOnNameChanged(std::function<void(const std::string&)> cb) { m_OnNameChanged = std::move(cb); }
    void SetNameValueFromExternal(const std::string& name);

private:
    void RebuildUI();
    void RequestRebuildUI();
    void EnsureLocationOptionsCached();
    void BuildNameField(SmartFolder* folder);
    void BuildLocationDropdown(SmartFolder* folder);
    void BuildMatchDropdown(SmartFolder* folder);
    void BuildGlobalToggle(SmartFolder* folder);
    void BuildFiltersSection(SmartFolder* folder);
    void BuildFilterRow(SmartFolder* folder, size_t filterIndex);
    void BuildDeleteButton(SmartFolder* folder);

    std::string m_SmartFolderId;
    SmartFolderManager* m_Manager = nullptr;
    Editor::UndoRedoService* m_Undo = nullptr;
    std::function<void()> m_OnDeleted;
    std::function<void(const std::string&)> m_OnNameChanging;
    std::function<void(const std::string&)> m_OnNameChanged;
    TextField* m_NameField = nullptr;

    bool m_RebuildPosted = false;

    // Cached location dropdown options. Building these can be expensive for large projects,
    // so avoid rescanning the filesystem on every small inspector change.
    std::filesystem::path m_CachedWorkspaceRoot;
    std::vector<std::string> m_CachedLocationOptions;
    bool m_CachedLocationOptionsValid = false;
};

} // namespace GameEngine
