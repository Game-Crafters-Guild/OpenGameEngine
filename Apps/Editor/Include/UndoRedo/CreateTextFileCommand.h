#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "Logger/Logger.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace GameEngine::Editor
{
// Simple undoable filesystem operation: create (write) a text file.
// - Do/Redo: writes contents to path (creates directories as needed)
// - Undo: deletes the file (best-effort)
class CreateTextFileCommand final : public IEditorCommand
{
  public:
    CreateTextFileCommand(std::string displayName, std::filesystem::path path, std::string contents)
        : m_Name(std::move(displayName)), m_Path(std::move(path)), m_Contents(std::move(contents))
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }

    void Do() override { WriteFile(); }
    void Undo() override { RemoveFile(); }
    void Redo() override { WriteFile(); }

  private:
    void WriteFile()
    {
        if (m_Path.empty())
            return;

        std::error_code ec;
        std::filesystem::create_directories(m_Path.parent_path(), ec);
        ec.clear();

        std::ofstream out(m_Path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            Logger::Log::Warning("CreateTextFileCommand: failed to open '{}' for write", m_Path.string());
            return;
        }
        out << m_Contents;
        out.close();
    }

    void RemoveFile()
    {
        if (m_Path.empty())
            return;
        std::error_code ec;
        (void)std::filesystem::remove(m_Path, ec);
    }

    std::string m_Name;
    std::filesystem::path m_Path;
    std::string m_Contents;
};

} // namespace GameEngine::Editor

