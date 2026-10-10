#pragma once

#include <functional>

namespace GameEngine::Editor
{

/// What the Build settings pages need from the application shell. The pages are
/// rebuilt from their descriptors every time one is opened, so these outlive
/// any single SettingsPanel instance and must stay valid for the session.
struct BuildSettingsPageHooks
{
    /// Bring the Build panel to the front.
    std::function<void()> OpenBuildPanel;
    /// Whether the Build panel is already the active tab; the "Open Build
    /// Window" row is left out of the page when it is.
    std::function<bool()> IsBuildPanelActive;
    /// Run an action outside the current UI event dispatch. The version field
    /// needs it: notifying the Build panel rebuilds its rows, which must not
    /// happen while a text field is still dispatching.
    std::function<void(std::function<void()>)> Defer;
};

/// Registers the Build page and one sub-page per build target with
/// EditorSettingsRegistry. Main thread, editor startup, once.
void RegisterBuildSettingsCategories(BuildSettingsPageHooks hooks);

} // namespace GameEngine::Editor
