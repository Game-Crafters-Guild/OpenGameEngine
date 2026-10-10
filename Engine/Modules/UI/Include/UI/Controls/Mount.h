#pragma once

#include "UI/UIElement.h"

namespace GameEngine {

class Mount;

// Process-wide registry of live Mount instances keyed by their current target.
// Step 0c of the primary-flip-redesign migration. Lets UIElement destruction
// notify any Mount across any UIManager that its target is going away, so
// the Mount can clear its raw target pointer before it dangles.
//
// Today only Mount writes to it (Register / Unregister) — consumption
// (OnElementDestroyed clearing matched Mounts) wires in step 2 when
// UIElement destruction routes through it.
//
// Thread-safety: UI update is single-threaded; no synchronization needed.
class MountRegistry {
public:
    static MountRegistry& Instance();

    // Called by Mount::SetTarget / SwapTargetForActivation / dtor.
    // Either argument may be nullptr.
    void OnTargetChanged(Mount* mount, UIElement* oldTarget, UIElement* newTarget);

    // Called when a UIElement is about to be destroyed. Walks matched
    // Mounts and clears their m_Target to nullptr. Does NOT fire
    // OnMountVisibilityChanged(false) — the target is mid-destruction
    // and downstream listeners have no safe way to deref it. Wired in
    // Step 2a (called from ~UIElement); no other callers today.
    void OnElementDestroyed(UIElement* element);

    // Called by `target` when its overlay-in-subtree bit changes.
    // Triggers RefreshOverlaySubtreeBitAndPropagate on every Mount whose
    // m_Target points at `target`, so the host tree's cull-relevant bit
    // reflects state changes that happen inside the mount target after
    // SetTarget already ran.
    void NotifyTargetOverlayBitChanged(UIElement* target);

    // Diagnostic: number of Mount entries currently registered. For tests.
    size_t Size() const;

    // Primitive generation and hit-testing assume each UIElement is mounted in
    // at most one place. Before assigning `target` to `keeper`, clears any
    // other live Mount that currently references the same target.
    void DetachTargetFromOtherMountsExcept(UIElement* target, Mount* keeper);

private:
    MountRegistry() = default;
    MountRegistry(const MountRegistry&) = delete;
    MountRegistry& operator=(const MountRegistry&) = delete;
};

// Mount is a lightweight host that displays a non-owned UIElement inside its own bounds.
// Ownership stays with the creator; Mount never deletes the target.
class Mount : public UIElement {
public:
    Mount() = default;

    UIElementKind Kind() const override { return UIElementKind::Mount; }

    ~Mount() override
    {
        if (m_Target)
        {
            // Stage 7 step 6.3b: clear the cascade DFS-parent edge before
            // the host goes away. Idempotent if already nullptr.
            m_Target->SetMountHostAsDfsParent(nullptr);
            MountRegistry::Instance().OnTargetChanged(this, m_Target, nullptr);
            m_Target->OnMountVisibilityChanged(false);
            // The target outlives its host here — it is not owned by the Mount — so it
            // has just become unreachable and that IS a detach.
            NoteMountReachabilityChanged(m_Target);
        }
    }

