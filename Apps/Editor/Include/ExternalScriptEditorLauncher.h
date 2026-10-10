#pragma once

#include <filesystem>

namespace GameEngine
{

class ScriptManager;

// Helper for opening C# scripts from the Editor in an external code editor
// with the appropriate C# project context when available.
//
// This class is intentionally stateless; all logic is contained in the static
// OpenScript function so callers do not need to manage instances.
class ExternalScriptEditorLauncher
{
public:
    // Open a C# script in the user's preferred external editor. When scripting
    // is enabled, this opens it with the project ScriptProjectPath names and
    // delegates to Platform::OpenScriptWithProject(script, project).
    //
    // When scripting is disabled, ScriptManager is not initialized, or the
    // project cannot be resolved, this gracefully falls back to
    // Platform::OpenPath(scriptPath).
    //
    // Returns true if an editor (or the OS handler) was launched successfully.
    static bool OpenScript(const std::filesystem::path& scriptPath);

    // The C# project a script opens with: the auto-generated scripts project
    // where `scripts` writes it. Empty when there is none to name.
    static std::filesystem::path ScriptProjectPath(const ScriptManager& scripts);
};

} // namespace GameEngine

