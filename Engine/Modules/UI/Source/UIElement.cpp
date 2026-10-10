#include "UI/UIElement.h"
#include "UIAttributeAccess.h"
#include "UI/UiContext.h"
#include "UI/UIManager.h"
#include "UI/UIManagerRef.h"
#include "UI/Controls/Mount.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/StyleProperties.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/Internal/AttachDetachInternal.h"
#include "UI/Interaction/Manipulator.h"

#include "Logger/Logger.h"

#include <set>
#include <typeinfo>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <functional>
#include <typeinfo>
#include <unordered_set>

using namespace GameEngine;


// Stage 5 Block C: custom-state string-overload helpers. Mirror the
// case-insensitive normalization used by CSSParser for :my-state names
// (lowercased ASCII before hashing).
namespace
{
StringId HashCustomStateName(std::string_view name)
{
    std::string lowered;
    lowered.reserve(name.size());
    for (char c : name)
    {
        lowered.push_back(static_cast<char>(
            (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c));
    }
    return HashStringId(lowered);
}
} // namespace

void UIElement::AddCustomState(StringId stateId)
{
    for (StringId existing : m_CustomStateIds)
        if (existing == stateId) return;
    m_CustomStateIds.push_back(stateId);
    // A custom-state flip changes selector matches like a class change. Route
    // the dirty mark through the owning manager's aggregate custom-state
    // analysis (MarkPseudoStateScope): it raises LayoutDirty when a
    // custom-state rule is layout-affecting — the correctness mirror of the
    // :focus-within generalization (#211), which the old blind
    // StyleDirty|VisualDirty relied on the signature fallback for — and fans
    // the mark out to descendants/siblings for combinator rules, which the old
    // self-only mark never reached. The forwarder still applies the
    // CustomStateRules narrowing (via MarkPseudoStateScope) and no-ops when the
    // stylesheet has no custom-state rules at all.
    UIManagerMarkCustomStateScope(m_OwnerManager, this);
}

void UIElement::RemoveCustomState(StringId stateId)
{
    for (size_t i = 0; i < m_CustomStateIds.size(); ++i)
    {
        if (m_CustomStateIds[i] == stateId)
        {
            m_CustomStateIds.erase(m_CustomStateIds.begin() + (ptrdiff_t)i);
            UIManagerMarkCustomStateScope(m_OwnerManager, this);
            return;
        }
    }
}

void UIElement::AddCustomState(std::string_view stateName)
{
    AddCustomState(HashCustomStateName(stateName));
}

void UIElement::RemoveCustomState(std::string_view stateName)
{
    RemoveCustomState(HashCustomStateName(stateName));
}

bool UIElement::HasCustomState(std::string_view stateName) const
{
    return HasCustomState(HashCustomStateName(stateName));
}

UIElement::UIElement()
    : m_InstanceId(s_NextInstanceId.fetch_add(1, std::memory_order_relaxed))
{
    WireOverridesDirtyCallback();
}

UIElement::UIElement(const std::string& id)
    : m_InstanceId(s_NextInstanceId.fetch_add(1, std::memory_order_relaxed)),
      m_Id(id),
      m_IdHash(id.empty() ? StringId{0} : HashStringId(id))
{
    WireOverridesDirtyCallback();
}

UIElement::~UIElement()
{
    // Expire every WeakRef before anything below runs a callback that might Get() one.
    m_WeakRefBlock.reset();

    // Post handles held by other threads: refuse new posts, and skip the ones already queued.
    if (m_PostLink)
    {
        m_PostLink->ElementAlive = false;
        m_PostLink->Route.Set(nullptr, nullptr);
    }

    // Module-owned-handler index: leave it BEFORE m_EventHandlers is destroyed,
    // or the index keeps a dangling element the next module unload would walk.
    // One predicted-not-taken branch for every element that never carried a
    // handler from a hot-swappable image — which is all of them in any host that
    // never loads a user module.
    if (m_OwnedHandlerCount)
    {
        m_OwnedHandlerCount = 0;
        UI::ForgetElementOwnsModuleHandler(this);
    }

    // Step 2a (additive): notify the global Mount registry so any Mount
    // currently targeting this element nulls its raw m_Target before the
    // pointer dangles. Constant-time hash-map probe; early-outs on
    // elements that were never targeted by a Mount.
    MountRegistry::Instance().OnElementDestroyed(this);

    // Leave the attach/detach settle queue. This is BOTH halves of a contract: the
    // queue holds raw pointers and must not outlive them, and destruction is
    // deliberately NOT a detach — an element removed with RemoveChild is destroyed
    // inside that call, so it leaves here before the settle ever judges it and its
    // subscribers hear nothing. Scripts observe destruction through IsAlive.
    // THE CONTRACT DETECTOR, and it is a detector rather than a dispatch on purpose.
    //
    // Reaching here with the bit still set means this element's subscribers were told
    // ATTACHED and are never going to be told DETACHED: it is being destroyed through a
    // path that has no decision point wired to it. Dispatching from here is exactly what
    // the design forbids — the derived part of the object is already gone — so this reads
    // the element's own bool, dispatches nothing, and says so once.
    //
    // Live in DebugFast, not Debug alone: a detector that only exists in a configuration
    // nobody runs never fires. It would have caught the three gaps found in round-5 review
    // (the LoadLayout trio, the reconcile's type-change path, a dying manager's surviving
    // Mount target) on the existing suite's first run.
#if !defined(NDEBUG) || defined(GE_DEV_DIAG)
    if (m_AttachStateDispatched)
    {
        // Deduplicated on the element's TYPE, the way the ABI's undispatched-subscription
        // report is: a pooled control tearing down every frame gets one line, while a
        // genuinely different leak elsewhere in the session still gets its own. A single
        // process-global flag would hide every leak after the first.
        static std::set<std::string> reported;
        if (reported.emplace(typeid(*this).name()).second)
        {
            Logger::Log::Warning(
                "UI: element id='{}' (instance {}) destroyed while its subscribers still "
                "believe it is ATTACHED — this destruction path has no attach/detach "
                "decision point, so the detach was never announced. Route it through one "
                "(UI::DispatchDestructionDetach where destruction is decided), or "
                "unmount/remove the element before destroying it. Reported once per element "
                "type.",
                m_Id, m_InstanceId);
        }
    }
#endif

    // BOTH slots: the settle queue and an in-flight walk snapshot can belong to DIFFERENT
    // managers, and a vector this element does not leave keeps a raw pointer that nothing
    // will ever tombstone.
    if (m_AttachQueueOwner)
        UI::Detail::ForgetAttachSettle(m_AttachQueueOwner, this);
    if (m_AttachWalkOwner && m_AttachWalkOwner != m_AttachQueueOwner)
        UI::Detail::ForgetAttachSettle(m_AttachWalkOwner, this);

    // Event-driven UI rewrite Stage 1: return any persistent render slots
    // to the owner's allocators. Today this is a no-op for every element
    // because Stage 1 doesn't allocate; Stage 2+ starts populating these
    // and the destructor must free them deterministically. The forwarder
    // (UIManagerFreeRenderSlots) is a no-op while the owner is being
    // destroyed. An element never outlives its manager with the owner still
    // set: ~UIManager detaches every survivor.
    if (m_OwnerManager &&
        (m_PrimitiveRangeCap > 0 || m_ClipSlotIdx != 0xFFFFu || m_DrawOrderIdx != 0xFFFFFFFFu))
    {
        UIManagerFreeRenderSlots(m_OwnerManager, this);
    }

    // Stage 5 Block A: tombstone any dirty-queue entries pointing at this
    // element + erase any m_FocusWithinChain entry. m_InQueueFlags now
    // includes the InFocusChain bit (set when RebuildFocusWithinChain
    // adds the element to m_FocusWithinChain), so this single check
    // covers both queue and focus-chain membership; flags == 0 skips the
    // call entirely.
    if (m_OwnerManager && m_InQueueFlags != 0)
    {
        UIManagerRemoveFromDirtyQueues(m_OwnerManager, this);
    }

    // Symmetric to the local-sheet registration in SetOwnerManager/AddStylesheet:
    // the manager iterates m_LocalSheetElements (raw pointers) every frame, so an
    // element destroyed while still owned (a parent's unique_ptr freeing children
    // without a prior detach) must remove itself or the next Update reads freed
    // memory. A no-op inside the forwarder while the owner is being destroyed.
    if (m_OwnerManager && !m_Stylesheets.empty())
    {
        UIManagerUnregisterLocalSheetElement(m_OwnerManager, this);
    }

    // Declared-transition registry (mirrored at cascade time): an element
    // destroyed while still owned must remove itself or the next
    // TransitionEngine::Advance iterates a freed pointer. The guard matches
    // the registration invariant — non-empty Transitions <=> registered.
    if (m_OwnerManager && !m_ResolvedStyle.Transitions.IsEmpty())
    {
        UIManagerUnregisterTransitioningElement(m_OwnerManager, this);
    }

    // instanceId map: a destroyed id must resolve to nullptr — the event
    // dispatch paths store ids instead of raw pointers for exactly this.
    if (m_OwnerManager)
    {
        UIManagerUnregisterElementInstanceId(m_OwnerManager, this);
    }
}

std::weak_ptr<UIElement> UIElement::WeakRefBlock()
{
    if (!m_WeakRefBlock)
        m_WeakRefBlock = std::shared_ptr<UIElement>(this, [](UIElement*) {});
    return m_WeakRefBlock;
}

void UIElement::RequestSubtreeStyleAssetPath(const std::string& assetPath, const std::string& sourceAlias)
{
    if (assetPath.empty())
        return;

    const SubtreeStyleAssetRequest request{assetPath, sourceAlias};
    // Deduplicate both path and source.
    if (std::find(m_RequestedSubtreeStyleAssetPaths.begin(), m_RequestedSubtreeStyleAssetPaths.end(), request) ==
        m_RequestedSubtreeStyleAssetPaths.end())
    {
        m_RequestedSubtreeStyleAssetPaths.push_back(request);
    }

    // If already owned, ask the manager to consume + attach immediately.
    if (m_OwnerManager)
    {
        m_OwnerManager->NotifyElementOwnerChanged(this);
    }
}

std::vector<UIElement::SubtreeStyleAssetRequest> UIElement::ConsumeRequestedSubtreeStyleAssetPaths()
{
    std::vector<SubtreeStyleAssetRequest> out;
    out.swap(m_RequestedSubtreeStyleAssetPaths);
    return out;
}

void UIElement::AddRequestedSubtreeStyleGuid(const GUID& guid)
{
    if (guid.IsNull())
        return;
    if (std::find(m_RequestedSubtreeStyleAssetGuids.begin(), m_RequestedSubtreeStyleAssetGuids.end(), guid) ==
        m_RequestedSubtreeStyleAssetGuids.end())
    {
        m_RequestedSubtreeStyleAssetGuids.push_back(guid);
    }
}

namespace
{
static void ClearLiveTexture(UIElement::LiveTextureRef& r)
{
    r.Handle = 0;
    r.Getter = {};
}
} // namespace

UIElement::StyleWriter& UIElement::StyleWriter::SetBackgroundResourceName(const std::string& name)
{
    if (!el)
        return *this;
    BackgroundImageSource src{};
    src.Kind = BackgroundImageSource::SourceKind::ResourceName;
    src.Value = name;
    el->Overrides().Set(Style::BackgroundImage, std::move(src));
    ClearLiveTexture(el->m_BackgroundImageTextureRef);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::SetBackgroundImageNone()
{
    if (!el)
        return *this;
    BackgroundImageSource src{};
    src.Kind = BackgroundImageSource::SourceKind::None;
    el->Overrides().Set(Style::BackgroundImage, std::move(src));
    ClearLiveTexture(el->m_BackgroundImageTextureRef);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::ResetBackgroundImage()
{
    if (!el)
        return *this;
    el->Overrides().Reset(Style::BackgroundImage);
    ClearLiveTexture(el->m_BackgroundImageTextureRef);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::SetBackgroundTint(uint32_t argb)
{
    if (!el)
        return *this;
    el->Overrides().Set(Style::BackgroundTint, argb);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::ResetBackgroundTint()
{
    if (!el)
        return *this;
    el->Overrides().Reset(Style::BackgroundTint);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::SetBackgroundRepeat(BackgroundRepeat repeat)
{
    if (!el)
        return *this;
    el->Overrides().Set(Style::BackgroundRepeatProp, repeat);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::ResetBackgroundRepeat()
{
    if (!el)
        return *this;
    el->Overrides().Reset(Style::BackgroundRepeatProp);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::SetBackgroundSizeAuto()
{
    if (!el)
        return *this;
    BackgroundSizeValue v{};
    v.Mode = BackgroundSizeMode::Auto;
    el->Overrides().Set(Style::BackgroundSize, v);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::SetBackgroundSizeCover()
{
    if (!el)
        return *this;
    BackgroundSizeValue v{};
    v.Mode = BackgroundSizeMode::Cover;
    el->Overrides().Set(Style::BackgroundSize, v);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::SetBackgroundSizeContain()
{
    if (!el)
        return *this;
    BackgroundSizeValue v{};
    v.Mode = BackgroundSizeMode::Contain;
    el->Overrides().Set(Style::BackgroundSize, v);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::SetBackgroundSizeExplicit(float x, bool xIsPercent, float y, bool yIsPercent)
{
    if (!el)
        return *this;
    BackgroundSizeValue v{};
    v.Mode = BackgroundSizeMode::Explicit;
    v.SizeX = x;
    v.SizeXIsPercent = xIsPercent;
    v.SizeY = y;
    v.SizeYIsPercent = yIsPercent;
    el->Overrides().Set(Style::BackgroundSize, v);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::ResetBackgroundSize()
{
    if (!el)
        return *this;
    el->Overrides().Reset(Style::BackgroundSize);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::SetBackgroundPosition(float x, bool xIsPercent, float y, bool yIsPercent)
{
    if (!el)
        return *this;
    BackgroundPositionValue v{};
    v.X = x;
    v.XIsPercent = xIsPercent;
    v.Y = y;
    v.YIsPercent = yIsPercent;
    el->Overrides().Set(Style::BackgroundPosition, v);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::SetBackgroundPositionPercent(float xPercent, float yPercent)
{
    if (!el)
        return *this;
    BackgroundPositionValue v{};
    v.X = xPercent;
    v.XIsPercent = true;
    v.Y = yPercent;
    v.YIsPercent = true;
    el->Overrides().Set(Style::BackgroundPosition, v);
    return *this;
}

UIElement::StyleWriter& UIElement::StyleWriter::ResetBackgroundPosition()
{
    if (!el)
        return *this;
    el->Overrides().Reset(Style::BackgroundPosition);
    return *this;
}

void UIElement::AddStylesheet(const StylesheetHandle& sheet)
{
	    if (!sheet)
	        return;
	    // Re-adding a present sheet is a position-preserving no-op — cascade
	    // ties at equal specificity break on sheet order (see the manager-side
	    // AddStylesheet note).
	    auto it = std::find_if(m_Stylesheets.begin(), m_Stylesheets.end(),
	                           [&](const StylesheetHandle& h) { return h.get() == sheet.get(); });
	    if (it != m_Stylesheets.end())
	        return;

	    m_Stylesheets.push_back(sheet);
	    MarkDirtySubtree(StyleDirty | LayoutDirty | VisualDirty);
	    // Subtree's effective sheet sets now include this new sheet — zero
	    // the persistent sheet-set IDs so next cascade re-interns. Also
	    // reaches Mount targets (MarkDirtySubtree does not).
	    if (m_OwnerManager)
	        m_OwnerManager->InvalidateSheetSetSubtree(this);
}

void UIElement::RemoveStylesheet(const StylesheetHandle& sheet)
{
	    if (!sheet)
	        return;
	    auto it = std::remove_if(m_Stylesheets.begin(), m_Stylesheets.end(),
	                             [&](const StylesheetHandle& h) { return h.get() == sheet.get(); });
	    if (it != m_Stylesheets.end())
	    {
	        m_Stylesheets.erase(it, m_Stylesheets.end());
	        MarkDirtySubtree(StyleDirty | LayoutDirty | VisualDirty);
	        if (m_OwnerManager)
	            m_OwnerManager->InvalidateSheetSetSubtree(this);
	    }
}

void UIElement::ReplaceStylesheetBlock(const std::vector<const Stylesheet*>& oldBlock,
                                      const std::vector<StylesheetHandle>& newBlock)
{
    // Stage allocations before releasing any sheet referenced by the live caches.
    auto replacement = m_Stylesheets;

    // Determine insertion point: the earliest index occupied by any old stylesheet.
    std::unordered_set<const Stylesheet*> oldSet;
    oldSet.reserve(oldBlock.size());
    for (const Stylesheet* s : oldBlock)
    {
        if (s)
            oldSet.insert(s);
    }

    size_t insertAt = replacement.size();
    if (!oldSet.empty())
    {
        for (size_t i = 0; i < replacement.size(); ++i)
        {
            const Stylesheet* ptr = replacement[i] ? replacement[i].get() : nullptr;
            if (ptr && oldSet.find(ptr) != oldSet.end())
            {
                insertAt = std::min(insertAt, i);
            }
        }
    }

    // Remove old block entries.
    if (!oldSet.empty())
    {
        auto it = std::remove_if(replacement.begin(), replacement.end(),
                                 [&](const StylesheetHandle& h)
                                 {
                                     const Stylesheet* ptr = h ? h.get() : nullptr;
                                     return ptr && oldSet.find(ptr) != oldSet.end();
                                 });
        replacement.erase(it, replacement.end());
    }

    if (insertAt > replacement.size())
        insertAt = replacement.size();

    // Prepare insertion list: dedupe within the new block AND against
    // survivors outside the removed block (shared @imports whose entry is
    // owned by another attached style would otherwise duplicate at a
    // shifted cascade position — C-10; see the manager-side variant).
    std::unordered_set<const Stylesheet*> surviving;
    surviving.reserve(replacement.size());
    for (const auto& h : replacement)
    {
        if (h)
            surviving.insert(h.get());
    }

    std::vector<StylesheetHandle> toInsert;
    toInsert.reserve(newBlock.size());
    std::unordered_set<const Stylesheet*> seenNew;
    seenNew.reserve(newBlock.size());
    for (const auto& h : newBlock)
    {
        if (!h)
            continue;
        const Stylesheet* ptr = h.get();
        if (!ptr)
            continue;
        if (surviving.find(ptr) != surviving.end())
            continue;
        if (!seenNew.insert(ptr).second)
            continue;
        toInsert.push_back(h);
    }

    if (!toInsert.empty())
    {
        replacement.insert(replacement.begin() + (ptrdiff_t)insertAt, toInsert.begin(), toInsert.end());
    }

    m_Stylesheets.swap(replacement);
    MarkDirtySubtree(StyleDirty | LayoutDirty | VisualDirty);
    if (m_OwnerManager)
        m_OwnerManager->InvalidateSheetSetSubtree(this);
}

void UIElement::RequestRelayout() const
{
    // If this element is attached to a UIManager, request a relayout on that
    // specific instance (multi-window safe). Detached elements have no owner
    // and therefore cannot trigger a relayout.
    if (m_OwnerManager)
    {
        m_OwnerManager->RequestRelayout();
        m_OwnerManager->NotifyRelayoutRequested(this);
    }
}


void UIElement::RefreshOverlaySubtreeBitAndPropagate()
{
    bool now = (m_OverlayLayer != OverlayLayer::None);
    if (!now)
    {
        for (auto& ch : m_Children)
        {
            if (ch && ch->m_HasOverlayInSubtree)
            {
                now = true;
                break;
            }
        }
    }
    // Mount targets count as part of "subtree" for cull purposes — Mount's
    // own children list is empty but its target may host overlays whose
    // hit-test escapes the host's clipped ancestors. GetMountTarget() is
    // virtual and returns nullptr on plain UIElement; only Mount overrides.
    if (!now)
    {
        if (UIElement* tgt = GetMountTarget())
            now = tgt->m_HasOverlayInSubtree;
    }
    if (m_HasOverlayInSubtree == now)
        return;
    m_HasOverlayInSubtree = now;
    if (m_Parent)
        m_Parent->RefreshOverlaySubtreeBitAndPropagate();
    // Any Mount that has THIS element as its target needs to see the bit
    // change too — its own bit is computed from this target's bit, and
    // the host tree above the Mount must reflect the new state.
    MountRegistry::Instance().NotifyTargetOverlayBitChanged(this);
}

namespace
{
// One line per refusal, not per element: a handler that tries this usually tries it for a
// whole list, and the useful signal is that it happened at all plus which edit it was.
void WarnOnDoomedTreeEdit(const char* op)
{
    Logger::Log::Warning(
        "UI: {} refused inside a destruction-detach — the subtree is already condemned and "
        "is freed as soon as the dispatch returns, so adopting into it or rescuing out of it "
        "would dangle. Do the work from a PostAction if it must outlive the teardown.",
        op);
}
} // namespace

void UIElement::AddChild(std::unique_ptr<UIElement> child) {
    if (!child) return;
    if (UI::Detail::IsDoomed(this) || UI::Detail::IsDoomed(child.get()))
    {
        // A handler reacting to a destruction-detach tried to adopt into the doomed
        // subtree, or to re-home one of its elements. Both are refused: the memory is
        // released the moment the dispatch returns, so either would leave a dangling
        // parent or a dangling child. `child` is destroyed here with the rest.
        WarnOnDoomedTreeEdit("AddChild");
        return;
    }
    UIElement* raw = child.get();
    child->m_Parent = this;
    // Propagate owning UIManager from parent to newly added child subtree
    if (m_OwnerManager) child->SetOwnerManager(m_OwnerManager);
    // A newly attached subtree must have its styles/layout computed at least once.
    child->MarkDirtySubtree(StyleDirty | LayoutDirty | VisualDirty | ChildrenDirty);
    // Cascade-memoization Phase 2: parent/ancestor context changed for the
    // newly attached subtree — descendant selectors (`.foo .bar`) targeting
    // the new ancestors may now match (or stop matching). Invalidate the
    // child subtree's rule cache.
    child->InvalidateRuleCacheSubtree();
    m_Children.emplace_back(std::move(child));
    MarkDirty(ChildrenDirty | LayoutDirty | VisualDirty);
    RestyleAfterChildListChange(m_Children.size() - 1, /*inserted=*/true, typeid(*raw));
    if (raw->m_HasOverlayInSubtree && !m_HasOverlayInSubtree)
        RefreshOverlaySubtreeBitAndPropagate();
    if (m_OwnerManager)
    {
        // Synchronous Yoga child attach. Without this, Yoga child-list
        // sync is deferred until the next BuildYogaRecursive walk, which
        // produces transient `yogaParent mismatch` warnings during
        // visibility-flip transitions (scrollbars, hidden classes,
        // mid-frame ghost insertion).
        UIElementAttachYogaChild(this, raw);
        UIManagerNotifyTreeStructureChanged(m_OwnerManager);
        // Attach/detach settle, enqueue site 2: the child's OWNER may not have changed —
        // an already-owned but unreachable subtree, an inactive dock tab's panel — while
        // its reachability just did. SetOwnerManager's enqueue sees nothing in that case,
        // so the link itself has to report it.
        UI::Detail::EnqueueAttachSettle(m_OwnerManager, raw);
    }
}

void UIElement::InsertChild(size_t index, std::unique_ptr<UIElement> child)
{
    if (!child)
        return;
    if (index > m_Children.size())
        return;
    if (UI::Detail::IsDoomed(this) || UI::Detail::IsDoomed(child.get()))
    {
        WarnOnDoomedTreeEdit("InsertChild");
        return;
    }
    UIElement* raw = child.get();
    child->m_Parent = this;
    if (m_OwnerManager)
        child->SetOwnerManager(m_OwnerManager);
    child->MarkDirtySubtree(StyleDirty | LayoutDirty | VisualDirty | ChildrenDirty);
    // Cascade-memoization Phase 2: same as AddChild — new ancestor context.
    child->InvalidateRuleCacheSubtree();
    m_Children.insert(m_Children.begin() + static_cast<ptrdiff_t>(index), std::move(child));
    MarkDirty(ChildrenDirty | LayoutDirty | VisualDirty);
    if (raw->m_HasOverlayInSubtree && !m_HasOverlayInSubtree)
        RefreshOverlaySubtreeBitAndPropagate();
    RestyleAfterChildListChange(index, /*inserted=*/true, typeid(*raw));
    if (m_OwnerManager)
    {
        // Synchronous Yoga child attach (see AddChild). The CSS `order`
        // resort happens in InsertChildrenSortedByOrder after cascade.
        UIElementAttachYogaChild(this, raw);
        UIManagerNotifyTreeStructureChanged(m_OwnerManager);
        // Attach/detach settle, enqueue site 2 (see AddChild).
        UI::Detail::EnqueueAttachSettle(m_OwnerManager, raw);
    }
}

// Single shared dispatch-depth counter (see the header note: header-inline
// definitions duplicated this per module and split the guard's view of it).
namespace
{
unsigned s_EventDispatchDepth = 0u;
}

void UIElement::SetInEventDispatch(bool v)
{
    if (v)
    {
        ++s_EventDispatchDepth;
        return;
    }

    if (s_EventDispatchDepth == 0u)
    {
#if defined(_DEBUG)
        assert(false && "UIElement::SetInEventDispatch(false) underflow");
#endif
        return;
    }
    --s_EventDispatchDepth;
}

bool UIElement::IsInEventDispatch()
{
    return s_EventDispatchDepth > 0u;
}

bool UIElement::AddManipulator(const std::shared_ptr<Manipulator>& manipulator)
{
    if (!manipulator)
        return false;

    // One instance, one element. A manipulator records its subscriptions on itself with no
    // element attached to the record, and handler keys are minted per element — so a second
    // attach mixes two elements' tokens into one list, and detaching from either unregisters
    // whatever the other element happens to hold under those keys.
    if (manipulator->m_Attached)
    {
        Logger::Log::Warning(
            "UI: AddManipulator refused on element '{}' — this {} is already attached to an "
            "element (or its element was destroyed). One instance serves one element: its "
            "subscription records name no element, so removing it here would unregister the "
            "other element's handlers. Build a second instance for this element.",
            GetId(), typeid(*manipulator).name());
        return false;
    }

    if (!m_Manipulators)
    {
        m_Manipulators = std::make_unique<std::vector<std::weak_ptr<Manipulator>>>();
    }
    else
    {
        // Prune while we are walking anyway: entries expire silently when a manipulator is
        // removed or revoked, and nothing else ever visits this list. Without this, an element
        // that cycles manipulators grows one dead entry per cycle forever.
        auto& entries = *m_Manipulators;
        for (auto it = entries.begin(); it != entries.end();)
        {
            const std::shared_ptr<Manipulator> existing = it->lock();
            if (!existing)
            {
                it = entries.erase(it);
                continue;
            }
            if (typeid(*existing) == typeid(*manipulator))
            {
                Logger::Log::Warning(
                    "UI: AddManipulator refused on element '{}' — a {} is already attached. The "
                    "second one can never fire (the first claims the gesture and the dispatch "
                    "stops there). Remove the existing one first, or attach this instance to a "
                    "different element.",
                    GetId(), typeid(*manipulator).name());
                return false;
            }
            ++it;
        }
    }

    m_Manipulators->push_back(manipulator);
    Manipulator::Attach(manipulator, *this);
    return true;
}

bool UIElement::RemoveManipulator(const std::shared_ptr<Manipulator>& manipulator)
{
    if (!manipulator || !m_Manipulators)
        return false;

    bool found = false;
    auto& entries = *m_Manipulators;
    for (auto it = entries.begin(); it != entries.end();)
    {
        const std::shared_ptr<Manipulator> existing = it->lock();
        if (!existing)
        {
            it = entries.erase(it);
            continue;
        }
        if (existing == manipulator)
        {
            it = entries.erase(it);
            found = true;
            continue;
        }
        ++it;
    }

    if (!found)
        return false;

    // Erased first, deliberately: the teardown destroys handler callables, whose captures are
    // caller code free to add or remove manipulators on this element. No iterator into
    // `entries` may be alive across it.
    manipulator->DetachFromTarget(*this);
    return true;
}

void UIElement::RemoveChild(UIElement* child) {
    if (!child) return;
    if (UIElement::IsInEventDispatch()) {
        // Defer removal to a safe point after dispatch to avoid iterator invalidation/UAF.
        // The deferred lambda calls RemoveChildImpl directly — not RemoveChild — to avoid
        // infinite re-queuing if the dispatcher processes its queue while still inside
        // event dispatch (which would re-trigger the IsInEventDispatch() branch above
        // and post yet another deferred action, growing the queue without bound).
        this->PostSafeAction([this, child]() { this->RemoveChildImpl(child); });
        return;
    }
    RemoveChildImpl(child);
}

void UIElement::RemoveChildImpl(UIElement* child) {
    if (!child) return;

    // ANNOUNCE FIRST, AND HOLD NOTHING ACROSS THE DISPATCH.
    //
    // RemoveChild destroys the child, so this is where destruction is decided — the last
    // moment the subtree is fully alive, still owned and still resolvable. But a detach
    // handler is allowed to add a child to THIS parent (the doomed set is the subtree
    // below, not the live parent), and an add reallocates m_Children. An iterator taken
    // before the dispatch would dangle across it, so membership is checked first and the
    // iterator is acquired only afterwards, by pointer identity — which survives the
    // reallocation that an index or an iterator would not.
    //
    // Unlinking before dispatching would avoid the reallocation too, and is rejected: the
    // element would then be announced while already off the tree, so its parent chain, its
    // reachability and its resolvability would all be lies at exactly the moment the
    // handler asks about them. Keeping the tree whole through the dispatch is the property
    // this timing exists to provide.
    const bool isOurChild =
        std::find_if(m_Children.begin(), m_Children.end(),
                     [&](const std::unique_ptr<UIElement>& ptr) { return ptr.get() == child; }) !=
        m_Children.end();
    if (isOurChild)
        UI::DispatchDestructionDetach(child);

    auto it = std::find_if(m_Children.begin(), m_Children.end(), [&](const std::unique_ptr<UIElement>& ptr){ return ptr.get() == child; });
    if (it != m_Children.end()) {
	        // If this subtree currently contains the hovered element, clear hover
	        // state on the owning UIManager before we detach it to avoid stale
	        // pointers during subsequent style recomputation.
	        if (m_OwnerManager)
	            m_OwnerManager->ClearHoverForSubtree(it->get());
        (*it)->m_Parent = nullptr;
        // Clear owner on the removed subtree to avoid dangling UIManager pointer
        if (m_OwnerManager) (*it)->SetOwnerManager(nullptr);
        const size_t removedIndex = static_cast<size_t>(it - m_Children.begin());
        const UIElement& removed = **it;
        const std::type_info& removedType = typeid(removed);

        // Step 2b: explicitly detach the child's Yoga node from this
        // element's Yoga tree before the unique_ptr destructor runs.
        // Without this, the orphaned Yoga child stays attached to the
        // parent until the next GC reaps it — and any cascade / solve
        // pass that runs in between iterates the dead child.
        // Idempotent against the existing GC reap.
        UIElementDetachYogaChild(this, it->get());
        const bool removedSubtreeHadOverlay = it->get() && it->get()->m_HasOverlayInSubtree;
        m_Children.erase(it);
        if (removedSubtreeHadOverlay)
            RefreshOverlaySubtreeBitAndPropagate();
        RestyleAfterChildListChange(removedIndex, /*inserted=*/false, removedType);

        MarkDirty(ChildrenDirty | LayoutDirty | VisualDirty);
        if (m_OwnerManager)
            UIManagerNotifyTreeStructureChanged(m_OwnerManager);
    }
}

void UIElement::RestyleForStructuralChange()
{
    MarkDirtySubtree(StyleDirty | VisualDirty);
    InvalidateRuleCacheSubtree();
}

void UIElement::RestyleAfterChildListChange(size_t index, bool inserted, const std::type_info& changedType)
{
    if (!m_OwnerManager || m_OwnerManager->IsBeingDestroyed())
        return;
    const UIManager& owner = *m_OwnerManager;
    const size_t count = m_Children.size();

    // :empty flips on the first child in and the last child out.
    if (owner.UsesEmptyPseudo() && count == (inserted ? 1u : 0u))
    {
        MarkDirty(StyleDirty | VisualDirty);
        InvalidateRuleCacheSubtree();
    }

    const uint32_t pseudos = owner.GetStructuralPseudoMask();
    const bool combinators = owner.UsesSiblingCombinators();
    if (pseudos == 0 && !combinators)
        return;
    const auto uses = [pseudos](PseudoClass::Kind kind) { return (pseudos & PseudoKindBit(kind)) != 0; };
    const auto isChangedType = [&](size_t i) {
        const UIElement* sibling = m_Children[i].get();
        return sibling && typeid(*sibling) == changedType;
    };
    const auto restyle = [&](size_t i) {
        if (m_Children[i])
            m_Children[i]->RestyleForStructuralChange();
    };

    // Siblings in [0, index) keep their position from the start and move by
    // one from the end; siblings in [following, count) do the opposite and
    // gained or lost a preceding sibling.
    const size_t following = inserted ? index + 1 : index;
    const size_t others = inserted ? count - 1 : count;

    const bool allFollowing = combinators || uses(PseudoClass::Kind::NthChild);
    const bool allPreceding = uses(PseudoClass::Kind::NthLastChild);
    const bool typeFollowing = !allFollowing && uses(PseudoClass::Kind::NthOfType);
    const bool typePreceding = !allPreceding && uses(PseudoClass::Kind::NthLastOfType);
    if (allFollowing || typeFollowing)
    {
        for (size_t i = following; i < count; ++i)
            if (allFollowing || isChangedType(i))
                restyle(i);
    }
    if (allPreceding || typePreceding)
    {
        for (size_t i = 0; i < index; ++i)
            if (allPreceding || isChangedType(i))
                restyle(i);
    }

    // Boundary pseudos change for one sibling at most: the one that became,
    // or stopped being, first, last or only.
    if (!allFollowing && uses(PseudoClass::Kind::FirstChild) && index == 0 && following < count)
        restyle(following);
    if (!allPreceding && uses(PseudoClass::Kind::LastChild) && following == count && index > 0)
        restyle(index - 1);
    if (uses(PseudoClass::Kind::OnlyChild) && others == 1)
        restyle(inserted && index == 0 ? 1 : 0);

    const bool firstOfType = !allFollowing && !typeFollowing && uses(PseudoClass::Kind::FirstOfType);
    const bool lastOfType = !allPreceding && !typePreceding && uses(PseudoClass::Kind::LastOfType);
    const bool onlyOfType = uses(PseudoClass::Kind::OnlyOfType);
    if (!firstOfType && !lastOfType && !onlyOfType)
        return;
    size_t firstFollowingOfType = count;
    size_t lastPrecedingOfType = count;
    size_t othersOfType = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if ((inserted && i == index) || !isChangedType(i))
            continue;
        ++othersOfType;
        if (i < index)
            lastPrecedingOfType = i;
        else if (firstFollowingOfType == count)
            firstFollowingOfType = i;
    }
    if (firstOfType && lastPrecedingOfType == count && firstFollowingOfType != count)
        restyle(firstFollowingOfType);
    if (lastOfType && firstFollowingOfType == count && lastPrecedingOfType != count)
        restyle(lastPrecedingOfType);
    if (onlyOfType && othersOfType == 1)
        restyle(lastPrecedingOfType != count ? lastPrecedingOfType : firstFollowingOfType);
}

void UIElement::RemoveAllChildren() {
    // Snapshot first — RemoveChild defers during event dispatch so the live
    // m_Children list does not shrink synchronously. Iterating the live
    // container with `while (!empty()) RemoveChild(front)` would re-process
    // the same head every iteration and queue unbounded deferred removals.
    // Walking a snapshot guarantees each child is requested for removal
    // exactly once.
    std::vector<UIElement*> snapshot;
    snapshot.reserve(m_Children.size());
    for (auto& c : m_Children)
        if (c) snapshot.push_back(c.get());
    for (UIElement* child : snapshot)
        RemoveChild(child);
}

std::unique_ptr<UIElement> UIElement::TakeChild(UIElement* child)
{
    if (!child)
        return nullptr;
    if (UI::Detail::IsDoomed(child))
    {
        // The other direction: a handler trying to RESCUE a condemned element by taking
        // ownership of it. Refused for the same reason — the caller that condemned this
        // subtree destroys it as soon as the dispatch returns, and it does not consult
        // anyone. Resurrection from a teardown notification is the recursion-class trap
        // this whole timing exists to avoid.
        WarnOnDoomedTreeEdit("TakeChild");
        return nullptr;
    }
    if (UIElement::IsInEventDispatch())
    {
        // Cannot safely mutate child lists during dispatch.
        return nullptr;
    }

    auto it = std::find_if(m_Children.begin(), m_Children.end(),
                           [&](const std::unique_ptr<UIElement>& ptr) { return ptr.get() == child; });
    if (it == m_Children.end())
    {
        return nullptr;
    }

    // Clear hover state for safety (mirrors RemoveChild).
    if (m_OwnerManager)
        m_OwnerManager->ClearHoverForSubtree(it->get());

    std::unique_ptr<UIElement> out = std::move(*it);
    // Step 2b followup: detach the Yoga node before clearing UIElement
    // parent. Symmetric with RemoveChild — the moved subtree is alive
    // but no longer this parent's child, so its YGNode shouldn't stay
    // attached to this parent's Yoga node. The new parent's AddChild
    // will reinsert when the caller reparents.
    UIElementDetachYogaChild(this, out.get());
    out->m_Parent = nullptr;
    if (m_OwnerManager)
        out->SetOwnerManager(nullptr);

    const bool removedSubtreeHadOverlay = out && out->m_HasOverlayInSubtree;
    const size_t removedIndex = static_cast<size_t>(it - m_Children.begin());
    m_Children.erase(it);
    if (removedSubtreeHadOverlay)
        RefreshOverlaySubtreeBitAndPropagate();
    const UIElement& taken = *out;
    RestyleAfterChildListChange(removedIndex, /*inserted=*/false, typeid(taken));
    MarkDirty(ChildrenDirty | LayoutDirty | VisualDirty);
    if (m_OwnerManager)
        UIManagerNotifyTreeStructureChanged(m_OwnerManager);
    return out;
}

void UIElement::RoutePostsTo(UIManager* owner)
{
    std::shared_ptr<UI::UiDispatcher> dispatcher = owner ? UIManagerSharedDispatcher(*owner) : nullptr;
    if (m_PostLink)
        m_PostLink->Route.Set(dispatcher, owner);
    m_PostTarget.Set(std::move(dispatcher), owner);
}

UI::UiPostHandle UIElement::GetPostHandle()
{
    if (!m_PostLink)
    {
        m_PostLink = std::make_shared<UI::UiPostLink>();
        // Copied from the route rather than rebuilt from m_OwnerManager, which dangles on an
        // element that outlived its manager; the route names that manager's closed dispatcher.
        UI::UiPostTarget::Route route = m_PostTarget.Load();
        m_PostLink->Route.Set(std::move(route.Dispatcher), route.Owner);
    }
    return UI::UiPostHandle(m_PostLink);
}

namespace
{

// The dispatcher of the UIManager whose update is running on this thread, if any. Only the UI
// thread ever has one, so a non-null result also means the caller is on the UI thread.
UI::IUiDispatcher* CurrentUiDispatcher()
{
    const UI::UiContext* ctx = UI::GetTlsUiContext();
    return ctx ? ctx->Dispatcher : nullptr;
}

// Posts through an element's route when no UI context is current. A closed dispatcher (its
// manager is gone) refuses the action.
//
// An element with no owner has no route, and the action is dropped rather than run inline:
// PostAction defers work to a safe point, and running it here instead invites unbounded
// recursion and re-entrancy (virtualization retry loops), worst during shutdown. The caller is
// told through the return value, so a latch it set on the action running can be released.
bool PostThroughRoute(const UI::UiPostTarget::Route& route, std::function<void()> action)
{
    if (route.Dispatcher)
        return route.Dispatcher->Post(std::move(action));

    static std::atomic<bool> s_PostActionLoggedDropOnce{false};
    if (!s_PostActionLoggedDropOnce.exchange(true))
        Logger::Log::Warning("UI: PostAction dropped: the element has no owning UIManager.");
    return false;
}

} // namespace

bool UIElement::PostAction(std::function<void()> action)
{
    // The UI thread inside an update posts to the running manager without touching the route.
    if (UI::IUiDispatcher* current = CurrentUiDispatcher())
        return current->Post(std::move(action));
    return PostThroughRoute(m_PostTarget.Load(), std::move(action));
}

bool UIElement::PostSafeAction(std::function<void()> action)
{
    // One route snapshot names both the dispatcher and the manager that drains it. Off the UI
    // thread the liveness check therefore runs against the manager draining the action, which
    // is alive whenever the check runs: a destroyed manager's dispatcher refuses the post.
    //
    // On the UI thread the action joins the queue of whichever manager is updating, and
    // route.Owner (m_OwnerManager) may be another one, destroyed before that drain. The guard
    // holds it as a UIManagerRef, built here on the UI thread, so it reads a destroyed owner
    // as gone, including when a successor occupies its address.
    const UI::UiPostTarget::Route route = m_PostTarget.Load();
    const uint64_t id = m_InstanceId;
    if (UI::IUiDispatcher* current = CurrentUiDispatcher())
    {
        return current->Post([action = std::move(action), owner = UIManagerRef(route.Owner), id]() {
            UIManager* manager = owner.Get();
            if (manager && manager->FindElementByInstanceId(id))
                action();
        });
    }
    return PostThroughRoute(route, [action = std::move(action), owner = route.Owner, id]() {
        if (owner && owner->FindElementByInstanceId(id))
            action();
    });
}

UIElement* UIElement::FindById(std::string_view id) {
    // Treat id and name as aliases (name is folded into m_Id during parsing).
    if (m_Id == id) return this;
    for (auto& c : m_Children) {
        if (auto* f = c->FindById(id)) return f;
    }
    // Include portal-like Mount targets in lookup so ids inside mounted panel subtrees
    // are discoverable by callers (automation, editor integration, debugging tools).
    if (auto* m = dynamic_cast<Mount*>(this))
    {
        if (UIElement* tgt = m->GetTarget())
        {
            if (UIElement* f = tgt->FindById(id))
                return f;
        }
    }
    return nullptr;
}

namespace
{
static void NormalizeAttrNameInPlace(std::string& name)
{
    for (char& c : name)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
}
} // namespace

void UIAttributeAccess::SetAuthoredAttribute(UIElement& el, std::string_view name, std::string_view value, bool markDirty)
{
    if (name.empty())
        return;
    auto it = el.m_Attributes.find(name);
    if (it != el.m_Attributes.end() && it->second == value)
        return;
    std::string key(name);
    NormalizeAttrNameInPlace(key);
    el.m_Attributes[std::move(key)] = std::string(value);
    // P4 (C-10): [attr] selectors match against these values, and the
    // structural rule cache bakes the match result — without invalidation
    // the re-cascade reuses the stale rule set and attribute-driven rules
    // never re-match. Subtree because descendant chains ([a] .x) exist.
    el.InvalidateRuleCacheSubtree();
    if (markDirty)
        el.MarkDirtySubtree(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
}

void UIAttributeAccess::SetSelectorAttribute(UIElement& el, std::string_view name, std::string_view value, bool markDirty)
{
    if (name.empty())
        return;
    auto it = el.m_Attributes.find(name);
    if (it != el.m_Attributes.end() && it->second == value)
        return;
    std::string key(name);
    NormalizeAttrNameInPlace(key);
    el.m_Attributes[std::move(key)] = std::string(value);
    // P4 (C-10): see SetAuthoredAttribute — cached structural matches bake
    // the attribute state and must be rebuilt.
    el.InvalidateRuleCacheSubtree();
    if (markDirty)
        el.MarkDirtySubtree(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
}

const std::string* UIAttributeAccess::FindAuthoredAttribute(const UIElement& el, std::string_view name)
{
    if (name.empty())
        return nullptr;
    auto it = el.m_Attributes.find(name);
    if (it != el.m_Attributes.end())
        return &it->second;
    return nullptr;
}

const UIElement::AttributeMap& UIAttributeAccess::GetAuthoredAttributes(const UIElement& el)
{
    return el.m_Attributes;
}

void UIAttributeAccess::SetInlineStyleAttribute(UIElement& el, std::string_view inlineStyle)
{
    std::string trimmed(inlineStyle);
    // Trim leading/trailing whitespace.
    size_t a = 0, b = trimmed.size();
    while (a < b && std::isspace(static_cast<unsigned char>(trimmed[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(trimmed[b - 1])))
        --b;
    trimmed = trimmed.substr(a, b - a);

    if (trimmed == el.m_InlineStyleText)
        return;

    el.m_InlineStyleText = trimmed;
    if (el.m_InlineStyleText.empty())
    {
        el.m_InlineOverrides.Clear();
        el.MarkDirtySubtree(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
        return;
    }

    UIParsing::CSSParser::ParseInlineStyleToOverrides(el.m_InlineStyleText, el.m_InlineOverrides);
    el.MarkDirtySubtree(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
}
