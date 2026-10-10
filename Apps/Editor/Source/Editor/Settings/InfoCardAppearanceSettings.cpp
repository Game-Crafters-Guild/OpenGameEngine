#include "Editor/Settings/InfoCardAppearanceSettings.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "Types/StringId.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Mount.h"
#include "UI/InfoCard.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <cstdio>
#include <memory>
#include <string>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kPrefKeyBackground = "ui.infoCardBackground";
constexpr const char* kPrefKeyBackgroundColor = "ui.infoCardBackgroundColor";
constexpr const char* kPrefKeyOutline = "ui.infoCardOutline";
constexpr const char* kPrefKeyOutlineColor = "ui.infoCardOutlineColor";
constexpr const char* kPrefKeyShadow = "ui.infoCardShadow";
constexpr const char* kPrefKeyVisible = "ui.infoCardsVisible";

void WriteHexColor(char (&out)[8], uint32_t argb)
{
    std::snprintf(out, sizeof(out), "#%02X%02X%02X",
                  (argb >> 16) & 0xFF,
                  (argb >> 8) & 0xFF,
                  argb & 0xFF);
}

void ApplyBackground(UIManager* ui, bool enabled, uint32_t argb)
{
    if (!ui)
        return;
    UIElement* root = ui->GetRootElement();
    if (!root)
        return;

    const StringId backgroundVar = HashStringId("--ui_info_card_background");
    if (enabled)
    {
        char color[8];
        WriteHexColor(color, argb);
        root->Overrides().SetCustom(backgroundVar, color);
    }
    else
    {
        root->Overrides().SetCustom(backgroundVar, "transparent");
    }
    ui->MarkStyleDirtyAll();
}

void ApplyShadow(UIManager* ui, bool enabled)
{
    if (!ui)
        return;
    UIElement* root = ui->GetRootElement();
    if (!root)
        return;

    const StringId shadowVar = HashStringId("--ui_info_card_shadow");
    if (enabled)
        root->Overrides().SetCustom(shadowVar, "var(--popup-shadow, 0 -1px 24px rgba(0, 0, 0, 0.25))");
    else
        root->Overrides().SetCustom(shadowVar, "none");
    ui->MarkStyleDirtyAll();
}

void ApplyOutline(UIManager* ui, bool enabled, uint32_t argb)
{
    if (!ui)
        return;
    UIElement* root = ui->GetRootElement();
    if (!root)
        return;

    char color[8];
    WriteHexColor(color, argb);
    root->Overrides().SetCustom(HashStringId("--ui_info_card_outline_color"), color);
    if (enabled)
    {
        root->Overrides().SetCustom(HashStringId("--ui_info_card_outline_width"), "1px");
        root->Overrides().SetCustom(HashStringId("--ui_info_card_outline_style"), "solid");
    }
    else
    {
        root->Overrides().SetCustom(HashStringId("--ui_info_card_outline_width"), "0px");
        root->Overrides().SetCustom(HashStringId("--ui_info_card_outline_style"), "none");
    }
    ui->MarkStyleDirtyAll();
}

// Applied per card: a descendant selector rooted at the UI root does not reach
// panel content, so the class goes on each card element instead.
//
// Docked panels hang off a Mount, which does not own its target and so does not
// report it as a child — the walk follows those targets or it never leaves the
// dock chrome.
void SetCardVisibility(UIElement* element, bool visible)
{
    if (!element)
        return;

    if (element->HasClass("editor-info-card") &&
        !element->HasClass(EditorUI::kInfoCardAlwaysVisibleClass))
    {
        if (visible)
            element->RemoveClass(EditorUI::kInfoCardOffClass);
        else
            element->AddClass(EditorUI::kInfoCardOffClass);

        // display:none is a layout-structure change (Yoga prunes the node), so
        // a style mark alone leaves the card's box where it was.
        element->MarkDirtySubtree(UIElement::StyleDirty | UIElement::LayoutDirty |
                                  UIElement::VisualDirty);
    }

    if (auto* mount = dynamic_cast<Mount*>(element))
        SetCardVisibility(mount->GetTarget(), visible);

    for (const auto& child : element->GetChildren())
        SetCardVisibility(child.get(), visible);
}

void ApplyVisibility(UIManager* ui, bool visible)
{
    if (!ui)
        return;
    SetCardVisibility(ui->GetRootElement(), visible);
    ui->MarkStyleDirtyAll();
}

} // namespace

