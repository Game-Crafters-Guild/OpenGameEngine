#pragma once

#include <cstdint>
#include <vector>

#include "UI/UIManagerRef.h"

namespace GameEngine
{
class UIManager;

namespace Editor
{

// Shared explanatory-card look (Settings + inspectors): background, outline, drop shadow.
// Values live in Editor Preferences; CSS custom properties are pushed onto each UI root.
class InfoCardAppearanceSettings
{
public:
    static constexpr uint32_t kDefaultBackgroundColor = 0xFF212121u;
    static constexpr uint32_t kDefaultOutlineColor = 0xFF353535u;
    static constexpr bool kDefaultBackgroundEnabled = true;
    static constexpr bool kDefaultOutlineEnabled = true;
    static constexpr bool kDefaultShadowEnabled = false;
    static constexpr bool kDefaultCardsVisible = true;

    static InfoCardAppearanceSettings& Get();

    bool GetBackgroundEnabled() const { return m_BackgroundEnabled; }
    uint32_t GetBackgroundColor() const { return m_BackgroundColor; }
    bool GetOutlineEnabled() const { return m_OutlineEnabled; }
    uint32_t GetOutlineColor() const { return m_OutlineColor; }
    bool GetShadowEnabled() const { return m_ShadowEnabled; }
    bool GetCardsVisible() const { return m_CardsVisible; }

    void SetBackgroundEnabled(bool enabled);
    // persist=false updates the live CSS only (color-picker drag / cancel).
    void SetBackgroundColor(uint32_t argb, bool persist = true);
    void SetOutlineEnabled(bool enabled);
    void SetOutlineColor(uint32_t argb, bool persist = true);
    void SetShadowEnabled(bool enabled);
    // Global on/off for every explanatory card in Settings and the inspectors.
    void SetCardsVisible(bool visible);

    // Track this UIManager and apply the current look. Call once per editor window
    // after its root exists so later preference changes reach every live UI.
    void ApplyTo(UIManager* ui);

private:
    InfoCardAppearanceSettings();

    void Load();
    void Save();
    void ApplyToTrackedManagers();
    void ApplyToManager(UIManager* ui) const;

    bool m_BackgroundEnabled = kDefaultBackgroundEnabled;
    uint32_t m_BackgroundColor = kDefaultBackgroundColor;
    bool m_OutlineEnabled = kDefaultOutlineEnabled;
    uint32_t m_OutlineColor = kDefaultOutlineColor;
    bool m_ShadowEnabled = kDefaultShadowEnabled;
    bool m_CardsVisible = kDefaultCardsVisible;
    std::vector<UIManagerRef> m_TrackedManagers;
};

void RegisterInfoCardAppearanceSettingsCategory();

} // namespace Editor
} // namespace GameEngine