    void SetTarget(UIElement* el)
    {
        if (m_Target == el)
            return;

        if (el)
            MountRegistry::Instance().DetachTargetFromOtherMountsExcept(el, this);

        UIElement* oldTarget = m_Target;
        if (oldTarget)
        {
            // Stage 7 step 6.3b: drop the cascade DFS-parent edge from
            // the prior target first; the new target adopts it below.
            oldTarget->SetMountHostAsDfsParent(nullptr);
            oldTarget->OnMountVisibilityChanged(false);
        }

        MountRegistry::Instance().OnTargetChanged(this, oldTarget, el);
        m_Target = el;
        if (m_Target)
            m_Target->SetMountHostAsDfsParent(this);

        if (UIManager* owner = GetOwnerManager())
        {
            if (m_Target && m_Target->GetOwnerManager() != owner)
                m_Target->SetOwnerManager(owner);
            // Synchronous Yoga re-parent: detach old target's YGNode and
            // attach the new one. Without this the Mount's Yoga child
            // list lags one frame behind m_Target until the next Yoga
            // build pass — same bug class as deferred AddChild Yoga
            // sync.
            if (oldTarget)
                UIElementDetachYogaChild(this, oldTarget);
            if (m_Target)
                UIElementAttachYogaChild(this, m_Target);
            UIManagerNotifyTreeStructureChanged(owner);
        }

        MarkDirty(ChildrenDirty | LayoutDirty | VisualDirty);

        // Cascade-memoization Phase 2: the new target's ancestor chain now
        // routes through this Mount, so descendant selectors anchored on
        // the Mount's ancestors may match (or stop matching). Invalidate
        // the new target subtree's rule cache. The old target's cache
        // becomes irrelevant when it loses Mount visibility — leave it
        // alone (cheap to rebuild later if it's re-mounted).
        if (m_Target)
        {
            m_Target->OnMountVisibilityChanged(true);
            m_Target->InvalidateRuleCacheSubtree();
            m_Target->MarkDirtySubtree(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
        }
        // Hit-test cull bit reflects target's overlay-in-subtree status —
        // recompute now that m_Target changed.
        RefreshOverlaySubtreeBitAndPropagate();
        NoteMountReachabilityChanged(oldTarget);
        NoteMountReachabilityChanged(m_Target);
    }

    // Swap the mount target without triggering global tree-structure signals.
    // Used by DockspaceElement::PatchActiveTab to avoid a full dockspace rebuild
    // when only the active tab changes. The caller is responsible for ensuring
    // the owning UIManager picks up the subtree change through the normal
    // BuildYogaRecursive walk (which reads GetTarget() each frame).
    void SwapTargetForActivation(UIElement* el)
    {
        if (m_Target == el)
            return;

        if (el)
            MountRegistry::Instance().DetachTargetFromOtherMountsExcept(el, this);

        UIElement* oldTarget = m_Target;
        if (oldTarget)
        {
            oldTarget->SetMountHostAsDfsParent(nullptr);
            oldTarget->OnMountVisibilityChanged(false);
        }

        MountRegistry::Instance().OnTargetChanged(this, oldTarget, el);
        m_Target = el;
        if (m_Target)
            m_Target->SetMountHostAsDfsParent(this);

        if (UIManager* owner = GetOwnerManager())
        {
            if (m_Target && m_Target->GetOwnerManager() != owner)
                m_Target->SetOwnerManager(owner);
            if (oldTarget)
                UIElementDetachYogaChild(this, oldTarget);
            if (m_Target)
                UIElementAttachYogaChild(this, m_Target);
        }

        // ChildrenDirty is load-bearing: BuildYogaRecursive skips
        // InsertChildrenSortedByOrder unless `needInsert` is set (any of
        // hadChildrenDirty / isFreshNode / forceGlobalStyle /
        // anyChildStructureChange). On return-activation of an already-
        // mounted panel none of those fire on the Mount — without
        // ChildrenDirty the Yoga child list keeps pointing at the OLD
        // target and the new one has no Yoga parent (zero layout rect,
        // invisible to hit-test).
        MarkDirty(ChildrenDirty | LayoutDirty | VisualDirty);

        if (m_Target)
        {
            m_Target->OnMountVisibilityChanged(true);
            // Cascade-memoization Phase 2: same as SetTarget — Mount-target
            // ancestor context changed; invalidate the new target's cache.
            m_Target->InvalidateRuleCacheSubtree();
            m_Target->MarkDirtySubtree(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
        }
        RefreshOverlaySubtreeBitAndPropagate();
        NoteMountReachabilityChanged(oldTarget);
        NoteMountReachabilityChanged(m_Target);
    }

    UIElement* GetTarget() const { return m_Target; }
    UIElement* GetMountTarget() override { return m_Target; }

    void OnPostLayout() override
    {
        // Mount targets are portal-like and not part of the normal child list,
        // so they do not automatically receive owner-manager propagation when
        // this Mount is attached under a UIManager. Ensure the mounted subtree
        // has an owner before input routing or retained dirty tracking relies on it.
        if (UIManager* owner = GetOwnerManager())
        {
            if (m_Target && m_Target->GetOwnerManager() != owner)
            {
                m_Target->SetOwnerManager(owner);
                m_Target->MarkDirtySubtree(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
                UIManagerNotifyTreeStructureChanged(owner);
            }
        }
    }

private:
    friend class MountRegistry;

    // Attach/detach settle, enqueue site 3. A Mount is a portal: moving its target
    // re-routes that subtree's DFS-parent edge, so its root-reachability changes with no
    // AddChild and no owner change at all — which is exactly dock-tab activation
    // (DockspaceElement::PatchActiveTab -> SwapTargetForActivation). Neither of the other
    // two enqueue sites can observe it.
    //
    // Deliberately NOT called from SetTargetToNullDueToDestruction: that runs while the
    // TARGET is being destroyed, and destruction is not a detach.
    static void NoteMountReachabilityChanged(UIElement* target)
    {
        if (!target)
            return;
        if (UIManager* owner = target->GetOwnerManager())
            UI::Detail::EnqueueAttachSettle(owner, target);
    }

    // Called by MountRegistry::OnElementDestroyed when our target is about
    // to be destroyed. Nulls m_Target without re-entering the registry (the
    // entry is being erased by the registry itself) and without firing
    // OnMountVisibilityChanged (the target is already mid-destruction).
    void SetTargetToNullDueToDestruction()
    {
        m_Target = nullptr;
        // The destroyed target (or a descendant) may be cached as the
        // manager's hover/capture element. Hover recovery revalidates those
        // pointers only when the structure generation moves — an externally
        // owned target dying with no RemoveChild in its chain must bump it,
        // or the stale pointer survives until the next structural change.
        if (UIManager* owner = GetOwnerManager())
            UIManagerNotifyTreeStructureChanged(owner);
    }

    UIElement* m_Target = nullptr; // not owned
};

} // namespace GameEngine
