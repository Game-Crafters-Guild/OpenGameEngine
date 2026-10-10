#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class Foldout;
class UIElement;
namespace Platform { class Window; }
} // namespace GameEngine

// The sections a component inspector groups its own rows into — a level below InspectorSection,
// which heads the component itself. One implementation, so every panel that groups rows under a
// heading gets the SAME heading: the full-width bar the material and terrain inspectors wear, over
// a body the stylesheet indents. A hand-built Foldout is how a panel ends up looking like a
// different program from the one beside it.
namespace GameEngine::InspectorUI
{

// Adds a section to `parent` and returns it; rows go into its GetContentContainer(), so collapsing
// the header takes them with it. A section nested inside another indents instead of pulling out to
// the panel edges — the stylesheet reads the nesting, callers do not pad.
//
// `key` identifies the section across panel rebuilds so a collapsed section stays collapsed. The
// panel is rebuilt far more often than a user changes their mind about a section — a selection
// change, an undo and every discrete edit request a refresh — so without a stable key a section
// could be closed but never stay closed. Titles repeat across panels and across nesting levels
// ("Grass" is both a TerrainGrass section and a material layer), so scope the key rather than
// letting a title collide: "Terrain/Advanced", "SurfaceRules/Rule0/Condition1".
Foldout* AddComponentSection(UIElement* parent, std::string_view key, std::string_view title,
                             bool expandedByDefault = true);

// One entry in a section header's options menu.
//
// A disabled entry stays in the menu rather than disappearing from it: the actions a section
// offers are part of what the section IS, and a menu whose length changes with state makes the
// author hunt for an item that was there a moment ago. `Reason` is what the entry says when it is
// disabled — a greyed row with no explanation is the failure this replaces.
struct SectionMenuItem
{
    std::string Label;
    const char* Icon = nullptr; // an EditorIcons::k* path, or nullptr for no icon
    bool Enabled = true;
    std::string Reason;         // appended to the label when disabled; ignored when enabled
    std::function<void()> OnInvoke;
};

// Gives a section's header the options affordance the component headers wear, opening `items` as
// a native menu. Right-clicking anywhere on the header bar opens the same menu, which is the
// gesture a user tries first.
//
// This is where a section's per-item actions belong. Laid out as buttons they cost a full row
// each in the value column and pile up at the bottom of the block — three stacked bars under
// every rule, ahead of the next rule's heading — which reads as content rather than as chrome.
//
// Builds nothing without a window to parent the native menu to (`ctx.Window`).
void AddSectionHeaderMenu(Foldout* section, Platform::Window* window,
                          std::vector<SectionMenuItem> items);

// Puts the round on/off toggle the component headers wear at the start of a section's header, for
// a section that stands for something the author can switch off (a processor, an effect).
// `onChanged` runs with the new state; `tooltip` says what switching it does.
void AddSectionEnableToggle(Foldout* section, bool enabled, const std::string& tooltip,
                            std::function<void(bool)> onChanged);

} // namespace GameEngine::InspectorUI
