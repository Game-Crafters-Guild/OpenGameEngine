#pragma once

#include "UI/UIElement.h"
#include "UI/ToolbarDragDrop.h"
#include "Types/StringId.h"
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace GameEngine
{

class Button;
class SceneViewController;


// Editor-specific toolbar shown above the Scene View viewport.
// Instantiated from UXML via the UI element factory registry.
class SceneViewToolbar final : public UIElement
{
public:
    SceneViewToolbar();
    ~SceneViewToolbar() override = default;

    void SetSceneController(SceneViewController* controller);
    void SetOnToggleView2D(std::function<bool()> cb)
    {
        m_OnToggleView2D = std::move(cb);
    }
    // The "Open Grid Settings..." / "Open Snap Settings..." rows in this toolbar's grid
    // and snap menus route to the settings panel through the host.
    void SetOnOpenGridSettings(std::function<void()> cb)
    {
        m_OnOpenGridSettings = std::move(cb);
    }
    // The auto-exposure toggle seeds Fixed EV100 as a side effect (AE-lock); the host uses
    // this to sync an open camera-settings popup whose stale field would clobber the seed.
    void SetOnPostFxAutoExposureToggled(std::function<void()> cb)
    {
        m_OnPostFxAutoExposureToggled = std::move(cb);
    }
    // Left-click on the camera button; opens the scene camera quick-settings popup
    // anchored below it (window coordinates).
    void SetOnOpenCameraSettings(std::function<void(float windowX, float windowY)> cb)
    {
        m_OnOpenCameraSettings = std::move(cb);
    }
    void UpdateGizmoButtonState();
    void UpdateGridButtonState();
    void UpdateSnapButtonState();
    void UpdateView2DButtonState();
    void UpdatePostProcessButtonState();
    void UpdatePickModeButtonState();

    // Enable drag-to-reorder for the scene view toolbar buttons.
    // rootEl must be the main window root so ghosts appear above all other UI.
    // Call once after the toolbar is mounted in the UI tree.
    void SetupDragDrop(UIElement* rootEl, Editor::ToolbarDragDrop* dragDrop);


    void OnPostLayout() override;

private:
    void WireControls();
    /// Opens a wiring pass: resolves every id the pass will ask for in ONE subtree
    /// walk. Returns false when the pass can be skipped outright, in which case
    /// AcquireUnwiredButton returns nullptr for every id and WireControls must not
    /// assume anything was resolved.
    bool BeginWiringPass();
    /// One pre-order walk filling `Resolved` on every registered entry. Visit order
    /// matches UIElement::FindById (self, children in order, then a Mount target), so
    /// a duplicate id resolves to the same element a per-id lookup would have found.
    void ResolveWiredButtons();
    /// Hands `id`'s button back only the first time this toolbar sees that element
    /// instance; returns nullptr when the node is absent or already wired. Wiring is
    /// therefore one-shot per button instance: repeat WireControls passes bind
    /// nothing, a node that mounts late (UXML children bind asynchronously) is bound
    /// when it appears, and a replaced node carries a new instance id and is bound
    /// again. Only valid inside a pass BeginWiringPass opened.
    Button* AcquireUnwiredButton(std::string_view id);
    /// Re-reads controller/settings state into the icon-active classes. Idempotent:
    /// AddClass/RemoveClass no-op when the class is already in the wanted state.
    void RefreshButtonStates();
    void UpdateProjectionButtonState();
    void UpdateRotationGizmoButtonState();

    SceneViewController*   m_Controller = nullptr; // not owned
    Editor::ToolbarDragDrop* m_DragDrop = nullptr; // not owned
    UIElement*             m_DragRootEl = nullptr; // not owned — main window root for ghosts
    std::function<bool()> m_OnToggleView2D;
    std::function<void()> m_OnOpenGridSettings;
    std::function<void()> m_OnPostFxAutoExposureToggled;
    std::function<void(float, float)> m_OnOpenCameraSettings;
    // One entry per button id WireControls has ever asked for. The key is a value, not
    // a pointer into the caller's id: nothing can dangle, and hashing a string_view
    // allocates nothing however the id was spelled at the call site.
    struct WiredButton
    {
        StringId      Id{0};
        // The element instance already wired for this id. Instance ids start at 1, so
        // 0 means "never wired".
        std::uint64_t WiredInstanceId = 0;
        // Filled by ResolveWiredButtons for the duration of one pass and nulled again
        // before the pass closes, so no pass can be handed a pointer another pass
        // resolved. Not owned.
        Button*       Resolved = nullptr;
    };
    // A flat vector rather than a hash map: the set is ~11 entries, so a linear scan
    // over contiguous 64-bit keys beats a bucket chase, and the vector's capacity
    // survives the first pass — steady state allocates nothing.
    std::vector<WiredButton> m_WiredButtons;
    // UIManager::GetTreeStructureGeneration() as of the last completed walk.
    std::uint64_t m_WiringStructureGeneration = 0;
    // True when the last walk resolved every registered id. Half of the skip gate;
    // see BeginWiringPass for why the generation alone is not enough.
    bool m_WiringAllResolved = false;
    bool m_WiringPassOpen = false;
};

} // namespace GameEngine
