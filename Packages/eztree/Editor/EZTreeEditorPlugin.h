#pragma once

namespace GameEngine::EZTreeEditor
{

// Registers the Tree Generator editor plugin, its component traits (display
// name, icons, pick-root/pivot behavior, enabled toggle) and its scene-view
// pick provider into the EditorSDK registries. Idempotent per module load;
// a hot-reloaded module re-registers and replaces the previous instance.
void RegisterEZTreeEditorPlugin();

} // namespace GameEngine::EZTreeEditor
