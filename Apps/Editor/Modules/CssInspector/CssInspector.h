#pragma once

#include <cstdint>
#include <string>

namespace GameEngine
{

class UIManager;
class UIElement;

// Hover box that shows the CSS of the element under the cursor.
// Toggle with Ctrl+I; call Update() each frame before UI render.
// Press C (no modifiers) while the inspector is enabled to copy the displayed element's selector + style to clipboard.
class CssInspector
{
public:
    CssInspector() = default;

    bool IsEnabled() const { return m_Enabled; }
    void SetEnabled(bool on) { m_Enabled = on; }
    void ToggleEnabled() { m_Enabled = !m_Enabled; }

    // Update overlay for this window: show/hide and position based on hovered element.
    // Call once per frame before UIManager::Render() for the window.
    void Update(UIManager* ui, int windowWidth, int windowHeight);

    // Mouse wheel: cycle through layers (0 = hovered, 1 = parent, ...). Call when inspector is enabled.
    void OnScrollWheel(float deltaY);

    // Element currently shown in the overlay (selected layer; null if none). Used for copy-on-C.
    UIElement* GetDisplayedElement(UIManager* ui) const;

    // Format selector + resolved style + C++ overrides + inline style for an element (same as tooltip).
    std::string GetSummaryForElement(const UIManager* ui, const UIElement* el) const;
    // Same as above but prefixed with full selector path (root > ... > el), for copy-on-C.
    std::string GetSummaryIncludingPathForElement(const UIManager* ui, const UIElement* el) const;

private:
    bool m_Enabled = false;
    int m_DisplayLayerIndex = 0;   // which layer is shown (0 = hovered element)
    uint64_t m_LastHoveredInstanceId = 0;  // reset display layer when hovered element changes
};

} // namespace GameEngine
