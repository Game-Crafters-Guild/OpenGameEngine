#pragma once

#include <span>

namespace GameEngine::Editor
{

// Registers every editor panel type with EditorPanelRegistry, so the dock
// config (layout.uxml <DockablePanel type="...">) and the fallback docking both
// build panels through EditorPanelRegistry::CreatePanelOfType. Call once at
// startup, before the docking model is built.
void RegisterBuiltInPanelTypes();

// One panel of the fallback dock inventory: the fixed layout used when
// layout.uxml has no valid dock config, and always under UI replay so scripted
// runs target the same panel ids on every machine.
struct FallbackDockPanel
{
    const char* PanelId;
    const char* TypeKey;
};

// The fallback inventory, in the order the panels are created.
std::span<const FallbackDockPanel> FallbackDockPanels();

} // namespace GameEngine::Editor
