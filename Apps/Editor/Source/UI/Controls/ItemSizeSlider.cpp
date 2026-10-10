#include "UI/Controls/ItemSizeSlider.h"

#include "Editor/Shortcuts/EditorShortcuts.h"
#include "UI/Interaction/ItemResizeGesture.h"
#include "UI/UIEvents.h"

#include <string>
#include <vector>

namespace GameEngine::EditorUI
{
namespace
{
constexpr const char* kStyleAssetPath = "UI/controls/ItemSizeSlider/ItemSizeSlider.css";

// Names the gesture's first binding, the one the Keyboard Shortcuts row lists first.
std::string TooltipFor(const std::vector<Editor::ShortcutBinding>& bindings)
{
    if (bindings.empty() || bindings.front().key == 0)
        return "Item size. Bind Resize Items under Keyboard Shortcuts > View Navigation to resize "
               "the items with the scroll wheel.";
    return "Item size. " + Editor::FormatShortcutBinding(bindings.front().key, bindings.front().mods) +
           " over the items or this slider also resizes them. Rebind under Keyboard Shortcuts > "
           "View Navigation > Resize Items.";
}
} // namespace

ItemSizeSlider::ItemSizeSlider()
{
    AddClass("item-size-slider");
    SetShowValueBubble(false);
    SetTooltip(TooltipFor(Editor::ItemResizeGestureBindings()));
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    m_ShortcutBindingsSaved = Editor::ShortcutBindingsSaved().Subscribe(
        [this](const Editor::ShortcutCatalogEntry& entry, const std::vector<Editor::ShortcutBinding>& bindings)
        {
            if (&entry == &Editor::ItemResizeGestureCatalogEntry())
                SetTooltip(TooltipFor(bindings));
        });

    RegisterEventHandler(kEventScroll, [this](UIEvent& e)
    {
        if (e.ScrollY == 0.0f || !m_OnResizeGesture || !IsEnabledInHierarchy() ||
            !UI::MatchesItemResizeGesture(e.Mods))
            return;
        m_OnResizeGesture(e.ScrollY);
        e.Stop();
    });
}

} // namespace GameEngine::EditorUI
