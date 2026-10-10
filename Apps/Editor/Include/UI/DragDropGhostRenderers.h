#pragma once

namespace GameEngine::Editor
{
// Registers Editor-specific drag ghost renderers (payload-type -> UI customization).
// Safe to call multiple times (subsequent calls overwrite the same entries).
void RegisterEditorDragDropGhostRenderers();
} // namespace GameEngine::Editor

