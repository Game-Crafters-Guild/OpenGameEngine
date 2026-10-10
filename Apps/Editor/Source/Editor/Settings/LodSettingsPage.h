#pragma once

namespace GameEngine::Editor
{

// Registers the "Level of Detail" project-settings page with
// Editor::EditorSettingsRegistry. Main-thread only, once at editor startup;
// the SettingsPanel builds the rows from the descriptor.
void RegisterLodSettingsCategory();

} // namespace GameEngine::Editor
