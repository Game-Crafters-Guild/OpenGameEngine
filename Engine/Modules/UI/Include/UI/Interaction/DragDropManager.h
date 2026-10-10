#pragma once

#include "UI/Interaction/DropTarget.h"

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::UI::Interaction
{
// Per-window drag/drop state machine:
// - owns the active payload + session context
// - resolves hovered drop target from the current hovered element chain
// - drives drop preview + commit/cancel
//
// Cross-window routing is done by an Editor-level router (outside UI module)
// which picks the active window/UIManager and feeds mouse+hover into this manager.
class DragDropManager final
{
  public:
    void BeginDrag(DragPayload payload, DragSessionContext ctx = {});
    void CancelDrag();
    bool IsDragging() const { return m_Dragging; }

    const DragPayload& GetPayload() const { return m_Payload; }
    void SetDisplayLabel(const std::string& label) { m_Payload.DisplayLabel = label; }
    const DragSessionContext& GetContext() const { return m_Ctx; }
    const DropHit& GetCurrentHit() const { return m_CurrentHit; }
    const DropFeedback& GetCurrentFeedback() const { return m_CurrentFeedback; }
    GameEngine::UIElement* GetCurrentTargetElement() const { return m_CurrentTargetEl; }
    int GetCurrentMods() const { return m_CurrentMods; }

    // Update hover target and previews. `hoveredLeaf` is the UIManager's current hovered element.
    void UpdateHover(GameEngine::UIElement* hoveredLeaf, float mouseX, float mouseY, int mods);
    void UpdateHover(GameEngine::UIElement* hoveredLeaf, float mouseX, float mouseY)
    {
        UpdateHover(hoveredLeaf, mouseX, mouseY, /*mods=*/0);
    }

    // Commit against the current hovered target (if allowed). Always ends the session.
    void CommitDrop(int mods);
    void CommitDrop() { CommitDrop(/*mods=*/0); }

    // Last pointer position passed to UpdateHover (absolute UI coordinates).
    float GetLastHoverMouseX() const { return m_LastHoverMouseX; }
    float GetLastHoverMouseY() const { return m_LastHoverMouseY; }

  private:
    void ClearPreview();

    bool m_Dragging = false;
    DragPayload m_Payload{};
    DragSessionContext m_Ctx{};

    IDropTarget* m_CurrentTarget = nullptr;          // not owned
    GameEngine::UIElement* m_CurrentTargetEl = nullptr; // not owned (same object as m_CurrentTarget, but typed)
    DropHit m_CurrentHit{};
    DropFeedback m_CurrentFeedback{};
    int m_CurrentMods = 0;

    float m_LastHoverMouseX = 0.0f;
    float m_LastHoverMouseY = 0.0f;
};
} // namespace GameEngine::UI::Interaction

