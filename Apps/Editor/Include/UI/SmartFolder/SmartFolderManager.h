#pragma once

#include "UI/SmartFolder/SmartFolder.h"
#include <functional>
#include <memory>

namespace GameEngine {

/// Manages smart folders - CRUD operations and persistence
class SmartFolderManager {
public:
    using ChangeCallback = std::function<void()>;
    
    SmartFolderManager();
    ~SmartFolderManager();
    
    /// Set the project root directory (for per-project smart folders)
    void SetProjectRoot(const std::filesystem::path& projectRoot);
    
    /// Load smart folders from both global and project storage
    void Load();
    
    /// Save all smart folders to their respective storage locations
    void Save();
    
    /// Get all smart folders (both global and project)
    const std::vector<SmartFolder>& GetAll() const { return m_SmartFolders; }
    
    /// Get a smart folder by ID (returns nullptr if not found)
    SmartFolder* GetById(const std::string& id);
    const SmartFolder* GetById(const std::string& id) const;
    
    /// Create a new smart folder and return its ID
    std::string Create(const std::string& name, bool isGlobal = false);
    
    /// Update an existing smart folder
    void Update(const SmartFolder& folder);
    
    /// Delete a smart folder by ID
    void Delete(const std::string& id);
    
    /// Register a callback to be notified when smart folders change
    void SetOnChanged(ChangeCallback callback) { m_OnChanged = std::move(callback); }
    
    // Low-level methods for undo/redo commands (don't trigger OnChanged)
    void AddFolder(const SmartFolder& folder);
    void RemoveFolder(const std::string& id);
    void UpdateFolder(const SmartFolder& folder);
    
    /// Notify listeners that smart folders have changed (called by undo/redo commands)
    void NotifyChanged();
    
private:
    void LoadFromFile(const std::filesystem::path& path, bool isGlobal);
    void SaveToFile(const std::filesystem::path& path, bool isGlobal) const;
    
    std::filesystem::path GetGlobalStoragePath() const;
    std::filesystem::path GetProjectStoragePath() const;
    
    std::vector<SmartFolder> m_SmartFolders;
    std::filesystem::path m_ProjectRoot;
    ChangeCallback m_OnChanged;
};

} // namespace GameEngine
