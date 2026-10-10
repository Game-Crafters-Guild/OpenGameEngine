#pragma once

#include "InspectorRegistry.h"
#include "Mathematics/Vector3.h"
#include "UI/Interaction/ContextMenuManipulator.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class SceneViewController;
class UIElement;
namespace ECS
{
class World;
struct EntityHandle;
} // namespace ECS
namespace Editor::SceneTools
{
class ISceneTool;
}
} // namespace GameEngine

namespace GameEngine::Editor
{

// One button in the Scene View's tool strips (the floating strip and its inline mirror
// in the toolbar), contributed by the module that owns the tool. The strips append
// registered entries after their built-in buttons, in registration order; a click
// activates the entry's tool in that view (and a second click returns to the transform
// tool).
struct SceneViewToolStripEntry
{
    std::string Id;      // unique across the strip, e.g. "markups"; the debug server's tool name
    std::string Tooltip; // shown beside the button
    std::string Icon;    // an editor icon as EditorIcons names one, e.g. "editor:Icons/markup.svg"
    // Optional: an extra class on the entry's buttons, for a style of its own (an
    // active color).
    std::string ButtonClass;
    // Builds the entry's tool for one Scene View; the view's controller owns it. The
    // controller's undo and change-notification services are set before any tool is
    // built.
    std::function<std::unique_ptr<SceneTools::ISceneTool>(SceneViewController&)> CreateTool;
    // Optional: false hides the entry's buttons, and the view refuses to activate the
    // entry's tool (a view whose active tool is the entry's returns to the transform
    // tool). Read once per frame while the visible strip shows, so it must be cheap.
    std::function<bool(ECS::World* world)> IsAvailable;
    // With IsAvailable: what its false means and how to change it, the refusal of an
    // activation while the entry is not available.
    std::string UnavailableReason;
    // Optional: the menu a right-click on the entry's buttons opens, built on every show;
    // `openColorPicker` is the view's color picker, empty when the editor has none.
    std::function<std::vector<ContextMenuManipulator::Item>(const OpenColorPickerWindowFn& openColorPicker)>
        ContextMenuItems;
    // Optional: true when the view's new selection makes the entry's tool the active one
    // (a spline selected activates the spline tool). `primary` is the selection's
    // primary entity.
    std::function<bool(ECS::World& world, ECS::EntityHandle primary, const std::vector<ECS::EntityHandle>& selection)>
        ActivatesForSelection;
    // Optional: the point Frame Selected centres on while the entry holds a selection
    // finer than an entity (a spline knot); false when it holds none.
    std::function<bool(ECS::World& world, Mathematics::Vector3& outWorldPosition)> FrameTarget;
    // Optional: the count the button's badge shows; 0 hides the badge. Read once per
    // frame while the visible strip shows, so it must be cheap (cache it between changes).
    std::function<std::size_t()> BadgeCount;
    // Optional: a smaller button beside the entry's, with the badge on it instead, for
    // the panel the badge counts for (the mark-ups' Activity). Pressing it calls this.
    std::function<void(UIElement& anchor)> OnBadgeButton;
    std::string BadgeButtonTooltip;
    std::string BadgeButtonIcon; // an editor icon, as Icon
};

// The Scene View tool strip's registered entries. Main thread; registration replaces an
// entry with the same id in place.
class SceneViewToolStripRegistry
{
public:
    static SceneViewToolStripRegistry& Get();

    void Register(SceneViewToolStripEntry entry);
    const std::vector<SceneViewToolStripEntry>& Entries() const { return m_Entries; }
    const SceneViewToolStripEntry* Find(std::string_view id) const;

private:
    std::vector<SceneViewToolStripEntry> m_Entries;
};

// Why a view cannot activate the tool of entry `id` now: no entry has the id, or the entry
// is not available (its UnavailableReason). Empty when it can.
std::string RegisteredToolRefusal(const SceneViewToolStripRegistry& registry, std::string_view id, ECS::World* world);

} // namespace GameEngine::Editor
