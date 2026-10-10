#pragma once

#include "Editor/Settings/InfoCardAppearanceSettings.h"
#include "UI/UIElement.h"

namespace GameEngine::EditorUI
{

// Class every explanatory card carries while the global "Show Info Cards"
// setting is off. Cards built while it is off start with it; the setting
// adds and removes it on the cards already on screen.
inline constexpr const char* kInfoCardOffClass = "info-card-off";

// A card the global switch must not hide — the one explaining the switch
// itself. Without it the setting's own page loses the text describing what
// was just turned off.
inline constexpr const char* kInfoCardAlwaysVisibleClass = "editor-info-card-always";

inline void StyleInfoCard(UIElement* card)
{
    if (!card)
        return;
    card->AddClass("editor-info-card");
    if (!Editor::InfoCardAppearanceSettings::Get().GetCardsVisible())
        card->AddClass(kInfoCardOffClass);
}

inline void MarkInfoCardAlwaysVisible(UIElement* card)
{
    if (!card)
        return;
    card->AddClass(kInfoCardAlwaysVisibleClass);
    card->RemoveClass(kInfoCardOffClass);
}

inline void StyleInfoCardText(UIElement* text)
{
    if (text)
        text->AddClass("editor-info-card-text");
}

inline void StyleTextInfoCard(UIElement* text)
{
    StyleInfoCard(text);
    StyleInfoCardText(text);
}

} // namespace GameEngine::EditorUI
