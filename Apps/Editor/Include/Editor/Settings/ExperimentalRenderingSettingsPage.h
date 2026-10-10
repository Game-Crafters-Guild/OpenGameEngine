#pragma once

namespace GameEngine::Editor
{

// Registers the "Experimental" user-settings page with
// Editor::EditorSettingsRegistry and applies its stored values to the renderer.
// Main-thread only, once at editor startup; the SettingsPanel builds the rows
// from the descriptor.
//
// The page holds renderer switches that exist to be A/B'd by eye and are not
// yet a shipping decision. Every row defaults OFF, so an editor that has never
// opened the page behaves exactly as it did before the row existed.
void RegisterExperimentalRenderingSettingsCategory();

} // namespace GameEngine::Editor
