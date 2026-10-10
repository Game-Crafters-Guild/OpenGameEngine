#pragma once

#include <filesystem>

namespace GameEngine::Rendering
{
struct LodProjectSettings;
}

namespace GameEngine::Editor
{

// Persists the project's mesh-LOD settings into
// <workspaceRoot>/.Editor/ProjectSettings.json.
//
// Engine owns the JSON shape (Rendering::LodProjectSettings::WriteTo) and the
// read side; this is only the editor-side transport, because atomic writes and
// the schemaVersion convention are SettingsStore's and the settings page is the
// only writer that exists. It names no keys, so the two cannot drift.
//
// Read-modify-write of just that struct's keys, so a hand-edited sibling under
// "rendering" survives. Returns false when there is no writable project.
bool SaveLodProjectSettings(const std::filesystem::path& workspaceRoot,
                            const Rendering::LodProjectSettings& settings);

} // namespace GameEngine::Editor
