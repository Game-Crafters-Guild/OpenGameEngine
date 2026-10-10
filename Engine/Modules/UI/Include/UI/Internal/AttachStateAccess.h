#pragma once

#include "UI/UIElement.h"

namespace GameEngine
{

// The attach/detach settle's private window onto UIElement (UIManager_AttachEvents.cpp).
//
// Three members, and all three are invariants rather than data: which manager holds this
// element in its settle queue, which holds it in a walk snapshot, and the last attach
// state its subscribers were told. Written by the settle and by the enqueue sites only —
// a bit that disagrees with what was actually dispatched turns the mechanism from
// edge-triggered into silently lossy, so the writes stay behind this accessor instead of
// on the element's public surface.
//
// THE TWO OWNER SLOTS ARE INDEPENDENT ON PURPOSE. Queue membership and walk membership are
// different relationships held by possibly different managers at the same instant, so each
// carries its own manager identity; collapsing them into one slot is exactly the defect
// that let a dying element tombstone the wrong manager's vector.
struct UIAttachStateAccess
{
    static UIManager* QueueOwner(const UIElement& el) { return el.m_AttachQueueOwner; }
    static UIManager* WalkOwner(const UIElement& el) { return el.m_AttachWalkOwner; }

    static bool IsQueued(const UIElement& el) { return el.m_AttachQueueOwner != nullptr; }
    static bool IsWalking(const UIElement& el) { return el.m_AttachWalkOwner != nullptr; }

    static void MarkQueued(UIElement& el, UIManager* owner) { el.m_AttachQueueOwner = owner; }
    static void ClearQueued(UIElement& el) { el.m_AttachQueueOwner = nullptr; }

    static void MarkWalking(UIElement& el, UIManager* owner) { el.m_AttachWalkOwner = owner; }
    static void ClearWalking(UIElement& el) { el.m_AttachWalkOwner = nullptr; }

    static bool Doomed(const UIElement& el) { return el.m_Doomed; }
    static void SetDoomed(UIElement& el, bool doomed) { el.m_Doomed = doomed; }

    static bool AttachStateDispatched(const UIElement& el) { return el.m_AttachStateDispatched; }
    static void SetAttachStateDispatched(UIElement& el, bool attached)
    {
        el.m_AttachStateDispatched = attached;
    }
};

} // namespace GameEngine
