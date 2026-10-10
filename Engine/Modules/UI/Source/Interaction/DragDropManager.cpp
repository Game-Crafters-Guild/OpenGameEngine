#include "UI/Interaction/DragDropManager.h"

#include "UI/UIElement.h"

namespace GameEngine::UI::Interaction
{
void DragDropManager::BeginDrag(DragPayload payload, DragSessionContext ctx)
{
    CancelDrag();
    if (!payload.IsValid())
        return;
    m_Dragging = true;
    m_Payload = std::move(payload);
    m_Ctx = ctx;
}

void DragDropManager::CancelDrag()
{
    if (!m_Dragging)
        return;
    ClearPreview();
    m_Dragging = false;
    m_Payload = {};
    m_Ctx = {};
    // Note: m_Ctx.OnDragEnded is intentionally NOT fired here. CancelDrag is also used by the
    // cross-window drag router to transfer a session between windows (latch + per-window handoff),
    // which would fire it prematurely. The session owner fires OnDragEnded once at the true end.
}

void DragDropManager::ClearPreview()
{
    if (m_CurrentTarget)
    {
        DropPreviewState s{};
        s.Visible = false;
        m_CurrentTarget->SetDropPreview(s);
    }
    m_CurrentTarget = nullptr;
    m_CurrentTargetEl = nullptr;
    m_CurrentHit = {};
    m_CurrentFeedback = {};
    m_CurrentMods = 0;
}

void DragDropManager::UpdateHover(GameEngine::UIElement* hoveredLeaf, float mouseX, float mouseY, int mods)
{
    m_LastHoverMouseX = mouseX;
    m_LastHoverMouseY = mouseY;

    if (!m_Dragging)
        return;
    m_CurrentMods = mods;

    // Find the nearest ancestor that implements IDropTarget.
    UIElement* p = hoveredLeaf;
    IDropTarget* bestTarget = nullptr;
    UIElement* bestEl = nullptr;
    DropHit bestHit{};
    DropFeedback bestFeedback{};

    while (p)
    {
        // Only consider targets that accept this payload type.
        if (auto* dt = dynamic_cast<IDropTarget*>(p))
        {
            if (dt->AcceptsPayload(m_Payload.TypeId))
            {
                DropHit hit{};
                if (dt->HitTestDropTarget(mouseX, mouseY, hit))
                {
                    DropRequest req{};
                    req.payload = m_Payload;
                    req.hit = hit;
                    req.mods = mods;
                    DropFeedback fb = dt->CanDrop(req);
                    bestTarget = dt;
                    bestEl = p;
                    bestHit = hit;
                    bestFeedback = std::move(fb);
                    break; // nearest ancestor wins
                }
            }
        }
        p = p->GetParent();
    }

    const bool changed = (bestTarget != m_CurrentTarget) ||
                         (bestEl != m_CurrentTargetEl) ||
                         (bestHit.TargetId != m_CurrentHit.TargetId) ||
                         (bestHit.Location != m_CurrentHit.Location) ||
                         (bestFeedback.Allowed != m_CurrentFeedback.Allowed) ||
                         (bestFeedback.SuppressGhost != m_CurrentFeedback.SuppressGhost) ||
                         (bestFeedback.Reason != m_CurrentFeedback.Reason) ||
                         (bestFeedback.CssClass != m_CurrentFeedback.CssClass);

    // Even if the target didn't change, allow the active target to auto-scroll
    // while dragging (e.g. list/grid views near top/bottom edges).
    if (bestEl)
    {
        if (auto* autoScroll = dynamic_cast<IDragAutoScrollTarget*>(bestEl))
        {
            autoScroll->AutoScrollDuringDrag(mouseX, mouseY);
        }
    }

    if (!changed)
    {
        // Allow targets to "tick" hover behaviors while the hit stays stable
        // (e.g., TreeView auto-expand-on-hover during dragging).
        // Also tick for empty-space targets (e.g. scene view placement follows the pointer).
        if (m_CurrentTarget && m_CurrentFeedback.Allowed)
        {
            const bool tickItem =
                (m_CurrentHit.Location == DropLocation::OnItem && m_CurrentHit.TargetId != 0);
            const bool tickEmpty = (m_CurrentHit.Location == DropLocation::OnEmptySpace);
            if (tickItem || tickEmpty)
            {
                DropPreviewState s{};
                s.Visible = true;
                s.Allowed = true;
                s.Hit = m_CurrentHit;
                s.CssClass = m_CurrentFeedback.CssClass;
                m_CurrentTarget->SetDropPreview(s);
            }
        }
        return;
    }

    // Clear prior preview.
    if (m_CurrentTarget)
    {
        DropPreviewState s{};
        s.Visible = false;
        m_CurrentTarget->SetDropPreview(s);
    }

    m_CurrentTarget = bestTarget;
    m_CurrentTargetEl = bestEl;
    m_CurrentHit = bestHit;
    m_CurrentFeedback = bestFeedback;

    // Apply new preview.
    if (m_CurrentTarget)
    {
        DropPreviewState s{};
        s.Visible = true;
        s.Allowed = m_CurrentFeedback.Allowed;
        s.Hit = m_CurrentHit;
        s.CssClass = m_CurrentFeedback.CssClass;
        m_CurrentTarget->SetDropPreview(s);
    }
}

void DragDropManager::CommitDrop(int mods)
{
    if (!m_Dragging)
        return;
    m_CurrentMods = mods;

    // Snapshot the target + drop preview state, then clear our member
    // references BEFORE invoking PerformDrop. The drop callback often
    // mutates the underlying model (e.g. assigning a new clip to an
    // Animator), which can trigger an inspector rebuild that destroys
    // the very UIElement m_CurrentTargetEl points at. If we clear
    // m_CurrentTarget after the callback, the post-drop CancelDrag /
    // ClearPreview path dereferences a freed pointer (read AV at
    // 0xFFFFFFFFFFFFFFD7-style addresses). Clearing first means
    // ClearPreview becomes a no-op for the post-drop teardown.
    IDropTarget* target = m_CurrentTarget;
    const DropHit hit = m_CurrentHit;
    DragPayload payload = m_Payload; // snapshot before we clear state

    // Clear the visual preview on the target while it's still alive,
    // since after PerformDrop runs we have no guarantee it remains.
    if (target)
    {
        DropPreviewState s{};
        s.Visible = false;
        target->SetDropPreview(s);
    }

    m_CurrentTarget = nullptr;
    m_CurrentTargetEl = nullptr;
    m_CurrentHit = {};
    m_CurrentFeedback = {};
    m_CurrentMods = 0;

    // Tear down the drag session BEFORE running the drop callback so a
    // PerformDrop that triggers BeginDrag() (e.g. a chained drag-out)
    // sees a clean slate and can install its new session safely. Without
    // this, the new BeginDrag's CancelDrag would clobber the new
    // session's state because m_Dragging would still be true from this
    // outer drop. Also makes us safe against PerformDrop throwing —
    // there's no half-cleaned drag state to leak.
    m_Dragging = false;
    m_Payload = {};
    m_Ctx = {};

    if (target)
    {
        DropRequest req{};
        req.payload = payload;
        req.hit = hit;
        req.mods = mods;
        const DropFeedback fb = target->CanDrop(req);
        if (fb.Allowed)
        {
            // Note: target / targetEl may be destroyed by this call. We
            // must not touch them afterward.
            target->PerformDrop(req);
        }
    }
    // No CancelDrag here — state was already cleared above to handle the
    // re-entrant BeginDrag case AND the PerformDrop-throws case.
    // (OnDragEnded is fired by the session owner — the cross-window router — at the true end.)
}
} // namespace GameEngine::UI::Interaction

