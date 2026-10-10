#pragma once

namespace GameEngine::Editor
{

// Registers the "Node Graph" page under UI. Main-thread only, once at editor
// startup; the SettingsPanel builds the rows from the descriptor. Preference
// keys stay nodeGraph.* (frozen).
void RegisterGraphSettingsCategory();

} // namespace GameEngine::Editor
