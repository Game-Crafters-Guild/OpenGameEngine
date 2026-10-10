#pragma once

#include "Editor/Settings/EditorSettingsRegistry.h"

#include <functional>

namespace GameEngine::Editor
{

// Describes the Directional Shadows section hosted by the built-in Rendering
// page. The descriptor keeps field presentation and SettingsStore persistence
// beside the render-project setting instead of embedding either in
// SettingsPanel.
SettingsCategoryDescriptor CreateDirectionalShadowSettingsSection(
    std::function<void()> onProjectRenderSettingsChanged = {});

} // namespace GameEngine::Editor
