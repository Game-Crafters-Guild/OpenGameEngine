#pragma once

#include "InspectorRegistry.h"
#include "Mathematics/Vector2.h"
#include "UI/UIElement.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace ECS { class World; }
class Button;
class Label;
} // namespace GameEngine

namespace GameEngine::Editor
{
class SceneViewToolStripRegistry;

// The Scene View's two tool strips: the floating strip over the viewport and its inline
// mirror in the toolbar (shown while the floating one is hidden). Both carry every
// registered entry.
enum class ToolStripPlacement : std::uint8_t
{
    Floating,
    Inline
};

// What the Scene View tool strip asks of its view for registered entries
// (SceneView/SceneViewToolStripRegistry.h).
struct RegisteredToolStripActions
{
    // A click on an entry's button: activate its tool, or leave it when it is active.
    std::function<void(const std::string& entryId)> ToggleTool;
    // Gives a button its icon (an editor icon, as EditorIcons names one).
    std::function<void(UIElement& button, std::string_view icon)> SetIcon;
    // The view's color picker for an entry's menu, read each time the menu opens.
    std::function<OpenColorPickerWindowFn()> ColorPicker;
    // Where an entry's menu opens, in window layout pixels, for its button.
    std::function<Mathematics::Vector2(const UIElement& button)> MenuAnchor;
};

// The registered entries' buttons in one Scene View tool strip. Populate appends them and
// keeps each entry's button, badge button and badge label, so the refreshes walk no
// element tree and build no strings.
class RegisteredToolStrip
{
public:
    // Appends to `strip` a button for every entry of `registry` (with the entry's menu when
    // it has one; an entry with a badge button gets a divider before its button and the badge
    // button with the count after it, so the two read as one group), after the buttons
    // already there and in registration order. A second call
    // on the same strip appends only the entries registered since; a call on another strip
    // (a layout reload) starts over.
    void Populate(UIElement& strip, ToolStripPlacement placement, const SceneViewToolStripRegistry& registry,
                  const RegisteredToolStripActions& actions);

    // Marks the button of `activeEntryId` active and every other registered one not; the
    // view calls it when its tool changes. An entry unavailable now takes the mark when
    // Refresh shows it again, so a strip hidden across the change shows the active tool.
    void SetActiveEntry(std::string_view activeEntryId);

    // Hides the buttons of each entry that is not available and shows each badge's count
    // (hidden at 0, "99+" above 99), reading the entries' cheap providers and writing an
    // element only when what it shows changed. Does nothing while the strip is hidden; the
    // view calls it once per frame.
    void Refresh(ECS::World* world);

private:
    struct Entry
    {
        std::size_t RegistryIndex = 0;
        UIElement::WeakRef<Button> ButtonRef;
        UIElement::WeakRef<Button> BadgeButtonRef;
        UIElement::WeakRef<Label> CountRef;
        bool Available = true;
        std::size_t ShownCount = 0;
        bool CountShown = false;
    };

    // True when the entry's buttons were shown or hidden. An entry shown again takes the
    // active mark when it is the active entry.
    bool ShowAvailability(Entry& entry, bool available);
    static void ShowCount(Entry& entry, std::size_t count);

    UIElement::WeakRef<UIElement> m_Strip;
    const SceneViewToolStripRegistry* m_Registry = nullptr;
    std::vector<Entry> m_Entries;
    std::string m_ActiveEntryId; // the last SetActiveEntry; empty for none
};

// The ids the strip gives an entry's elements; the two placements' ids differ.
std::string RegisteredToolButtonId(ToolStripPlacement placement, std::string_view entryId);
std::string RegisteredToolBadgeButtonId(ToolStripPlacement placement, std::string_view entryId);
std::string RegisteredToolBadgeCountId(ToolStripPlacement placement, std::string_view entryId);

} // namespace GameEngine::Editor
