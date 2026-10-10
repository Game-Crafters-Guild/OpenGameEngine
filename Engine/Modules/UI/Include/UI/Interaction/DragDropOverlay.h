#pragma once

#include <string>

#include "UI/Interaction/Payload.h"

namespace GameEngine
{
class UIElement;
class Label;

namespace UI::Interaction
{
class DragDropManager;

// Owns the UI-only overlay widgets used during drag/drop (ghost + tooltip).
// This keeps UIManager_Update.cpp slim and avoids payload-specific branching there.
class DragDropOverlay final
{
  public:
    void Update(UIElement* root, const DragDropManager& dnd, float mouseX, float mouseY);
    void Hide();

  private:
    void ResetIfDetached();
    void EnsureGhost(UIElement* root);
    void EnsureTooltip(UIElement* root);

    UIElement* m_Ghost = nullptr;                 // not owned (child of root)
    UIElement* m_GhostIcon = nullptr;             // not owned (child of ghost)
    Label* m_GhostLabel = nullptr;            // not owned (child of ghost)
    Label* m_GhostBadge = nullptr;            // not owned (child of ghost)
    std::string m_LastGhostText;
    DragGhostIconKind m_LastGhostIconKind = DragGhostIconKind::None;
    std::string m_LastGhostThumbnailEngineName;
    bool m_LastGhostVisible = false;
    int m_LastGhostX = 0;
    int m_LastGhostY = 0;

    UIElement* m_Tooltip = nullptr; // not owned (child of root)
    Label* m_TooltipLabel = nullptr;            // not owned (child of tooltip)
    std::string m_LastTooltipText;
    bool m_LastTooltipVisible = false;
    int m_LastTooltipX = 0;
    int m_LastTooltipY = 0;
};

} // namespace UI::Interaction
} // namespace GameEngine

