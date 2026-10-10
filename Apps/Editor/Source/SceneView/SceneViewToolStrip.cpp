#include "SceneView/SceneViewToolStrip.h"

#include "SceneView/SceneViewToolStripRegistry.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Interaction/ContextMenuManipulator.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIEvents.h"

#include <memory>

namespace GameEngine::Editor
{

namespace
{
constexpr std::size_t kLargestShownCount = 99;
constexpr StringId kHiddenClassId = HashStringId("hidden");

std::string_view IdPrefix(ToolStripPlacement placement)
{
    return placement == ToolStripPlacement::Floating ? "ToolStrip." : "InlineToolStrip.";
}

// The entry's menu, rebuilt from the entry registered under `entryId` on every show.
std::shared_ptr<ContextMenuManipulator> MakeEntryMenu(const SceneViewToolStripRegistry& registry,
                                                      const std::string& entryId,
                                                      const RegisteredToolStripActions& actions)
{
    auto menu = ContextMenuManipulator::Create(
        [&registry, entryId, colorPicker = actions.ColorPicker]() -> std::vector<ContextMenuManipulator::Item> {
            const SceneViewToolStripEntry* current = registry.Find(entryId);
            if (!current || !current->ContextMenuItems)
                return {};
            return current->ContextMenuItems(colorPicker ? colorPicker() : OpenColorPickerWindowFn{});
        });
    if (actions.MenuAnchor)
        menu->SetAnchor(actions.MenuAnchor);
    return menu;
}

// Shows or hides one of an entry's buttons; true when that changed it.
bool ShowEntryButton(Button* button, bool shown)
{
    if (!button)
        return false;
    if (!shown)
        button->RemoveClass("icon-active");
    return UI::Layout::SetElementHidden(*button, !shown);
}

} // namespace

std::string RegisteredToolButtonId(ToolStripPlacement placement, std::string_view entryId)
{
    return std::string(IdPrefix(placement)) + std::string(entryId);
}

std::string RegisteredToolBadgeButtonId(ToolStripPlacement placement, std::string_view entryId)
{
    return std::string(IdPrefix(placement)) + std::string(entryId) + ".Badge";
}

std::string RegisteredToolBadgeCountId(ToolStripPlacement placement, std::string_view entryId)
{
    return std::string(IdPrefix(placement)) + std::string(entryId) + ".BadgeCount";
}

void RegisteredToolStrip::Populate(UIElement& strip, ToolStripPlacement placement,
                                   const SceneViewToolStripRegistry& registry,
                                   const RegisteredToolStripActions& actions)
{
    if (m_Strip.Get() != &strip || m_Registry != &registry)
        m_Entries.clear();
    m_Strip = UIElement::MakeWeakRef(&strip);
    m_Registry = &registry;

    const bool floating = placement == ToolStripPlacement::Floating;
    const std::vector<SceneViewToolStripEntry>& entries = registry.Entries();
    for (std::size_t index = m_Entries.size(); index < entries.size(); ++index)
    {
        const SceneViewToolStripEntry& entry = entries[index];
        Entry& kept = m_Entries.emplace_back();
        kept.RegistryIndex = index;

        // An entry with a badge button is a group of its own, its tool beside the panel the
        // badge opens: a divider sets the pair apart from the tools before it.
        if (entry.OnBadgeButton)
        {
            auto divider = std::make_unique<UIElement>();
            divider->AddClass("scene-tool-divider");
            strip.AddChild(std::move(divider));
        }

        auto button = std::make_unique<Button>();
        button->SetId(RegisteredToolButtonId(placement, entry.Id));
        button->AddClass("scene-tool-btn");
        if (!entry.ButtonClass.empty())
            button->AddClass(entry.ButtonClass);
        if (actions.SetIcon)
            actions.SetIcon(*button, entry.Icon);
        button->SetTooltip(entry.Tooltip);
        if (floating)
            button->SetTooltipPlacement(UIElement::TooltipPlacement::Right);
        button->RegisterEventHandler(kEventButtonClick, [toggle = actions.ToggleTool, id = entry.Id](UIEvent&) {
            if (toggle)
                toggle(id);
        });
        if (entry.ContextMenuItems)
            button->AddManipulator(MakeEntryMenu(registry, entry.Id, actions));
        kept.ButtonRef = UIElement::MakeWeakRef(button.get());
        strip.AddChild(std::move(button));

        if (!entry.OnBadgeButton)
            continue;
        // The count is the button's sibling in a host, not its child, so it does not take
        // the resting button's dimmed opacity.
        auto host = std::make_unique<UIElement>();
        host->AddClass("scene-tool-badge-host");
        auto badgeButton = std::make_unique<Button>();
        Button* badgeButtonPtr = badgeButton.get();
        badgeButton->SetId(RegisteredToolBadgeButtonId(placement, entry.Id));
        badgeButton->AddClass("scene-tool-btn");
        if (actions.SetIcon)
            actions.SetIcon(*badgeButton, entry.BadgeButtonIcon);
        badgeButton->SetTooltip(entry.BadgeButtonTooltip);
        if (floating)
            badgeButton->SetTooltipPlacement(UIElement::TooltipPlacement::Right);
        badgeButton->RegisterEventHandler(kEventButtonClick, [&registry, id = entry.Id, badgeButtonPtr](UIEvent&) {
            if (const SceneViewToolStripEntry* current = registry.Find(id); current && current->OnBadgeButton)
                current->OnBadgeButton(*badgeButtonPtr);
        });
        auto count = std::make_unique<Label>();
        count->SetId(RegisteredToolBadgeCountId(placement, entry.Id));
        count->AddClass("scene-tool-badge");
        count->AddClass("hidden");
        kept.CountRef = UIElement::MakeWeakRef(count.get());
        kept.BadgeButtonRef = UIElement::MakeWeakRef(badgeButtonPtr);
        host->AddChild(std::move(badgeButton));
        host->AddChild(std::move(count));
        strip.AddChild(std::move(host));
    }
}

void RegisteredToolStrip::SetActiveEntry(std::string_view activeEntryId)
{
    m_ActiveEntryId = activeEntryId;
    if (!m_Registry)
        return;
    const std::vector<SceneViewToolStripEntry>& entries = m_Registry->Entries();
    for (const Entry& kept : m_Entries)
    {
        Button* button = kept.ButtonRef.Get();
        if (!button || kept.RegistryIndex >= entries.size())
            continue;
        if (kept.Available && entries[kept.RegistryIndex].Id == activeEntryId)
            button->AddClass("icon-active");
        else
            button->RemoveClass("icon-active");
    }
}

void RegisteredToolStrip::Refresh(ECS::World* world)
{
    UIElement* strip = m_Strip.Get();
    if (!strip || !m_Registry || strip->HasClass(kHiddenClassId))
        return;
    const std::vector<SceneViewToolStripEntry>& entries = m_Registry->Entries();
    bool shownChanged = false;
    for (Entry& kept : m_Entries)
    {
        if (kept.RegistryIndex >= entries.size())
            continue;
        const SceneViewToolStripEntry& entry = entries[kept.RegistryIndex];
        if (entry.IsAvailable)
            shownChanged |= ShowAvailability(kept, entry.IsAvailable(world));
        if (entry.BadgeCount)
            ShowCount(kept, entry.BadgeCount());
    }
    // The strips are content-sized: a shown or hidden button changes their extent in
    // either orientation.
    if (shownChanged)
        strip->RequestRelayout();
}

bool RegisteredToolStrip::ShowAvailability(Entry& entry, bool available)
{
    if (available == entry.Available)
        return false;
    entry.Available = available;
    const bool buttonChanged = ShowEntryButton(entry.ButtonRef.Get(), available);
    if (Button* button = entry.ButtonRef.Get(); available && button && m_Registry &&
                                                 entry.RegistryIndex < m_Registry->Entries().size() &&
                                                 m_Registry->Entries()[entry.RegistryIndex].Id == m_ActiveEntryId)
        button->AddClass("icon-active");
    const bool badgeChanged = ShowEntryButton(entry.BadgeButtonRef.Get(), available);
    return buttonChanged || badgeChanged;
}

void RegisteredToolStrip::ShowCount(Entry& entry, std::size_t count)
{
    if (entry.CountShown && count == entry.ShownCount)
        return;
    Label* label = entry.CountRef.Get();
    if (!label)
        return;
    entry.ShownCount = count;
    entry.CountShown = true;
    if (count == 0)
    {
        label->AddClass("hidden");
        return;
    }
    label->RemoveClass("hidden");
    label->SetText(count > kLargestShownCount ? std::string("99+") : std::to_string(count));
}

} // namespace GameEngine::Editor
