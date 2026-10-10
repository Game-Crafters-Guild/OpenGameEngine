#include "UI/SmartFolder/SmartFolderCommands.h"
#include "UI/SmartFolder/SmartFolderManager.h"

namespace GameEngine::Editor {

// CreateSmartFolderCommand

CreateSmartFolderCommand::CreateSmartFolderCommand(SmartFolderManager* manager, 
                                                   const SmartFolder& folder,
                                                   std::function<void()> onChanged)
    : m_Manager(manager)
    , m_Folder(folder)
    , m_OnChanged(std::move(onChanged))
{
}

void CreateSmartFolderCommand::Do()
{
    if (m_Manager) {
        m_Manager->AddFolder(m_Folder);
        if (m_OnChanged) m_OnChanged();
    }
}

void CreateSmartFolderCommand::Undo()
{
    if (m_Manager) {
        m_Manager->RemoveFolder(m_Folder.Id);
        if (m_OnChanged) m_OnChanged();
    }
}

// DeleteSmartFolderCommand

DeleteSmartFolderCommand::DeleteSmartFolderCommand(SmartFolderManager* manager,
                                                   const std::string& folderId,
                                                   std::function<void()> onChanged)
    : m_Manager(manager)
    , m_FolderId(folderId)
    , m_OnChanged(std::move(onChanged))
{
    // Store the folder for undo
    if (m_Manager) {
        if (const SmartFolder* folder = m_Manager->GetById(folderId)) {
            m_DeletedFolder = *folder;
        }
    }
}

void DeleteSmartFolderCommand::Do()
{
    if (m_Manager) {
        m_Manager->RemoveFolder(m_FolderId);
        if (m_OnChanged) m_OnChanged();
    }
}

void DeleteSmartFolderCommand::Undo()
{
    if (m_Manager) {
        m_Manager->AddFolder(m_DeletedFolder);
        if (m_OnChanged) m_OnChanged();
    }
}

// UpdateSmartFolderCommand

UpdateSmartFolderCommand::UpdateSmartFolderCommand(SmartFolderManager* manager,
                                                   const SmartFolder& oldFolder,
                                                   const SmartFolder& newFolder,
                                                   std::function<void()> onChanged)
    : m_Manager(manager)
    , m_OldFolder(oldFolder)
    , m_NewFolder(newFolder)
    , m_OnChanged(std::move(onChanged))
{
}

void UpdateSmartFolderCommand::Do()
{
    if (m_Manager) {
        m_Manager->UpdateFolder(m_NewFolder);
        if (m_OnChanged) m_OnChanged();
    }
}

void UpdateSmartFolderCommand::Undo()
{
    if (m_Manager) {
        m_Manager->UpdateFolder(m_OldFolder);
        if (m_OnChanged) m_OnChanged();
    }
}

} // namespace GameEngine::Editor
