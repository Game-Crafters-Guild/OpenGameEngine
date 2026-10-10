#pragma once

#include "Events/Event.h"

#include <filesystem>

namespace GameEngine::UI
{
struct UIScaleSettings;
}

namespace GameEngine::Editor
{

// Persists the project's game UI scale policy into
// <workspaceRoot>/.Editor/ProjectSettings.json.
//
// Engine owns the JSON shape (UIScaleProjectSettings::WriteTo) and the read
// side; this is only the editor-side transport, because atomic writes and the
// schemaVersion convention are SettingsStore's. Returns false when there is no
// writable project.
bool SaveUIScaleProjectSettings(const std::filesystem::path& workspaceRoot,
                                const UI::UIScaleSettings& settings);

// Raised by SaveUIScaleProjectSettings after the file is written, so a game UI
// host previewing the policy re-reads it. Main thread only.
Event<>& UIScaleProjectSettingsSaved();

} // namespace GameEngine::Editor
