#include "Editor/Settings/ToggleAppearanceSettings.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kInnerShadowPrefKey = "ui.toggleInnerShadow";
constexpr const char* kInnerShadowRootClass = "toggle-inner-shadow";

void ApplyClass(UIManager* ui, bool enabled)
{
    if (!ui)
        return;
    UIElement* root = ui->GetRootElement();
    if (!root)
        return;
    if (enabled)
        root->AddClass(kInnerShadowRootClass);
    else
        root->RemoveClass(kInnerShadowRootClass);
    ui->MarkStyleDirtyAll();
}

} // namespace

ToggleAppearanceSettings& ToggleAppearanceSettings::Get()
{
    static ToggleAppearanceSettings instance;
    // After the singleton exists: RegisterCategory may replay into a
    // SettingsPanel observer, and those closures call Get().
    static const bool registered = []
    {
        RegisterToggleAppearanceSettings();
        return true;
    }();
    (void)registered;
    return instance;
}

ToggleAppearanceSettings::ToggleAppearanceSettings()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    prefs.TryGetBool(kInnerShadowPrefKey, m_InnerShadowEnabled);
}

void ToggleAppearanceSettings::SetInnerShadowEnabled(bool enabled)
{
    m_InnerShadowEnabled = enabled;
    ApplyToTrackedManagers();
}

void ToggleAppearanceSettings::ApplyTo(UIManager* ui)
{
    if (!ui)
        return;

    bool alreadyTracked = false;
    for (const UIManagerRef& tracked : m_TrackedManagers)
    {
        if (tracked.Get() == ui)
        {
            alreadyTracked = true;
            break;
        }
    }
    if (!alreadyTracked)
        m_TrackedManagers.push_back(UIManagerRef(ui));

    ApplyClass(ui, m_InnerShadowEnabled);
}

void ToggleAppearanceSettings::ApplyToTrackedManagers()
{
    size_t write = 0;
    for (const UIManagerRef& tracked : m_TrackedManagers)
    {
        UIManager* ui = tracked.Get();
        if (!ui)
            continue;
        ApplyClass(ui, m_InnerShadowEnabled);
        m_TrackedManagers[write++] = tracked;
    }
    m_TrackedManagers.resize(write);
}

void RegisterToggleAppearanceSettings()
{
    SettingsCategoryDescriptor toggles;
    toggles.CategoryId = "toggles";
    toggles.Title = "Toggles";
    toggles.Group = SettingsCategoryGroup::UIAppearance;
    toggles.SearchKeywords = "toggle switch inner shadow inset checkbox";
    toggles.Description = "How boolean switch tracks are drawn.";

    SettingsFieldDescriptor innerShadow;
    innerShadow.Label = "Toggle Inner Shadow";
    innerShadow.Tooltip =
        "Recess switch tracks with a small inset shadow along the top and left "
        "inner edge. Square checkmark toggles are unchanged.";
    innerShadow.SearchKeywords = "toggle inner shadow inset recess switch track";
    innerShadow.PrefKey = kInnerShadowPrefKey;
    SettingsFieldDescriptor::ToggleField innerShadowToggle;
    innerShadowToggle.DefaultValue = false;
    innerShadowToggle.Get = []
    { return ToggleAppearanceSettings::Get().GetInnerShadowEnabled(); };
    innerShadowToggle.Set = [](bool enabled)
    { ToggleAppearanceSettings::Get().SetInnerShadowEnabled(enabled); };
    innerShadow.Control = std::move(innerShadowToggle);
    toggles.Fields.push_back(std::move(innerShadow));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(toggles));
}

} // namespace GameEngine::Editor
