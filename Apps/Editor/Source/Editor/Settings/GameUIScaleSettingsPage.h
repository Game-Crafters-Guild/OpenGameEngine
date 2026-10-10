#pragma once

namespace GameEngine::Editor
{

// Registers the "Game UI Scaling" project-settings page with
// Editor::EditorSettingsRegistry. Main-thread only, once at editor startup;
// the SettingsPanel builds the rows from the descriptor.
void RegisterGameUIScaleSettingsCategory();

} // namespace GameEngine::Editor
