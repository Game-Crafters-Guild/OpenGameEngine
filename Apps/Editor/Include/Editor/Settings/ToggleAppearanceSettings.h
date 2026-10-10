#pragma once

#include <vector>

#include "UI/UIManagerRef.h"

namespace GameEngine
{
class UIManager;

namespace Editor
{

// User preference for switch-track inset shadow. Persisted in editor
// preferences. Get() registers the Settings descriptor; ApplyTo stamps
// the root class on a UIManager and tracks it for live Set.
class ToggleAppearanceSettings
{
public:
    static ToggleAppearanceSettings& Get();

    bool GetInnerShadowEnabled() const { return m_InnerShadowEnabled; }

    // Live apply only. Persistence is the Settings row's PrefKey — this
    // method must not write the file, because the row also fires Set with
    // Get()'s own value when the page is built.
    void SetInnerShadowEnabled(bool enabled);

    // Stamp the root class on `ui` and remember it so Set can update
    // every window that has already been applied.
    void ApplyTo(UIManager* ui);

private:
    ToggleAppearanceSettings();
    void ApplyToTrackedManagers();

    bool m_InnerShadowEnabled = false;
    std::vector<UIManagerRef> m_TrackedManagers;
};

void RegisterToggleAppearanceSettings();

} // namespace Editor
} // namespace GameEngine
