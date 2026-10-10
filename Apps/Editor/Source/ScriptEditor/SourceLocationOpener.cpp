#include "ScriptEditor/SourceLocationOpener.h"

#include "Editor/Settings/ScriptEditorSettings.h"
#include "EditorPanelManager.h"
#include "ExternalScriptEditorLauncher.h"
#include "Panels/ScriptEditorPanel.h"
#include "ScriptEditor/SourceLocation.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace GameEngine::Editor
{
namespace
{

bool HasCSharpExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".cs";
}

} // namespace

SourceLocationOpener::SourceLocationOpener(EditorPanelManager& panelManager, EditorWindowContext* window,
                                           ScriptEditorPanel* scriptEditor)
    : m_PanelManager(&panelManager)
    , m_Window(window)
    , m_ScriptEditor(scriptEditor)
{
}

void SourceLocationOpener::Open(const SourceLocation& location) const
{
    if (ScriptEditorSettings::Get().GetOpenInScriptInspector() && OpenInScriptEditor(location))
        return;
    OpenExternally(location.Path);
}

bool SourceLocationOpener::OpenInScriptEditor(const SourceLocation& location) const
{
    if (!m_ScriptEditor)
        return false;

    m_PanelManager->ShowOrActivateScriptEditorPanel(m_Window);
    m_ScriptEditor->OpenScript(location.Path);
    if (location.Line > 0)
        m_ScriptEditor->ScrollToLine(location.Line, {}, location.Column);
    return true;
}

void SourceLocationOpener::OpenLogLine(std::string_view lineText) const
{
    if (const std::optional<SourceLocation> location = FindSourceLocationInLogLine(lineText))
        Open(*location);
}

void SourceLocationOpener::OpenLogSourceFile(const std::string& file, int line) const
{
    if (file.empty())
        return;

    SourceLocation location;
    location.Path = std::filesystem::path(file);
    location.Line = static_cast<size_t>(std::max(line, 0));

    if (HasCSharpExtension(location.Path))
        Open(location);
    else
        OpenExternally(location.Path);
}

void SourceLocationOpener::OpenExternally(const std::filesystem::path& path) const
{
    // The launcher falls back to the OS handler itself; a failed launch has no
    // better destination here.
    (void)ExternalScriptEditorLauncher::OpenScript(path);
}

} // namespace GameEngine::Editor
