#pragma once

#include "UI/SmartFolder/SmartFolder.h"
#include "UndoRedo/IEditorCommand.h"
#include <functional>

namespace GameEngine {

class SmartFolderManager;

namespace Editor {

// Command for creating a smart folder
class CreateSmartFolderCommand : public IEditorCommand {
public:
    CreateSmartFolderCommand(SmartFolderManager* manager, const SmartFolder& folder,
                             std::function<void()> onChanged);
    
    const char* GetName() const override { return "Create Smart Folder"; }
    void Do() override;
    void Undo() override;
    
private:
    SmartFolderManager* m_Manager;
    SmartFolder m_Folder;
    std::function<void()> m_OnChanged;
};

// Command for deleting a smart folder
class DeleteSmartFolderCommand : public IEditorCommand {
public:
    DeleteSmartFolderCommand(SmartFolderManager* manager, const std::string& folderId,
                             std::function<void()> onChanged);
    
    const char* GetName() const override { return "Delete Smart Folder"; }
    void Do() override;
    void Undo() override;
    
private:
    SmartFolderManager* m_Manager;
    std::string m_FolderId;
    SmartFolder m_DeletedFolder; // Stored for undo
    std::function<void()> m_OnChanged;
};

// Command for updating a smart folder
class UpdateSmartFolderCommand : public IEditorCommand {
public:
    UpdateSmartFolderCommand(SmartFolderManager* manager, const SmartFolder& oldFolder,
                             const SmartFolder& newFolder, std::function<void()> onChanged);
    
    const char* GetName() const override { return "Update Smart Folder"; }
    void Do() override;
    void Undo() override;
    
private:
    SmartFolderManager* m_Manager;
    SmartFolder m_OldFolder;
    SmartFolder m_NewFolder;
    std::function<void()> m_OnChanged;
};

} // namespace Editor
} // namespace GameEngine
