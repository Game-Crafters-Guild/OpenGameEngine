#include "Inspectors/InspectorComponentSection.h"

#include "EditorContextMenu/UIContextMenu.h"

#include "Platform/ContextMenu.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace GameEngine::InspectorUI
{
namespace
{

// A section's open/shut state, keyed by the caller's key. Node-based on purpose: the flag is
// handed to a handler by reference and outlives the panel that opened it.
bool& ExpandedFlag(std::string_view key, bool expandedByDefault)
{
    static std::map<std::string, bool, std::less<>> flags;
    if (auto it = flags.find(key); it != flags.end())
        return it->second;
    return flags.emplace(std::string(key), expandedByDefault).first->second;
}

// Opens the section's menu at (x, y) in window client pixels.
//
// The menu is built per invocation rather than kept alive between them: an item's enablement
// tracks state the panel rebuilds around it (a cap reached, a last item that may not be removed),
// and a menu built once would go on offering whatever was true when the panel was built. The
// handlers run against the same rebuild-and-refresh path a button did, so nothing here outlives
// the click.
void ShowSectionMenu(Platform::Window* window, const std::vector<SectionMenuItem>& items,
                     float x, float y)
{
    // One menu, rebuilt per invocation. Kept alive between shows because the editor-drawn backend
    // outlives the call that opens it, and rebuilt because an item's enablement tracks state the
    // panel changes around it — a cap reached, a last item that may not be removed — so a menu
    // built once would go on offering whatever was true when it was first opened.
    static std::shared_ptr<INativeContextMenu> s_Menu;
    if (!s_Menu)
    {
        s_Menu = CreateContextMenu();
        if (!s_Menu)
            return;
    }
    s_Menu->Clear();

    // Command ids are 1-based: 0 is the root parent id AddItem takes, so it is not a command.
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const SectionMenuItem& item = items[i];
        const auto command = static_cast<uint32_t>(i + 1);
        const std::string title =
            item.Enabled || item.Reason.empty() ? item.Label : item.Label + " (" + item.Reason + ")";
        s_Menu->AddItem(0, title, command);
        if (item.Icon)
            s_Menu->SetItemIcon(command, item.Icon);
        if (!item.Enabled)
            s_Menu->SetItemEnabled(command, false);
    }

    // Copied into the handler: the caller's vector is a per-build temporary, and the menu invokes
    // this after the click, by which time that build is gone.
    s_Menu->SetCommandHandler([items](uint32_t command) {
        if (command == 0 || command > items.size())
            return;
        const SectionMenuItem& item = items[command - 1];
        if (item.Enabled && item.OnInvoke)
            item.OnInvoke();
    });

    s_Menu->Show(window, static_cast<int>(x), static_cast<int>(y));
}

} // namespace

Foldout* AddComponentSection(UIElement* parent, std::string_view key, std::string_view title,
                             bool expandedByDefault)
{
    if (!parent)
        return nullptr;

    bool& expanded = ExpandedFlag(key, expandedByDefault);

    auto section = std::make_unique<Foldout>();
    Foldout* raw = section.get();
    raw->SetTitle(std::string(title));
    raw->AddClass("rp-foldout");
    raw->AddClass("inspector-component-section");
    raw->SetExpanded(expanded);
    raw->SetOnExpandedChanged([&expanded](Foldout&, bool nowExpanded) { expanded = nowExpanded; });

    parent->AddChild(std::move(section));
    return raw;
}

void AddSectionEnableToggle(Foldout* section, bool enabled, const std::string& tooltip,
                            std::function<void(bool)> onChanged)
{
    if (!section)
        return;
    auto toggle = std::make_unique<ToggleBase>();
    toggle->AddClass("inspector-foldout-enable-dot");
    toggle->SetValueWithoutNotify(enabled);
    toggle->SetTooltip(tooltip);
    auto dot = std::make_unique<UIElement>();
    dot->AddClass("inspector-foldout-enable-dot-visual");
    toggle->AddChild(std::move(dot));
    toggle->SetOnValueChanged(std::move(onChanged));
    section->GetHeader()->InsertChild(0, std::move(toggle));
}

void AddSectionHeaderMenu(Foldout* section, Platform::Window* window,
                          std::vector<SectionMenuItem> items)
{
    if (!section || !window || items.empty())
        return;

    UIElement* header = section->GetHeader();
    if (!header)
        return;

    auto shared = std::make_shared<std::vector<SectionMenuItem>>(std::move(items));

    // The same affordance the component headers carry, so one glyph means "what can I do to this
    // block" wherever it appears.
    auto options = std::make_unique<UIElement>();
    UIElement* optionsRaw = options.get();
    optionsRaw->AddClass("button");
    optionsRaw->AddClass("icon-button");
    optionsRaw->AddClass("inspector-section-header-options");
    optionsRaw->SetTooltip("Actions for this section.");
    header->AddChild(std::move(options));

    // Consumed on the affordance, so the press does not also reach the header and toggle the
    // fold: clicking the gear opens the menu, it does not collapse what the menu acts on.
    optionsRaw->RegisterEventHandler(kEventMouseDown, [shared, window](UIEvent& e) {
        ShowSectionMenu(window, *shared, e.X, e.Y);
        e.Stop();
    });

    // Right-click anywhere on the bar, the gesture tried before the glyph is spotted.
    header->RegisterEventHandler(kEventMouseDown, [shared, window](UIEvent& e) {
        if (e.Button != 1)
            return;
        ShowSectionMenu(window, *shared, e.X, e.Y);
        e.Stop();
    });
}

} // namespace GameEngine::InspectorUI