InfoCardAppearanceSettings& InfoCardAppearanceSettings::Get()
{
    static InfoCardAppearanceSettings instance;
    return instance;
}

InfoCardAppearanceSettings::InfoCardAppearanceSettings()
{
    Load();
}

void InfoCardAppearanceSettings::Load()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);

    prefs.TryGetBool(kPrefKeyBackground, m_BackgroundEnabled);
    int64_t backgroundColor = static_cast<int64_t>(kDefaultBackgroundColor);
    prefs.TryGetInt64(kPrefKeyBackgroundColor, backgroundColor);
    m_BackgroundColor = static_cast<uint32_t>(backgroundColor);

    prefs.TryGetBool(kPrefKeyOutline, m_OutlineEnabled);
    int64_t outlineColor = static_cast<int64_t>(kDefaultOutlineColor);
    prefs.TryGetInt64(kPrefKeyOutlineColor, outlineColor);
    m_OutlineColor = static_cast<uint32_t>(outlineColor);

    prefs.TryGetBool(kPrefKeyShadow, m_ShadowEnabled);
    prefs.TryGetBool(kPrefKeyVisible, m_CardsVisible);
}

void InfoCardAppearanceSettings::Save()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);

    prefs.SetBool(kPrefKeyBackground, m_BackgroundEnabled);
    prefs.SetInt64(kPrefKeyBackgroundColor, static_cast<int64_t>(m_BackgroundColor));
    prefs.SetBool(kPrefKeyOutline, m_OutlineEnabled);
    prefs.SetInt64(kPrefKeyOutlineColor, static_cast<int64_t>(m_OutlineColor));
    prefs.SetBool(kPrefKeyShadow, m_ShadowEnabled);
    prefs.SetBool(kPrefKeyVisible, m_CardsVisible);
    prefs.Save(&error);
}

void InfoCardAppearanceSettings::ApplyToManager(UIManager* ui) const
{
    ApplyBackground(ui, m_BackgroundEnabled, m_BackgroundColor);
    ApplyOutline(ui, m_OutlineEnabled, m_OutlineColor);
    ApplyShadow(ui, m_ShadowEnabled);
    ApplyVisibility(ui, m_CardsVisible);
}

void InfoCardAppearanceSettings::ApplyTo(UIManager* ui)
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

    ApplyToManager(ui);
}

void InfoCardAppearanceSettings::ApplyToTrackedManagers()
{
    size_t write = 0;
    for (const UIManagerRef& tracked : m_TrackedManagers)
    {
        UIManager* ui = tracked.Get();
        if (!ui)
            continue;
        ApplyToManager(ui);
        m_TrackedManagers[write++] = tracked;
    }
    m_TrackedManagers.resize(write);
}

void InfoCardAppearanceSettings::SetBackgroundEnabled(bool enabled)
{
    if (m_BackgroundEnabled == enabled)
        return;
    m_BackgroundEnabled = enabled;
    Save();
    ApplyToTrackedManagers();
}

void InfoCardAppearanceSettings::SetBackgroundColor(uint32_t argb, bool persist)
{
    const uint32_t opaque = 0xFF000000u | (argb & 0x00FFFFFFu);
    const bool colorChanged = m_BackgroundColor != opaque;
    if (!colorChanged && !persist)
        return;
    m_BackgroundColor = opaque;
    if (persist)
        Save();
    if (colorChanged)
        ApplyToTrackedManagers();
}

void InfoCardAppearanceSettings::SetOutlineEnabled(bool enabled)
{
    if (m_OutlineEnabled == enabled)
        return;
    m_OutlineEnabled = enabled;
    Save();
    ApplyToTrackedManagers();
}

void InfoCardAppearanceSettings::SetOutlineColor(uint32_t argb, bool persist)
{
    const uint32_t opaque = 0xFF000000u | (argb & 0x00FFFFFFu);
    const bool colorChanged = m_OutlineColor != opaque;
    if (!colorChanged && !persist)
        return;
    m_OutlineColor = opaque;
    if (persist)
        Save();
    if (colorChanged)
        ApplyToTrackedManagers();
}

void InfoCardAppearanceSettings::SetShadowEnabled(bool enabled)
{
    if (m_ShadowEnabled == enabled)
        return;
    m_ShadowEnabled = enabled;
    Save();
    ApplyToTrackedManagers();
}

