#pragma once

#include "UI/UIElement.h"
#include "UI/Controls/Button.h"
#include "Types/StringId.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::Editor
{

// Helper class to manage toolbar button drag-and-drop reordering.
// Supports any set of toolbar containers — pass ContainerConfig entries
// to Setup() and LoadButtonOrder().
class ToolbarDragDrop
{
  public:
    // Maps a CSS container class to its settings persistence key.
    struct ContainerConfig
    {
        std::string cssClass;    // CSS class identifying the container element
        std::string settingsKey; // Key used to persist button order in SettingsStore
        // Optional CSS class of a wider element that accepts drops for this
        // container — use it when the container hugs its buttons but the drop
        // area should extend across surrounding empty space. Empty means the
        // container is its own drop zone.
        std::string dropZoneCssClass;
    };

    ToolbarDragDrop();
    ~ToolbarDragDrop();

    // Register drag-and-drop handlers for buttons inside the given containers.
    // ghostRoot  - element that receives the ghost and insertion indicator (should
    //              be the global UI root so overlays appear above everything).
    // containers - CSS class + settings key pairs identifying the container elements.
    // searchRoot - (optional) element to search for containers by class. Defaults
    //              to ghostRoot when null. Pass a local subtree root when the
    //              containers are not yet reachable from ghostRoot (e.g. async UXML).
    void Setup(UIElement* ghostRoot, const std::vector<ContainerConfig>& containers,
               UIElement* searchRoot = nullptr);

    void SetAccentColor(uint32_t argb) { m_AccentColor = argb; }

    // Restore previously saved button order from SettingsStore.
    // searchRoot must reach the containers (same rule as Setup).
    void LoadButtonOrder(UIElement* searchRoot, const std::vector<ContainerConfig>& containers);

    // Clear persisted button order from SettingsStore and restore the original
    // (UXML-defined) order that the first LoadButtonOrder call on the current container
    // instance captured.
    void ResetButtonOrder(UIElement* searchRoot, const std::vector<ContainerConfig>& containers);

  private:
    void SaveButtonOrder(UIElement* container, const std::string& settingsKey);

    // A container resolved from its CSS class. The drag handlers read this through
    // `this` rather than capturing a copy of the caller's config, which is what keeps
    // a re-Setup free of allocation. ClassId and SettingsKey stay valid while Element
    // is null, so a container that mounts late still matches on a later pass.
    //
    // Element and DropZone are weak: a .uxml hot reload can destroy a container and
    // mount a replacement between two Setup passes. Until the next Setup resolves the
    // replacement, the destroyed one reads null and is skipped.
    struct ResolvedContainer
    {
        UIElement::WeakRef<> Element;
        // Hit-test target for drops. Equals Element unless ContainerConfig named a
        // wider zone — then this is that element, so empty bar space still accepts
        // a drop while buttons stay parented to Element.
        UIElement::WeakRef<> DropZone;
        StringId ClassId = 0;
        std::string SettingsKey;
    };
    std::vector<ResolvedContainer> m_Containers;

    // Each container's child IDs as LoadButtonOrder first saw them, before reordering,
    // keyed by settings key. Retaken when the key resolves to a different container
    // instance.
    struct DefaultOrder
    {
        uint64_t ContainerInstanceId = 0; // 0: never taken; instance ids start at 1
        std::vector<std::string> Ids;
    };
    std::unordered_map<std::string, DefaultOrder> m_DefaultOrders;

    // Drag state. The containers are weak for the same reason as ResolvedContainer: a
    // hot reload in the middle of a drag, or before the posted drop action runs, can
    // destroy either one.
    Button*     m_DraggedToolbarButton   = nullptr;
    UIElement::WeakRef<> m_DraggedButtonContainer;
    UIElement::WeakRef<> m_TargetButtonContainer;
    UIElement*  m_DragGhostElement       = nullptr;
    UIElement*  m_InsertionIndicator     = nullptr;
    int         m_CurrentDropIndex       = -1;
    bool        m_DragOrderChanged       = false;
    bool        m_DragActive             = false;
    float       m_DragStartX             = 0.0f;
    float       m_DragStartY             = 0.0f;
    uint32_t    m_AccentColor            = 0xFF3A8FFF;

    // Expires with this object. The drop action posted on mouse-up holds it weakly and
    // runs only while it is live.
    std::shared_ptr<bool> m_LifetimeToken = std::make_shared<bool>(true);
};

} // namespace GameEngine::Editor
