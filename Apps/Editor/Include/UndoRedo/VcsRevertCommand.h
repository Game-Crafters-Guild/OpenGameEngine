#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "Logger/Logger.h"
#include "VCSIntegration/IVCSIntegration.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace GameEngine::Editor
{
// Undoable VCS revert: reverts a file to its committed version through the
// active provider's integration.
// - Do/Redo: revert to the committed version
// - Undo: restore the previously modified content from an in-memory backup
class VcsRevertCommand final : public IEditorCommand
{
  public:
    VcsRevertCommand(std::filesystem::path path)
        : m_Path(std::move(path))
    {
        m_Name = "Revert " + m_Path.filename().string();

        // Save the current (modified) content before reverting
        SaveCurrentContent();
    }

    const char* GetName() const override { return m_Name.c_str(); }

    void Do() override { RevertToCommitted(); }
    void Undo() override { RestoreModifiedContent(); }
    void Redo() override { RevertToCommitted(); }

  private:
    void SaveCurrentContent()
    {
        if (m_Path.empty())
            return;

        std::ifstream in(m_Path, std::ios::binary);
        if (!in.is_open())
        {
            Logger::Log::Warning("VcsRevertCommand: failed to read '{}' for backup", m_Path.string());
            return;
        }

        std::ostringstream ss;
        ss << in.rdbuf();
        m_SavedContent = ss.str();
        in.close();

        m_ContentSaved = true;
    }

    void RevertToCommitted()
    {
        if (m_Path.empty())
            return;

        auto* vcs = EditorVcsProviderRegistry::Get().ActiveIntegration();
        if (!vcs || !vcs->IsRepository())
        {
            Logger::Log::Warning("VcsRevertCommand: no active VCS repository");
            return;
        }

        if (vcs->Revert(m_Path))
        {
            Logger::Log::Info("VcsRevertCommand: reverted '{}'", m_Path.filename().string());
        }
        else
        {
            Logger::Log::Warning("VcsRevertCommand: failed to revert '{}'", m_Path.filename().string());
        }
    }

    void RestoreModifiedContent()
    {
        if (m_Path.empty() || !m_ContentSaved)
            return;

        std::ofstream out(m_Path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            Logger::Log::Warning("VcsRevertCommand: failed to restore '{}' - cannot open for write",
                                 m_Path.string());
            return;
        }

        out << m_SavedContent;
        out.close();

        Logger::Log::Info("VcsRevertCommand: restored modified content of '{}'",
                          m_Path.filename().string());

        if (auto* vcs = EditorVcsProviderRegistry::Get().ActiveIntegration())
            vcs->RefreshStatus(m_Path);
    }

    std::string m_Name;
    std::filesystem::path m_Path;
    std::string m_SavedContent;
    bool m_ContentSaved = false;
};

} // namespace GameEngine::Editor