void InfoCardAppearanceSettings::SetCardsVisible(bool visible)
{
    if (m_CardsVisible == visible)
        return;
    m_CardsVisible = visible;
    Save();
    ApplyToTrackedManagers();
}

void RegisterInfoCardAppearanceSettingsCategory()
{
    SettingsCategoryDescriptor infoCards;
    infoCards.CategoryId = "infoCards";
    infoCards.Title = "Info Cards";
    infoCards.Group = SettingsCategoryGroup::UIAppearance;
    infoCards.SearchKeywords = "info card background outline shadow explanatory help";
    infoCards.Description =
        "Look of shared explanatory cards in Settings and inspectors.";
    // This page owns the visibility switch, so its own description card has to
    // outlive it.
    infoCards.DescriptionAlwaysVisible = true;

    {
        SettingsFieldDescriptor visible;
        visible.Label = "Show Info Cards";
        visible.SearchKeywords = "info card show hide visible explanatory help toggle";
        SettingsFieldDescriptor::ToggleField control;
        control.DefaultValue = InfoCardAppearanceSettings::kDefaultCardsVisible;
        control.Get = [] { return InfoCardAppearanceSettings::Get().GetCardsVisible(); };
        control.Set = [](bool visible)
        { InfoCardAppearanceSettings::Get().SetCardsVisible(visible); };
        visible.Control = std::move(control);
        infoCards.Fields.push_back(std::move(visible));
    }

    {
        SettingsFieldDescriptor background;
        background.Label = "Info Card Background";
        background.SearchKeywords = "info card background toggle fill color";
        SettingsFieldDescriptor::ToggleColorField control;
        control.DefaultEnabled = InfoCardAppearanceSettings::kDefaultBackgroundEnabled;
        control.DefaultArgb = InfoCardAppearanceSettings::kDefaultBackgroundColor;
        control.GetEnabled = []
        { return InfoCardAppearanceSettings::Get().GetBackgroundEnabled(); };
        control.SetEnabled = [](bool enabled)
        { InfoCardAppearanceSettings::Get().SetBackgroundEnabled(enabled); };
        control.GetColor = []
        { return InfoCardAppearanceSettings::Get().GetBackgroundColor(); };
        control.SetColor = [](uint32_t argb, bool persist)
        { InfoCardAppearanceSettings::Get().SetBackgroundColor(argb, persist); };
        background.Control = std::move(control);
        infoCards.Fields.push_back(std::move(background));
    }

    {
        SettingsFieldDescriptor outline;
        outline.Label = "Info Card Outline";
        outline.SearchKeywords = "info card outline border stroke toggle color";
        SettingsFieldDescriptor::ToggleColorField control;
        control.DefaultEnabled = InfoCardAppearanceSettings::kDefaultOutlineEnabled;
        control.DefaultArgb = InfoCardAppearanceSettings::kDefaultOutlineColor;
        control.GetEnabled = []
        { return InfoCardAppearanceSettings::Get().GetOutlineEnabled(); };
        control.SetEnabled = [](bool enabled)
        { InfoCardAppearanceSettings::Get().SetOutlineEnabled(enabled); };
        control.GetColor = []
        { return InfoCardAppearanceSettings::Get().GetOutlineColor(); };
        control.SetColor = [](uint32_t argb, bool persist)
        { InfoCardAppearanceSettings::Get().SetOutlineColor(argb, persist); };
        outline.Control = std::move(control);
        infoCards.Fields.push_back(std::move(outline));
    }

    {
        SettingsFieldDescriptor shadow;
        shadow.Label = "Info Card Drop Shadow";
        shadow.SearchKeywords = "info card drop shadow depth toggle";
        SettingsFieldDescriptor::ToggleField control;
        control.DefaultValue = InfoCardAppearanceSettings::kDefaultShadowEnabled;
        control.Get = [] { return InfoCardAppearanceSettings::Get().GetShadowEnabled(); };
        control.Set = [](bool on) { InfoCardAppearanceSettings::Get().SetShadowEnabled(on); };
        shadow.Control = std::move(control);
        infoCards.Fields.push_back(std::move(shadow));
    }

    EditorSettingsRegistry::Get().RegisterCategory(std::move(infoCards));
}

} // namespace GameEngine::Editor
