#pragma once

#include "EditorApplication.h" // EditorApplication::EditorWindowContext

#include <filesystem>
#include <string>
#include <string_view>

namespace GameEngine
{
class EditorPanelManager;
class ScriptEditorPanel;

namespace Editor
{
struct SourceLocation;

/// Opens a source file, at a line and column when known, where the user reads
/// code: the Script Editor panel, or the external editor ExternalScriptEditorLauncher
/// picks (which opens the file without a line). The Log, Script Errors, Shader
/// Errors and inspector entry points all open files through it, so they honour
/// the Script Editor's "open in Script Inspector" preference the same way.
///
/// A small value: copy it into the callbacks that use it. The panel manager,
/// window and panel it names must outlive those callbacks.
class SourceLocationOpener
{
  public:
    using EditorWindowContext = EditorApplication::EditorWindowContext;

    /// `scriptEditor` may be null: the internal editor is then never used.
    SourceLocationOpener(EditorPanelManager& panelManager, EditorWindowContext* window,
                         ScriptEditorPanel* scriptEditor);

    /// The Script Editor panel when the preference asks for it, else the external editor.
    void Open(const SourceLocation& location) const;

    /// The Script Editor panel, whatever the preference. False when there is no panel.
    bool OpenInScriptEditor(const SourceLocation& location) const;

    /// The C# location a log line names (FindSourceLocationInLogLine); a line
    /// that names none opens nothing.
    void OpenLogLine(std::string_view lineText) const;

    /// A log entry's source file at a 1-based line (0 = unknown). Only C# files
    /// follow the preference; native sources always open externally.
    void OpenLogSourceFile(const std::string& file, int line) const;

  private:
    void OpenExternally(const std::filesystem::path& path) const;

    EditorPanelManager* m_PanelManager;
    EditorWindowContext* m_Window;
    ScriptEditorPanel* m_ScriptEditor;
};

} // namespace Editor
} // namespace GameEngine
