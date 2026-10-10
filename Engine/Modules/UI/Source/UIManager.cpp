#include "UI/Internal/AttachDetachInternal.h"
#include "UI/UIManager.h"
#include "UIManager_Internal.h"
#include "DefaultStylesheet.h"
#include "UI/UiContext.h"
#include "UI/UiDispatcher.h"
#include "UI/UIFrameBufferRing.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"
#include "UI/UIHotReload.h"
#include "UI/TransitionEngine.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Interaction/DragDropOverlay.h"
#include "UI/Interaction/TooltipOverlay.h"

#include <cstdio>
#include "Logger/Backtrace.h"
#include "Logger/Logger.h"

#include "UI/Controls/DockspaceElement.h"
#include "UI/Controls/IVirtualizedControl.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/UIElement.h"
#include "UIAttributeAccess.h"

#include "Types/ColorUtils.h"
#include "UI/Controls/Mount.h"

#include "Input/KeyCodes.h"
#include "UI/Layout/YogaLayout.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/StyleUtil.h"
#include "UI/UIEvents.h"
#include "UI/UIStyle.h"

#include "AssetCore/SharedFileRead.h"
#include "Engine/Rendering/EmbeddedImageDecoder.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"

#include "Rendering/Text/FontAtlas.h"
#include "Rendering/Text/TextLayout.h"
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstring>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <ctime>

#include <cmath>
#include <vector>

#include <unordered_map>

#include <algorithm>
#include <cstddef>

#include "Assets/AssetManager.h"
#include "Assets/TextureAsset.h"

#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"

#if defined(GE_HAVE_KTX)
#include <ktx.h>
#endif

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
#include <yoga/Yoga.h>
#endif

// Explicitly register built-in UI controls at runtime (no linker tricks)
namespace GameEngine
{
namespace UIRegistration
{
void RegisterBuiltInControls();
}
} // namespace GameEngine

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::Geometry;
using namespace GameEngine::UIParsing;
using namespace GameEngine::Rendering::Text;

namespace GameEngine
{
// Internal forwarder used by UIElement (keeps UIElement.h free of UIManager.h include cycles).
// A no-op while the owner is being destroyed, so MarkDirty / MarkDirtySubtree from the
// owned tree being freed inside ~UIManager does not call back into the teardown.
void UIManagerNotifyElementDirty(UIManager* owner, UIElement* el, unsigned flags)
{
    if (owner && !owner->IsBeingDestroyed())
    {
        owner->NotifyElementDirty(el, flags);
    }
}

void UIManager::ShiftDrawOrderIndicesAfter(uint32_t threshold, int32_t delta)
{
    if (delta == 0 || !m_Root)
        return;
    constexpr uint32_t kInvalid = 0xFFFFFFFFu;
    // Iterative DFS — std::function would heap-allocate per call.
    // Reused thread-local scratch keeps the walk allocation-free.
    thread_local std::vector<UIElement*> stack;
    stack.clear();
    stack.push_back(m_Root.get());
    while (!stack.empty())
    {
        UIElement* el = stack.back();
        stack.pop_back();
        if (!el)
            continue;
        if (el->m_DrawOrderIdx != kInvalid && el->m_DrawOrderIdx >= threshold)
            el->m_DrawOrderIdx = static_cast<uint32_t>(static_cast<int32_t>(el->m_DrawOrderIdx) + delta);
        for (auto& ch : el->GetChildren())
            stack.push_back(ch.get());
        if (UIElement* tgt = el->GetMountTarget())
            stack.push_back(tgt);
    }
}

void UIManager::NotifyDirty_UpdateRegenFlag(UIElement* el, unsigned flags)
{
    // A mark can only invalidate the snapshot if the emit walk visits this
    // element's subtree, so ask the walk's own predicate. An empty layout rect
    // is NOT that question: a 0x0 box that does not clip still descends into
    // children which keep their own geometry, and dropping its mark strands
    // their primitives at stale values (nothing downstream recovers it — a
    // paint-only change moves no Yoga input, so no solve runs and the commit
    // walk never escalates).
    //
    // Hidden elements (display:none, opacity:0, inactive dock tab, not yet
    // attached) are still dropped — the predicate answers false for them. The
    // marks that UN-hide such an element reach a regen by other routes: a
    // display flip moves a Yoga input, and an opacity flip leaves the element
    // unstamped by the last full DFS, which escalates the drain.
    if (!el || !EmitWalkEntersSubtree(*el))
        return;
    // Block E1: only layout-affecting mutations require a full regen.
    // Pure VisualDirty is handled by DrainPrimitiveDataDirty (the element
    // is already on m_PrimitiveDataDirty via the queue push in
    // NotifyElementDirty) — drain rewrites just that element's primitive
    // bytes, no DFS, no DrawOrder shift.
    constexpr unsigned kLayoutAffecting =
        UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::ChildrenDirty;
    if (flags & kLayoutAffecting)
        MarkPrimitivesNeedRegen(0x1u);
}

void UIManager::MarkPrimitivesNeedRegen(uint32_t causeBit)
{
    m_PrimitivesNeedRegen.store(true, std::memory_order_relaxed);
    if (m_UpdateProfilingEnabled && !m_UpdateProfilingHistory.empty())
        m_UpdateProfilingHistory.back().GenAllPrimitivesRegenCause |= causeBit;
}

// Internal forwarder used by UIElement/Mount to signal structural mutations without
// including UIManager.h in UIElement.h.
void UIManagerNotifyTreeStructureChanged(UIManager* owner)
{
    if (owner)
    {
        owner->NotifyTreeStructureChanged();
    }
}

// Internal forwarder used by UIElement to allow UIManager to attach default
// control styles without including UIManager.h in UIElement.h.
void UIManagerNotifyElementOwnerChanged(UIManager* owner, UIElement* el)
{
    if (owner && el)
    {
        owner->NotifyElementOwnerChanged(el);
    }
}

void UIManager::RegisterLocalSheetElement(UIElement* el)
{
    if (el)
        m_LocalSheetElements.insert(el);
}

void UIManager::UnregisterLocalSheetElement(UIElement* el)
{
    if (el)
        m_LocalSheetElements.erase(el);
}

// Internal forwarders used by UIElement (keeps UIElement.h free of UIManager.h).
// No-ops while the owner is being destroyed, like the others.
void UIManagerRegisterLocalSheetElement(UIManager* owner, UIElement* el)
{
    if (owner && el && !owner->IsBeingDestroyed())
        owner->RegisterLocalSheetElement(el);
}

void UIManagerUnregisterLocalSheetElement(UIManager* owner, UIElement* el)
{
    if (owner && el && !owner->IsBeingDestroyed())
        owner->UnregisterLocalSheetElement(el);
}

void UIManagerUnregisterTransitioningElement(UIManager* owner, UIElement* el)
{
    if (owner && el && !owner->IsBeingDestroyed())
        owner->UnregisterTransitioningElement(el);
}

// Not gated on the manager being alive, unlike its neighbours: the index has to stay exact
// through ~UIManager too, because DetachSurvivingElements reads it after the owned tree is
// destroyed. `owner` never dangles: an element outliving its manager is detached by that
// manager's destructor.
void UIManagerUnregisterElementInstanceId(UIManager* owner, UIElement* el)
{
    if (owner && el)
        owner->UnregisterElementInstanceId(el->GetInstanceId(), el);
}

void UIManagerNotifyElementContentDirty(UIManager* owner, UIElement* el)
{
    // Content-only pipeline: queue the render-side drain, nothing else.
    // Deliberately does NOT touch m_DirtyHintFlags or m_ResolvedStylesDirty
    // — a content mutation is invisible to layout and style resolve, so
    // the Update side may idle-gate the frame while the render side
    // re-emits the element's slots and range-uploads them.
    if (!(owner && el && !owner->IsBeingDestroyed()))
        return;

    owner->AssertUiThread();

    // The drain can only rewrite slots the element already owns. An element
    // with no primitive range emitted nothing at its last DFS (empty text,
    // no background), and since this pipeline raises no dirty hints, no DFS
    // is coming to allocate it one — its first content can only paint
    // through a full regen. Mirrors the layout writeback's escalation for
    // rectChanged with cap == 0 (cause 0x80). Elements that own a range —
    // every steady-state field the content pipeline exists for — still take
    // the surgical drain.
    if (el->m_PrimitiveRangeCap == 0 || el->m_PrimitiveRangeCount == 0)
    {
        // A subtree the emit walk will not enter cannot be helped by the
        // regen (the DFS still emits nothing for it), and unlike a rect
        // change a content mark can repeat every frame — so the escalation
        // is gated on reachability.
        if (!UIManager::EmitWalkEntersSubtree(*el))
            return;
        owner->MarkPrimitivesNeedRegen(0x800u);
        return;
    }

    owner->PushPrimitiveDataDirty(el);
}

void UIManager::RegisterScrollView(ScrollView* sv)
{
    if (!sv)
        return;
    if (std::find(m_ScrollViewRegistry.begin(), m_ScrollViewRegistry.end(), sv) ==
        m_ScrollViewRegistry.end())
    {
        m_ScrollViewRegistry.push_back(sv);
    }
}

void UIManager::UnregisterScrollView(ScrollView* sv)
{
    m_ScrollViewRegistry.erase(
        std::remove(m_ScrollViewRegistry.begin(), m_ScrollViewRegistry.end(), sv),
        m_ScrollViewRegistry.end());
}

void UIManager::RegisterDismissablePopup(DismissablePopup* popup)
{
    if (!popup)
        return;
    if (std::find(m_DismissablePopupRegistry.begin(), m_DismissablePopupRegistry.end(), popup) ==
        m_DismissablePopupRegistry.end())
    {
        m_DismissablePopupRegistry.push_back(popup);
    }
}

void UIManager::UnregisterDismissablePopup(DismissablePopup* popup)
{
    m_DismissablePopupRegistry.erase(
        std::remove(m_DismissablePopupRegistry.begin(), m_DismissablePopupRegistry.end(), popup),
        m_DismissablePopupRegistry.end());
}

void UIManager::RegisterVirtualizedControl(IVirtualizedControl* control)
{
    if (!control)
        return;
    for (const auto& e : m_VirtualizedControlRegistry)
    {
        if (e.Control == control)
            return;
    }
    // Seed the last-seen version with the control's current provider version so a
    // freshly attached control does not spuriously enqueue on its first frame
    // (its own mount-time virtualization already covers the initial bind).
    m_VirtualizedControlRegistry.push_back(
        VirtualizedControlPumpEntry{control, control->ProviderChangeVersion()});
}

void UIManager::UnregisterVirtualizedControl(IVirtualizedControl* control)
{
    m_VirtualizedControlRegistry.erase(
        std::remove_if(m_VirtualizedControlRegistry.begin(), m_VirtualizedControlRegistry.end(),
                       [control](const VirtualizedControlPumpEntry& e) { return e.Control == control; }),
        m_VirtualizedControlRegistry.end());
}

void UIManagerFreeRenderSlots(UIManager* owner, UIElement* el)
{
    // A no-op while the owner is being destroyed: its owned tree is freed from inside
    // ~UIManager, and the slot allocator is about to go with it.
    if (owner && el && !owner->IsBeingDestroyed())
    {
        owner->FreeRenderSlots(el);
    }
}

void UIManagerRemoveFromDirtyQueues(UIManager* owner, UIElement* el)
{
    if (owner && el && !owner->IsBeingDestroyed())
    {
        owner->RemoveFromDirtyQueues(el);
    }
}

// Analysis-driven :checked / custom-state markers, routed from ToggleBase and
// UIElement so those TUs stay clear of UIManager.h. No-ops while the owner is
// being destroyed, like the other forwarders.
void UIManagerMarkCheckedStateScope(UIManager* owner, UIElement* el)
{
    if (owner && el && !owner->IsBeingDestroyed())
    {
        owner->MarkCheckedStateScope(el);
    }
}

void UIManagerMarkCustomStateScope(UIManager* owner, UIElement* el)
{
    if (owner && el && !owner->IsBeingDestroyed())
    {
        owner->MarkCustomStateScope(el);
    }
}

bool UIManagerUsesSiblingCombinators(const UIManager* owner)
{
    // Cascade-memoization Phase 6 forwarder: read the analysis flag
    // through a free function so UIElement.h can consult it without
    // pulling in UIManager.h. Answers false while the owner is being
    // destroyed, like the other forwarders.
    if (!owner || owner->IsBeingDestroyed())
        return false;
    return owner->UsesSiblingCombinators();
}

uint32_t UIManagerRuleCacheEpoch(const UIManager* owner)
{
    if (!owner || owner->IsBeingDestroyed())
        return 0;
    return owner->GetRuleCacheEpoch();
}

void UIManager::FreeRenderSlots(UIElement* el)
{
    if (!el)
        return;

    if (el->m_PrimitiveRangeCap > 0)
    {
        m_PrimitiveAllocator.Free(el->m_PrimitiveRangeStart, el->m_PrimitiveRangeCap);
        el->m_PrimitiveRangeStart = 0xFFFFFFFFu;
        el->m_PrimitiveRangeCount = 0;
        el->m_PrimitiveRangeCap   = 0;
    }
    if (el->m_ClipSlotIdx != 0xFFFFu)
    {
        m_ClipAllocator.Free(el->m_ClipSlotIdx);
        el->m_ClipSlotIdx = 0xFFFFu;
    }
    // Draw-order is a compact array (no allocator); nothing to free.
    el->m_DrawOrderIdx = 0xFFFFFFFFu;
}

void UIManager::PushPrimitiveDataDirty(UIElement* el)
{
    AssertUiThread();
    // Dedup'd via UIElement::m_InQueueFlags — the element keeps the bit set
    // for the duration of queue membership; DrainPrimitiveDataDirty clears
    // it on dequeue. Pushing a null element or an element already enqueued
    // is a cheap no-op.
    if (!el || (el->m_InQueueFlags & UIElement::InPrimitiveDataDirty))
        return;
    el->m_InQueueFlags |= UIElement::InPrimitiveDataDirty;
    m_PrimitiveDataDirty.push_back(el);
}

void UIManager::RemoveFromDirtyQueues(UIElement* el)
{
    if (!el)
        return;

    // Erase from the :focus-within chain so a subsequent
    // RebuildFocusWithinChain doesn't dereference a dangling pointer in its
    // old-vs-new diff. Tracked via the InFocusChain bit in m_InQueueFlags so
    // we can skip the map probe when not in chain.
    if (el->m_InQueueFlags & UIElement::InFocusChain)
    {
        m_FocusWithinChain.erase(el);
        el->m_InQueueFlags &= ~(UIElement::InFocusChain |
            UIElement::InFocusLeaf | UIElement::InFocusVisibleLeaf);
    }

    // Tombstone the element's entry in the primitive-data queue (the drain
    // skips nullptr).
    if (el->m_InQueueFlags & UIElement::InPrimitiveDataDirty)
    {
        for (auto& e : m_PrimitiveDataDirty)
            if (e == el)
                e = nullptr;
    }

    el->m_InQueueFlags = 0;
}

void UIManager::RebuildFocusWithinChain()
{
    // Find the focused element by walking the tree once. If m_FocusId is
    // empty or the id no longer resolves to a live element, the chain is
    // empty (nothing has :focus-within).
    UIElement* focused = nullptr;
    if (!m_FocusId.empty() && m_Root)
        focused = m_Root->FindById(m_FocusId);

    // A control that is disabled while it holds focus loses it, and so does one
    // inside a container that is disabled — the focus a user cannot reach by Tab
    // or by pressing the control is not focus it should keep by having been
    // there first. Dropping the id here also strips :focus and :focus-within
    // through the diff below, because the chain becomes empty.
    if (focused && !focused->IsEnabledInHierarchy())
    {
        m_FocusId.clear();
        focused = nullptr;
    }

    // Build the new chain (focused element + every ancestor up to root).
    // Stored in a small thread_local scratch first so we can diff against
    // the previous chain without allocating.
    static thread_local std::vector<UIElement*> tl_newChain;
    tl_newChain.clear();
    for (UIElement* e = focused; e; e = e->GetParent())
        tl_newChain.push_back(e);

    // Symmetric difference vs the previous chain — those elements transition
    // either INTO or OUT OF the :focus-within state, so they need to
    // re-cascade. Other elements' FocusWithin is unchanged.
    //
    // The chain leaf (focused element) flips :focus / :focus-visible in addition
    // to :focus-within; its ancestors flip :focus-within only. Consulting the
    // union of all three through MarkPseudoStateScope means an element entering
    // or leaving the chain (a) raises LayoutDirty when any of those pseudos is
    // layout-affecting — not the StyleDirty-only mark this used to make, which
    // leaned on the needCascade→signature fallback to reach a solve — and
    // (b) fans the mark out to off-chain descendants for `.x:focus-within .y`
    // rules and to siblings for combinator rules, which the old chain-only mark
    // never reached. SetFocusById marks just the newly focused element, so this
    // symmetric-difference pass is the sole path that strips :focus / :focus-
    // within styling from the element losing focus on a programmatic change.
    const auto chainInfo = CombinePseudoInfo(
        CombinePseudoInfo(m_StyleAnalysis.Focus, m_StyleAnalysis.FocusVisible),
        m_StyleAnalysis.FocusWithin);
    constexpr std::uint16_t kFocusBits =
        UIElement::FocusRules | UIElement::FocusVisibleRules | UIElement::FocusWithinRules;

    for (UIElement* e : tl_newChain)
    {
        if (m_FocusWithinChain.count(e) == 0)
            MarkPseudoStateScope(e, chainInfo, kFocusBits); // newly entered: gain :focus-within
    }
    for (UIElement* e : m_FocusWithinChain)
    {
        bool stillIn = false;
        for (UIElement* n : tl_newChain)
            if (n == e) { stillIn = true; break; }
        if (!stillIn && e)
            MarkPseudoStateScope(e, chainInfo, kFocusBits); // left the chain: lose :focus-within
    }

    // Stage 5 Block A part 3e: clear the InFocusChain bit on departing
    // elements + set it on the new chain entries so ~UIElement can gate
    // RemoveFromDirtyQueues on m_InQueueFlags != 0 (which now includes
    // chain membership). The focus-leaf bits (:focus / :focus-visible) are a
    // strict subset of the chain, so they are cleared together here and re-set
    // on the new leaf below.
    constexpr std::uint8_t kFocusChainClearBits = UIElement::InFocusChain |
        UIElement::InFocusLeaf | UIElement::InFocusVisibleLeaf;
    for (UIElement* e : m_FocusWithinChain)
    {
        if (e)
            e->m_InQueueFlags &= ~kFocusChainClearBits;
    }
    m_FocusWithinChain.clear();
    for (UIElement* e : tl_newChain)
    {
        m_FocusWithinChain.insert(e);
        if (e)
            e->m_InQueueFlags |= UIElement::InFocusChain;
    }
    // The focused element itself (:focus) is the chain leaf — tl_newChain.front(),
    // pushed first during the parent ascent above. :focus-visible additionally
    // requires that focus arrived via the keyboard. These per-element bits are
    // read (never written) by the cascade matcher for non-target compounds, and
    // are refreshed here at Update entry before the build pass, so they stay
    // read-only during a (possibly parallel) cascade.
    if (!tl_newChain.empty() && tl_newChain.front())
    {
        UIElement* leaf = tl_newChain.front();
        leaf->m_InQueueFlags |= UIElement::InFocusLeaf;
        if (m_FocusViaKeyboard)
            leaf->m_InQueueFlags |= UIElement::InFocusVisibleLeaf;
    }
}

// Detach a child's Yoga node from its parent's Yoga tree before the child
// UIElement is destroyed. Idempotent: missing m_YogaState on either side
// means there's nothing to detach.
void UIElementDetachYogaChild(UIElement* parent, UIElement* child)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!parent || !child) return;

    YGNodeRef parentNode = parent->m_YogaState ? parent->m_YogaState->Node : nullptr;
    YGNodeRef childNode  = child->m_YogaState  ? child->m_YogaState->Node  : nullptr;
    if (!parentNode || !childNode) return;
    if (YGNodeGetParent(childNode) != parentNode) return;
    YGNodeRemoveChild(parentNode, childNode);
#else
    (void)parent;
    (void)child;
#endif
}

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
namespace
{
// Allocate m_YogaState + YGNode if missing. Synced m_YogaNode shadow.
YGNodeRef EnsureYogaStateForElement(UIElement* el)
{
    if (!el) return nullptr;
    if (el->m_YogaState && el->m_YogaState->Node)
        return el->m_YogaState->Node;
    if (!el->m_YogaState)
        el->m_YogaState = std::make_unique<RetainedYogaNode>();
    if (!el->m_YogaState->Node)
    {
        YGNodeRef n = UILayout::YogaAdapter::CreateNode();
        el->m_YogaState->Node = n;
        el->m_YogaState->InstanceId = el->GetInstanceId();
        el->m_YogaNode = n;
    }
    return el->m_YogaState->Node;
}
} // namespace
#endif

void UIElementAttachYogaChild(UIElement* parent, UIElement* child)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!parent || !child) return;
    UIManager* owner = parent->GetOwnerManager();
    if (!owner) owner = child->GetOwnerManager();
    if (!owner) return;     // Off-line tree; BuildYogaRecursive handles attach later.

    YGNodeRef parentNode = EnsureYogaStateForElement(parent);
    YGNodeRef childNode  = EnsureYogaStateForElement(child);
    if (!parentNode || !childNode) return;
    if (YGNodeGetParent(childNode) == parentNode) return;  // already attached

    if (YGNodeRef priorParent = YGNodeGetParent(childNode))
        YGNodeRemoveChild(priorParent, childNode);

    YGNodeInsertChild(parentNode, childNode, YGNodeGetChildCount(parentNode));
#else
    (void)parent;
    (void)child;
#endif
}
} // namespace GameEngine

// A UI element can end up pointing at a block-compressed payload two ways, and
// the per-pixel RGBA a preview needs comes from a different place in each.
//
//  - An AUTHORED container (.ktx2 / .basis) holds a Basis-supercompressed
//    payload the container itself transcodes, and the asset's path names it.
//  - A cooked artifact of an ordinary image is plain BC4/BC5/BC7 — no
//    transcoder decodes that — and the asset's path still names the .png/.jpg
//    it was cooked from, so the source file is what answers.
//
// Getting this wrong is silent: the upload is refused and the element draws
// nothing where a picture should be.
static bool IsTranscodableContainerPath(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ktx2" || ext == ".basis";
}

// Decode an ordinary image file to RGBA8 for a preview. Full resolution: the
// caller caches the upload per asset, and a UI background is scaled by the
// style, not by the payload.
static bool DecodeSourceImageToRgba8(const std::filesystem::path& path,
                                     std::vector<unsigned char>& outRgba,
                                     uint32_t& outWidth, uint32_t& outHeight)
{
    Vector<uint8> bytes;
    if (!ReadFileBytesShared(path, bytes) || bytes.empty())
        return false;

    const DecodedImage decoded = DecodeImageToRGBA(bytes.data(), bytes.size());
    if (!decoded.valid || decoded.width == 0 || decoded.height == 0)
        return false;

    outRgba = std::vector<unsigned char>(decoded.pixels.begin(), decoded.pixels.end());
    outWidth = decoded.width;
    outHeight = decoded.height;
    return true;
}

#if defined(GE_HAVE_KTX)
// Transcode an authored container to RGBA32 and take level 0 only — previews
// never need the mip chain.
static bool DecodeKtx2Level0ToRgba8(const std::filesystem::path& path,
                                    std::vector<unsigned char>& outRgba,
                                    uint32_t& outWidth, uint32_t& outHeight)
{
    ktxTexture2* ktexture = nullptr;
    if (ktxTexture2_CreateFromNamedFile(path.string().c_str(),
                                        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                        &ktexture) != KTX_SUCCESS || !ktexture)
        return false;

    ktxTexture* base = ktxTexture(ktexture);
    bool ok = ktxTexture2_NeedsTranscoding(ktexture) &&
              ktxTexture2_TranscodeBasis(ktexture, KTX_TTF_RGBA32, KTX_TF_HIGH_QUALITY) == KTX_SUCCESS;
    if (ok)
    {
        ktx_size_t level0Offset = 0;
        ok = ktxTexture_GetImageOffset(base, 0, 0, 0, &level0Offset) == KTX_SUCCESS;
        if (ok)
        {
            const ktx_size_t level0Size = ktxTexture_GetImageSize(base, 0);
            const ktx_uint8_t* data = ktxTexture_GetData(base);
            const ktx_size_t totalSize = ktxTexture_GetDataSize(base);
            const uint64 expected = static_cast<uint64>(base->baseWidth) * base->baseHeight * 4u;
            ok = data && level0Size == expected && level0Offset + level0Size <= totalSize;
            if (ok)
            {
                outRgba.assign(data + level0Offset, data + level0Offset + level0Size);
                outWidth = base->baseWidth;
                outHeight = base->baseHeight;
            }
        }
    }
    ktxTexture2_Destroy(ktexture);
    return ok && outWidth != 0 && outHeight != 0;
}
#endif // GE_HAVE_KTX

// Helper used by background-image handling to upload a TextureAsset into a
// RGBA8 UI texture, converting common 8-bit formats (R8/RG8/RGB8) to RGBA8
// when needed so that 24-bit PNGs and similar assets can be used as
// thumbnails/backgrounds.
Rendering::TextureHandle UIManager::CreateUIBackgroundTextureFromAsset(Rendering::IDevice* device, const TextureAsset* texA)
{
    if (!device || !texA)
        return Rendering::TextureHandle{};

    uint32_t width = texA->GetWidth();
    uint32_t height = texA->GetHeight();
    if (!width || !height)
        return Rendering::TextureHandle{};

    const auto fmt = texA->GetFormat();
    const uint32_t channels = texA->GetChannels();
    const uint64 dataSize = texA->GetDataSize();
    const uint8* pixelsTyped = texA->GetPixelData();
    if (!pixelsTyped || dataSize == 0)
        return Rendering::TextureHandle{};

    const unsigned char* src = reinterpret_cast<const unsigned char*>(pixelsTyped);
    const uint64 pixelCount = static_cast<uint64>(width) * static_cast<uint64>(height);
    if (pixelCount == 0)
        return Rendering::TextureHandle{};

    const void* uploadData = src;
    uint64 uploadSize = dataSize;
    std::vector<unsigned char> converted;

    // Block-compressed container payloads (KTX2 -> BC7) are not per-pixel
    // addressable; re-decode the CPU-readable file (the source, or a package's
    // portable payload) to RGBA32 for the preview.
    bool decodedFromContainer = false;
    if (texA->IsBlockCompressed())
    {
        const std::filesystem::path& assetPath = texA->GetCpuDecodePath();
        bool decoded = false;
#if defined(GE_HAVE_KTX)
        if (IsTranscodableContainerPath(assetPath))
            decoded = DecodeKtx2Level0ToRgba8(assetPath, converted, width, height);
        else
#endif
            decoded = DecodeSourceImageToRgba8(assetPath, converted, width, height);

        if (!decoded)
        {
            static bool sWarnedBlockCompressedOnce = false;
            if (!sWarnedBlockCompressedOnce)
            {
                Logger::Log::Warning("UIManager: failed to re-decode block-compressed texture '{}' for UI preview.",
                                     assetPath.string());
                sWarnedBlockCompressedOnce = true;
            }
            return Rendering::TextureHandle{};
        }
        uploadData = converted.data();
        uploadSize = static_cast<uint64>(converted.size());
        decodedFromContainer = true;
    }

    auto isSupported8Bit = [](GameEngine::TextureFormat f) -> bool
    {
        switch (f)
        {
        case GameEngine::TextureFormat::R8:
        case GameEngine::TextureFormat::RG8:
        case GameEngine::TextureFormat::RGB8:
        case GameEngine::TextureFormat::RGBA8:
            return true;
        default:
            return false;
        }
    };

    auto isSupportedFloat = [](GameEngine::TextureFormat f) -> bool
    {
        switch (f)
        {
        case GameEngine::TextureFormat::R32F:
        case GameEngine::TextureFormat::RG32F:
        case GameEngine::TextureFormat::RGB32F:
        case GameEngine::TextureFormat::RGBA32F:
            return true;
        default:
            return false;
        }
    };

    if (decodedFromContainer)
    {
        // RGBA8 level-0 data is already in `converted`; skip format handling.
    }
    else if (isSupportedFloat(fmt))
    {
        if (dataSize < pixelCount * channels * sizeof(float))
            return Rendering::TextureHandle{};

        auto toByte = [](float v) -> unsigned char
        {
            if (!std::isfinite(v))
                v = 0.0f;
            v = std::max(0.0f, v);
            v = v / (1.0f + v);
            v = std::pow(v, 1.0f / 2.2f);
            v = std::clamp(v, 0.0f, 1.0f);
            return static_cast<unsigned char>(v * 255.0f + 0.5f);
        };

        auto alphaToByte = [](float v) -> unsigned char
        {
            if (!std::isfinite(v))
                v = 1.0f;
            v = std::clamp(v, 0.0f, 1.0f);
            return static_cast<unsigned char>(v * 255.0f + 0.5f);
        };

        converted.resize(static_cast<size_t>(pixelCount * 4u));
        const float* srcIt = reinterpret_cast<const float*>(pixelsTyped);
        unsigned char* dst = converted.data();
        for (uint64 i = 0; i < pixelCount; ++i)
        {
            const float r = channels >= 1 ? srcIt[0] : 0.0f;
            const float g = channels >= 2 ? srcIt[1] : r;
            const float b = channels >= 3 ? srcIt[2] : g;
            const float a = channels >= 4 ? srcIt[3] : 1.0f;
            dst[0] = toByte(r);
            dst[1] = toByte(g);
            dst[2] = toByte(b);
            dst[3] = alphaToByte(a);
            srcIt += channels;
            dst += 4;
        }

        uploadData = converted.data();
        uploadSize = static_cast<uint64>(converted.size());
    }
    else if (!isSupported8Bit(fmt))
    {
        static bool sWarnedOnce = false;
        if (!sWarnedOnce)
        {
            Logger::Log::Warning("UIManager: background image texture '{}' has unsupported format for UI backgrounds; expected 8-bit LDR or HDR float texture. Background will not render.", texA->GetPath().string());
            sWarnedOnce = true;
        }
        return Rendering::TextureHandle{};
    }

    // Fast path: already RGBA8 with four channels, upload directly.
    if (!decodedFromContainer &&
        isSupported8Bit(fmt) && !(fmt == GameEngine::TextureFormat::RGBA8 && channels == 4))
    {
        // Convert to RGBA8 with opaque alpha so we can upload as UI texture.
        converted.resize(static_cast<size_t>(pixelCount * 4u));
        unsigned char* dst = converted.data();

        if (fmt == GameEngine::TextureFormat::RGB8 && channels >= 3)
        {
            const unsigned char* srcIt = src;
            for (uint64 i = 0; i < pixelCount; ++i)
            {
                dst[0] = srcIt[0];
                dst[1] = srcIt[1];
                dst[2] = srcIt[2];
                dst[3] = 255u;
                srcIt += channels;
                dst += 4;
            }
        }
        else if (fmt == GameEngine::TextureFormat::R8 && channels >= 1)
        {
            const unsigned char* srcIt = src;
            for (uint64 i = 0; i < pixelCount; ++i)
            {
                unsigned char v = srcIt[0];
                dst[0] = v;
                dst[1] = v;
                dst[2] = v;
                dst[3] = 255u;
                srcIt += channels;
                dst += 4;
            }
        }
        else if (fmt == GameEngine::TextureFormat::RG8 && channels >= 2)
        {
            const unsigned char* srcIt = src;
            for (uint64 i = 0; i < pixelCount; ++i)
            {
                unsigned char r = srcIt[0];
                unsigned char g = srcIt[1];
                dst[0] = r;
                dst[1] = g;
                dst[2] = g;
                dst[3] = 255u;
                srcIt += channels;
                dst += 4;
            }
        }
        else
        {
            // Unexpected combination; fail gracefully.
            return Rendering::TextureHandle{};
        }

        uploadData = converted.data();
        uploadSize = static_cast<uint64>(converted.size());
    }

    TextureDesc td{};
    td.width = width;
    td.height = height;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    Rendering::TextureFormat gpuFormat =
        (texA->GetColorSpace() == TextureColorSpace::SRGB)
            ? Rendering::TextureFormat::RGBA8_SRGB
            : Rendering::TextureFormat::RGBA8_UNORM;
    td.format = (uint32_t)gpuFormat;
    td.usage = (uint32_t)Rendering::TextureUsage::ShaderResource | (uint32_t)Rendering::TextureUsage::TransferDst;
    td.debugName = "UI_BG";

    Rendering::TextureHandle tex = device->CreateTexture(td);
    if (!tex)
        return Rendering::TextureHandle{};

    BufferDesc staging{};
    staging.size = uploadSize;
    staging.usage = (uint32_t)BufferUsage::TransferSrc;
    staging.memoryUsage = BufferMemoryUsage::Upload;
    staging.debugName = "UI_BG_Staging";
    BufferHandle sb = device->CreateBuffer(staging);
    if (!sb)
    {
        device->DestroyTexture(tex);
        return Rendering::TextureHandle{};
    }

    device->UpdateBuffer(sb, 0, staging.size, uploadData);
    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::Undefined, ResourceState::CopyDest, 0, 1, 0, 1));
    cl->CopyBufferToTextureSubresource(sb, tex, 0, 0, td.width, td.height, 0, 0);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest, ResourceState::ShaderResource, 0, 1, 0, 1));
    cl->End();
    device->ExecuteCommandLists(std::vector<CommandList*>{cl.get()});
    // Do not stall the GPU here. Backends are expected to either:
    // - defer buffer destruction until GPU completion (e.g., Vulkan timelines), or
    // - execute command lists synchronously (e.g., the current D3D12 backend).
    device->DestroyBuffer(sb);
    return tex;
}

// Ensure a UIElement has a non-empty id. Many controls created from C++ do
// not explicitly assign ids but still participate in focus and text input.
// UIManager relies on ids to track focus (m_FocusId) and to build
// m_FocusOrder, so we lazily assign a stable auto id the first time we need
// one.
std::string UIManager::EnsureElementId(UIElement* element)
{
    if (!element)
    {
        return std::string();
    }

    const std::string& existingId = element->GetId();
    if (!existingId.empty())
    {
        return existingId;
    }

    std::string newId = std::string("_auto_") + std::to_string(m_NextAutoElementId++);
    element->SetId(newId);
    return element->GetId();
}

#if defined(_DEBUG)

namespace
{
	// Small helpers for debug export so they are completely removed in
	// non-debug builds together with DebugExportLayoutAndStyles.

	inline void AppendIndent(std::string& s, int indent)
	{
	    if (indent > 0)
	    {
	        s.append(static_cast<size_t>(indent) * 2u, ' ');
	    }
	}

	inline void AppendEscapedXml(std::string& s, const std::string& value)
	{
	    for (char c : value)
	    {
	        switch (c)
	        {
	        case '&': s += "&amp;"; break;
	        case '<': s += "&lt;";  break;
	        case '>': s += "&gt;";  break;
	        case '"': s += "&quot;"; break;
	        case '\'': s += "&apos;"; break;
	        default:  s.push_back(c); break;
	        }
	    }
	}

	inline void AppendCssColor(std::string& s, uint32_t argb)
	{
	    // Stored as ARGB; convert to RGBA() for CSS.
	    uint8_t a = static_cast<uint8_t>((argb >> 24) & 0xFFu);
	    uint8_t r = static_cast<uint8_t>((argb >> 16) & 0xFFu);
	    uint8_t g = static_cast<uint8_t>((argb >> 8) & 0xFFu);
	    uint8_t b = static_cast<uint8_t>(argb & 0xFFu);
	    char buf[64];
	    float af = static_cast<float>(a) / 255.0f;
	    std::snprintf(buf, sizeof(buf), "rgba(%u,%u,%u,%.3f)", static_cast<unsigned>(r), static_cast<unsigned>(g), static_cast<unsigned>(b), static_cast<double>(af));
	    s += buf;
	}

		inline void AppendCssLength(std::string& s, float value, bool isPercent)
		{
		    s += std::to_string(value);
		    s += isPercent ? "%" : "px";
		}

        inline const char* WordBreakToCss(WordBreak wb)
        {
            switch (wb)
            {
            case WordBreak::BreakAll:
                return "break-all";
            case WordBreak::KeepAll:
                return "keep-all";
            case WordBreak::BreakWord:
                return "break-word";
            case WordBreak::Normal:
            default:
                return "normal";
            }
        }
		
			// Append a subset of the resolved style as inline CSS. Geometry is based on
			// the *final* engine layout rect, converted to coordinates that are local to
			// the parent layout rect. External tools are expected to reconstruct layout
			// using only standard CSS concepts (display/flex, position, left/top,
			// width/height) and may ignore the data-layout-* attributes that are also
			// exported for debugging/metadata.
			void AppendResolvedStyleCss(const GameEngine::UIElement* element,
		                           float parentLayoutX,
		                           float parentLayoutY,
		                           const GameEngine::ResolvedStyle* style,
		                           std::string& out)
			{
			    if (!style || !element)
			        return;
			
				    // Layout mode. External tools commonly treat display:block as a
				    // column-oriented flex container by default; we still emit the
				    // original display mode here but expose flex properties for both
				    // Flex and Block so flexbox-only renderers can consume them.
				    GameEngine::DisplayMode displayMode = style->Layout.DisplayMode;
				    switch (displayMode)
				    {
				    case GameEngine::DisplayMode::None:  out += "display:none;";  break;
				    case GameEngine::DisplayMode::Flex:  out += "display:flex;";  break;
				    case GameEngine::DisplayMode::Block: out += "display:block;"; break;
				    default: break;
				    }
				
				    const bool isFlexLike =
				        (displayMode == GameEngine::DisplayMode::Flex ||
				         displayMode == GameEngine::DisplayMode::Block);
				
				    // Flex properties (subset that is useful for external tools).
				    // These are emitted for both Flex and Block modes so that
				    // HTML/figma exporters that implement only flexbox layout can
				    // still reconstruct rows/columns even when the original UI
				    // element used display:block.
				    if (isFlexLike)
			    {
			        using FlexDir = GameEngine::FlexDirection;
			        switch (style->Layout.FlexDirection)
			        {
			        case FlexDir::Row:    out += "flex-direction:row;";    break;
			        case FlexDir::Column: out += "flex-direction:column;"; break;
			        default: break;
			        }
			
		        if (style->Layout.FlexWrap)
		            out += "flex-wrap:wrap;";
		
		        using Justify = GameEngine::JustifyContent;
		        switch (style->Layout.JustifyContent)
			        {
			        case Justify::FlexStart:   out += "justify-content:flex-start;";   break;
			        case Justify::FlexEnd:     out += "justify-content:flex-end;";     break;
			        case Justify::Center:      out += "justify-content:center;";       break;
			        case Justify::SpaceBetween: out += "justify-content:space-between;"; break;
			        case Justify::SpaceAround:  out += "justify-content:space-around;";  break;
			        case Justify::SpaceEvenly:  out += "justify-content:space-evenly;";  break;
			        default: break;
			        }
			
			        using Align = GameEngine::AlignItems;
			        switch (style->Layout.AlignItems)
			        {
			        case Align::FlexStart: out += "align-items:flex-start;"; break;
			        case Align::FlexEnd:   out += "align-items:flex-end;";   break;
			        case Align::Center:    out += "align-items:center;";     break;
			        case Align::Stretch:   out += "align-items:stretch;";    break;
			        default: break;
			        }
			    }
			
			    // Geometry based on the final engine layout rect. For elements that are
			    // positioned "absolute" in the engine, we emit CSS absolute positioning
			    // using coordinates *relative to the parent* so that nested offsets don't
			    // compound. For normal (relative) elements we keep them in normal flex/block
			    // flow and only lock width/height.
			    const float absX = element->GetLayoutX();
			    const float absY = element->GetLayoutY();
			    const float w    = element->GetLayoutWidth();
			    const float h    = element->GetLayoutHeight();
			    const float localX = absX - parentLayoutX;
			    const float localY = absY - parentLayoutY;
			
			    const bool isAbsolute =
			        (style->Layout.PositionType == GameEngine::PositionType::Absolute);
			
			    if (isAbsolute)
			    {
			        out += "position:absolute;";
			
			        if (w > 0.0f)
			        {
			            out += "width:";
			            AppendCssLength(out, w, false);
			            out += ";";
			        }
			        if (h > 0.0f)
			        {
			            out += "height:";
			            AppendCssLength(out, h, false);
			            out += ";";
			        }
			
			        out += "left:";
			        out += std::to_string(localX);
			        out += "px;";
			
			        out += "top:";
			        out += std::to_string(localY);
			        out += "px;";
			    }
			    else
			    {
			        // Non-absolute elements participate in normal layout. We still emit the
			        // final width/height from Yoga so external tools see concrete geometry,
			        // but we avoid forcing additional left/top offsets that would "double
			        // apply" the effects of things like DockTabBar height or split panes.
			        // We do, however, mark them as position:relative so that any absolutely
			        // positioned descendants use this box as their containing block instead
			        // of the page.
			        out += "position:relative;";
			
			        if (w > 0.0f)
			        {
			            out += "width:";
			            AppendCssLength(out, w, false);
			            out += ";";
			        }
			        if (h > 0.0f)
			        {
			            out += "height:";
			            AppendCssLength(out, h, false);
			            out += ";";
			        }
			    }
				
				    // Stacking context: emit z-index when explicitly set so external
				    // tools can reproduce engine hit-testing and paint order.
			    if (style->Layout.ZIndex != 0)
			    {
			        out += "z-index:";
			        out += std::to_string(style->Layout.ZIndex);
			        out += ";";
			    }
		
		    // Background and text colors
		    if (style->Visual.BackgroundColor != 0x00000000u)
		    {
		        out += "background-color:";
		        AppendCssColor(out, style->Visual.BackgroundColor);
		        out += ";";
		    }
		    if (style->Visual.HasColor)
		    {
		        out += "color:";
		        AppendCssColor(out, style->Visual.Color);
		        out += ";";
		    }
		
		    // Font
		    if (style->Visual.HasFontSize)
		    {
		        out += "font-size:";
		        AppendCssLength(out, style->Visual.FontSize, false);
		        out += ";";
		    }
		    if (style->Visual.HasLetterSpacing)
		    {
		        out += "letter-spacing:";
		        AppendCssLength(out, style->Visual.LetterSpacing, false);
		        out += ";";
		    }
                if (style->Visual.WordBreak != WordBreak::Normal)
                {
                    out += "word-break:";
                    out += WordBreakToCss(style->Visual.WordBreak);
                    out += ";";
                }
                if (style->Visual.OverflowWrap != OverflowWrap::Normal)
                {
                    out += "overflow-wrap:break-word;";
                }
			}

		void WriteElementXml(const GameEngine::UIElement* element,
		                     std::string& out,
		                     int indent,
		                     float parentLayoutX,
		                     float parentLayoutY)
		{
		    if (!element)
		        return;
		
		    AppendIndent(out, indent);
		
		    const std::string tag = UIAttributeAccess::GetDebugTypeName(*element);
		    out += "<";
		    out += tag;
		
		    const std::string& id = element->GetId();
		    if (!id.empty())
		    {
		        out += " id=\"";
		        AppendEscapedXml(out, id);
		        out += "\"";
		    }
		
		    const auto& classes = element->GetClasses();
		    if (!classes.empty())
		    {
		        out += " class=\"";
		        bool first = true;
		        for (const auto& cls : classes)
		        {
		            if (!first)
		                out.push_back(' ');
		            AppendEscapedXml(out, cls);
		            first = false;
		        }
		        out += "\"";
		    }
		
		    // Custom attributes. If user-specified attributes contain a "style"
		    // entry, merge it into our computed CSS instead of emitting a
		    // duplicate style attribute (which is invalid XML/HTML).
            const auto& attrs = UIAttributeAccess::GetAuthoredAttributes(*element);
		    std::string inlineStyle;
		    for (const auto& kv : attrs)
		    {
		        const std::string& name = kv.first;
		        if (name == "id" || name == "class")
		            continue;
		        if (name == "style")
		        {
		            inlineStyle = kv.second;
		            continue;
		        }
		        out.push_back(' ');
		        out += name;
		        out += "=\"";
		        AppendEscapedXml(out, kv.second);
		        out += "\"";
		    }
		
		    // Embed last layout rect to help external tools reconstruct geometry.
		    const float absX = element->GetLayoutX();
		    const float absY = element->GetLayoutY();
		    const float w    = element->GetLayoutWidth();
		    const float h    = element->GetLayoutHeight();
		    out += " data-layout-x=\"";
		    out += std::to_string(absX);
		    out += "\" data-layout-y=\"";
		    out += std::to_string(absY);
		    out += "\" data-layout-w=\"";
		    out += std::to_string(w);
		    out += "\" data-layout-h=\"";
		    out += std::to_string(h);
		    out += "\"";
		
		    // Inline a subset of the resolved style as CSS "style" attribute for
		    // easier importing into layout tools. If the element had an explicit
		    // "style" attribute in its definition, append it so both the resolved
		    // engine style and the authored inline style are preserved.
		    const GameEngine::ResolvedStyle& resolved = element->GetResolvedStyle();
		    std::string styleCss;
		    styleCss.reserve(256);
		    AppendResolvedStyleCss(element, parentLayoutX, parentLayoutY, &resolved, styleCss);
		    if (!inlineStyle.empty())
		    {
		        if (!styleCss.empty() && styleCss.back() != ';')
		            styleCss.push_back(';');
		        styleCss += inlineStyle;
		    }
		    if (!styleCss.empty())
		    {
		        out += " style=\"";
		        AppendEscapedXml(out, styleCss);
		        out += "\"";
		    }
		
		    // Collect logical children to export. For Mount elements, also include
		    // the mounted target subtree so that portals appear in the exported DOM.
		    std::vector<const GameEngine::UIElement*> exportChildren;
		    const auto& children = element->GetChildren();
		    exportChildren.reserve(children.size() + 1);
		    for (const auto& child : children)
		    {
		        if (child)
		            exportChildren.push_back(child.get());
		    }
		
		    if (const auto* mount = dynamic_cast<const GameEngine::Mount*>(element))
		    {
		        if (auto* target = mount->GetTarget())
		        {
		            exportChildren.push_back(target);
		        }
		    }
		
		    const bool hasChildren = !exportChildren.empty();
		    const std::string textContent = element->GetTextContent();
		    const bool hasText = !textContent.empty();
		    
		    // Always emit explicit end tags instead of self-closing tags. This keeps
		    // the exported XML friendlier to HTML parsers, which treat many elements
		    // (including unknown custom tags) as non-void and do not honor XML-style
		    // self-closing syntax. Self-closing on such elements can lead to
		    // mis-nesting when the markup is viewed as HTML, which in turn breaks
		    // layout/parenting.
		    out += ">";
		    if (hasText)
		    {
		        AppendEscapedXml(out, textContent);
		    }
		    if (hasChildren)
		    {
		        out += "\n";
		        for (const auto* child : exportChildren)
		        {
		            WriteElementXml(child, out, indent + 1, absX, absY);
		        }
		        AppendIndent(out, indent);
		    }
		    out += "</";
		    out += tag;
		    out += ">\n";
		}
}

bool UIManager::DebugExportLayoutAndStyles(const std::string& outDirectory) const
{
	UIElement* root = GetRootElement();
	if (!root)
	    return false;

	std::filesystem::path dirPath(outDirectory.empty() ? std::string("UIExport") : outDirectory);
	std::error_code ec;
	std::filesystem::create_directories(dirPath, ec);

	std::filesystem::path xmlPath = dirPath / "ui_layout_export.xml";

	std::string xml;
	xml.reserve(16 * 1024);
	xml += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
		WriteElementXml(root, xml, 0, 0.0f, 0.0f);

	std::ofstream file(xmlPath, std::ios::out | std::ios::trunc | std::ios::binary);
	if (!file.is_open())
	{
	    Logger::Log::Warning("UIManager: failed to open '{}' for layout export.", xmlPath.string());
	    return false;
	}
	file.write(xml.data(), static_cast<std::streamsize>(xml.size()));
	file.close();

	Logger::Log::Info("UIManager: exported current UI layout to '{}' (debug-only).", xmlPath.string());
	return true;
}

#endif // _DEBUG

std::string UIManager::GetHoveredElementDebugName() const
{
    UIElement* h = (m_HoveredInstanceId != 0) ? FindElementByInstanceId(m_HoveredInstanceId) : nullptr;
    if (!h)
        return std::string();
    const std::string& id = h->GetId();
    if (!id.empty())
        return id;
    return UIAttributeAccess::GetDebugTypeName(*h) + "#" + std::to_string(h->GetInstanceId());
}

UIElement* UIManager::GetHoveredElement() const
{
    return (m_HoveredInstanceId != 0) ? FindElementByInstanceId(m_HoveredInstanceId) : nullptr;
}

std::string UIManager::GetElementSelector(const UIElement* el) const
{
    if (!el)
        return std::string();
    std::string out = UIAttributeAccess::GetDebugTypeName(*el);
    const std::string& id = el->GetId();
    if (!id.empty())
        out += "#" + id;
    for (const std::string& c : el->GetClasses())
        out += "." + c;
    return out;
}

void UIManager::ToggleDebugCapture()
{
    if (m_DebugCaptureEnabled)
    {
        if (m_DebugCaptureOut.is_open())
        {
            m_DebugCaptureOut.flush();
            m_DebugCaptureOut.close();
        }
        Logger::Log::Info("[UI] Debug capture OFF (file='{}')", m_DebugCapturePath.empty() ? "<unknown>" : m_DebugCapturePath);
        m_DebugCaptureEnabled = false;
        m_DebugCapturePath.clear();
        return;
    }

    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif

    std::ostringstream name;
    name << "UI_DebugCapture_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".jsonl";

    std::error_code ec;
    std::filesystem::path outPath = std::filesystem::current_path(ec);
    if (!ec)
        outPath /= name.str();
    else
        outPath = name.str();

    m_DebugCaptureOut.open(outPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!m_DebugCaptureOut.is_open())
    {
        Logger::Log::Warning("[UI] Debug capture failed to open '{}'", outPath.string());
        return;
    }

    m_DebugCaptureEnabled = true;
    m_DebugCapturePath = outPath.string();
    m_DebugCaptureSeq = 0;
    Logger::Log::Info("[UI] Debug capture ON -> {}", m_DebugCapturePath);
}

static constexpr float kPeriodicRefreshIntervalSeconds = 0.5f;

uint64_t UIManager::RegisterPeriodicRefresh(std::function<void()> callback)
{
    if (!callback)
        return 0;
    const uint64_t token = m_NextPeriodicRefreshToken++;
    m_PeriodicRefreshCallbacks.push_back({token, std::move(callback)});
    return token;
}

void UIManager::UnregisterPeriodicRefresh(uint64_t token)
{
    if (token == 0)
        return;
    for (size_t i = 0; i < m_PeriodicRefreshCallbacks.size(); ++i)
    {
        if (m_PeriodicRefreshCallbacks[i].Token == token)
        {
            m_PeriodicRefreshCallbacks.erase(m_PeriodicRefreshCallbacks.begin() +
                                             static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void UIManager::PumpPeriodicRefresh()
{
    if (m_PeriodicRefreshCallbacks.empty())
        return;
    if (m_Time < m_NextPeriodicRefreshTime)
        return;
    m_NextPeriodicRefreshTime = m_Time + kPeriodicRefreshIntervalSeconds;
    // Snapshot: a refresh callback may register/unregister (a panel rebuilding
    // its subtree), so invoke copies rather than iterating the live vector.
    std::vector<std::function<void()>> callbacks;
    callbacks.reserve(m_PeriodicRefreshCallbacks.size());
    for (const PeriodicRefreshEntry& entry : m_PeriodicRefreshCallbacks)
        callbacks.push_back(entry.Callback);
    for (std::function<void()>& cb : callbacks)
        cb();
}

UIManager::UIManager(IDevice* device)
    : UIManager(device, nullptr)
{
}

UIManager::UIManager(IDevice* device, AssetManager* assetManager)
    : m_Device(device), m_AssetManager(assetManager)
{

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    // Respect GE_UI_SUBTREE_SKIP env override. "0" / "false" / "off" (any case)
    // disable the subtree-skip fast path at runtime without touching settings.
    if (const char* e = std::getenv("GE_UI_SUBTREE_SKIP"))
    {
        std::string v(e);
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (v == "0" || v == "false" || v == "off")
            m_SubtreeSkipEnabled = false;
    }
    // IndicesAndClips-rebuild skip A/B knob. Default on; "0" disables.
    if (const char* e = std::getenv("GE_UI_INDICES_CLIPS_SKIP"))
    {
        std::string v(e);
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (v == "0" || v == "false" || v == "off")
            m_IndicesAndClipsSkipEnabled = false;
    }
    // Dirty-source tracing. "1"/"true"/"on" enables per-frame
    // MarkDirty-source attribution into UpdateProfileFrame::dirtySources.
    // Small per-MarkDirty hash insert cost when on; off by default.
    if (const char* e = std::getenv("GE_UI_DIRTY_TRACE"))
    {
        std::string v(e);
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (v == "1" || v == "true" || v == "on")
            m_DirtyTraceEnabled = true;
    }
    // Opt-in per-frame update profiling. Needed for fast-path counter
    // visibility via get_render_stats. Low cost.
    if (const char* e = std::getenv("GE_UI_UPDATE_PROFILE"))
    {
        std::string v(e);
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (v == "1" || v == "true" || v == "on")
            m_UpdateProfilingEnabled = true;
    }
#endif
    m_DragDrop = std::make_unique<UI::Interaction::DragDropManager>();

    m_Scheduler = std::make_unique<Scheduler::DefaultScheduler>();
    UIRegistration::RegisterBuiltInControls();

    if (m_AssetManager)
    {
        m_HotReload = std::make_unique<UIHotReload>(*this, *m_AssetManager);
        InstallAssetReloadCallback();
    }

#if !defined(GE_HAVE_FREETYPE)
    Logger::Log::Warning(
        "UI: Text module was built without FreeType (GE_HAVE_FREETYPE not defined); "
        "UI text will not render in this build. Ensure the 'freetype' package is "
        "installed and CMake finds Freetype::Freetype.");
#endif

#if !defined(GE_HAVE_HARFBUZZ)
    Logger::Log::Warning(
        "UI: Text module was built without HarfBuzz (GE_HAVE_HARFBUZZ not defined); "
        "complex text shaping, ligatures, and RTL layout may be degraded in this "
        "build. Install 'harfbuzz' and ensure CMake finds harfbuzz::harfbuzz if "
        "full shaping is desired.");
#endif

    m_CorrectnessModeEnabled = false;
    if (const char* e = std::getenv("GE_UI_CORRECTNESS_MODE"))
        m_CorrectnessModeEnabled = (e[0] == '1');
    if (const char* e = std::getenv("GE_UI_CORRECTNESS_ASSERTS"))
        m_CorrectnessModeAssertsEnabled = (e[0] == '1');

    {
        const char* e = std::getenv("GE_UI_DISABLE_NOOP_FAST_PATH");
        m_DisableFastPathNoOp = (e && e[0] == '1');
    }

    {
        const char* e = std::getenv("GE_UI_DEBUG_KEYS");
        m_DebugKeysEnabled = (e && e[0] == '1');
    }

    m_DeviceRebuildGeneration = m_Device ? m_Device->GetDeviceRebuildGeneration() : 0;
    CreateDeviceSamplersAndFallbackTexture();

    // The controls' defaults. Their user-agent origin, not their place in the list, puts every
    // theme and document rule over them.
    if (StylesheetHandle defaults = UI::GetDefaultStylesheet())
        AddStylesheet(defaults);
}

void UIManager::CreateDeviceSamplersAndFallbackTexture()
{
    if (!m_Device)
        return;

    {
        auto repeat = Rendering::SamplerDesc::MaterialLinearRepeat("UI_Sampler_Repeat");
        m_SamplerRepeat = m_Device->CreateSampler(repeat);
        auto clamp = repeat;
        clamp.addressModeU = 2;
        clamp.addressModeV = 2;
        clamp.debugName = "UI_Sampler_Clamp";
        m_SamplerClamp = m_Device->CreateSampler(clamp);
        auto repX = repeat;
        repX.addressModeU = 0;
        repX.addressModeV = 2;
        repX.debugName = "UI_Sampler_RepeatX";
        m_SamplerRepeatX = m_Device->CreateSampler(repX);
        auto repY = repeat;
        repY.addressModeU = 2;
        repY.addressModeV = 0;
        repY.debugName = "UI_Sampler_RepeatY";
        m_SamplerRepeatY = m_Device->CreateSampler(repY);
        auto nearestClamp = clamp;
        nearestClamp.minFilter = 0;
        nearestClamp.magFilter = 0;
        nearestClamp.mipFilter = 0;
        nearestClamp.debugName = "UI_Sampler_NearestClamp";
        m_SamplerNearestClamp = m_Device->CreateSampler(nearestClamp);
    }

    {
        TextureDesc td{};
        td.width = 1;
        td.height = 1;
        td.mipLevels = 1;
        td.arrayLayers = 1;
        td.format = (uint32_t)Rendering::TextureFormat::RGBA8_UNORM;
        td.usage = (uint32_t)Rendering::TextureUsage::ShaderResource | (uint32_t)Rendering::TextureUsage::TransferDst;
        td.debugName = "UI_White1x1";
        m_White1x1 = m_Device->CreateTexture(td);
        uint32_t pixel = 0xFFFFFFFFu; // RGBA8 white
        BufferDesc staging{};
        staging.size = sizeof(uint32_t);
        staging.usage = (uint32_t)BufferUsage::TransferSrc;
        staging.memoryUsage = BufferMemoryUsage::Upload;
        staging.debugName = "UI1x1_Staging";
        BufferHandle sb = m_Device->CreateBuffer(staging);
        m_Device->UpdateBuffer(sb, 0, sizeof(uint32_t), &pixel);
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(m_White1x1, ResourceState::Undefined, ResourceState::CopyDest, 0, 1, 0, 1));
        cl->CopyBufferToTextureSubresource(sb, m_White1x1, 0, 0, 1, 1, 0, sizeof(uint32_t));
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(m_White1x1, ResourceState::CopyDest, ResourceState::ShaderResource, 0, 1, 0, 1));
        cl->End();
        m_Device->ExecuteCommandLists(std::vector<CommandList*>{cl.get()});
        m_Device->DestroyBuffer(sb);
    }
}

void UIManager::DetachSurvivingElements()
{
    // Runs after m_Root.reset(). The instance-id index holds exactly the elements whose owner
    // is this manager (registration and unregistration follow every owner change and every
    // destruction, teardown included), so what is left is what outlives it: externally owned
    // Mount targets and their subtrees. Detaching each one runs OnOwnerManagerChanged(nullptr),
    // which may destroy or re-home other survivors, so the loop re-resolves every id through
    // the index instead of trusting a pointer snapshot.
    std::vector<uint64_t> survivorIds;
    survivorIds.reserve(m_ElementsByInstanceId.size());
    for (const auto& entry : m_ElementsByInstanceId)
        survivorIds.push_back(entry.first);

    for (const uint64_t id : survivorIds)
    {
        const auto it = m_ElementsByInstanceId.find(id);
        if (it == m_ElementsByInstanceId.end() || !it->second)
            continue;
        // Detaching a subtree root unregisters its whole subtree, so later ids from that
        // subtree are no longer found above.
        it->second->SetOwnerManager(nullptr);
    }
}

UIManager::~UIManager()
{
    // The manager's tree dies with it, and that is a destruction the subscribers are owed.
    // FIRST, before anything else in this destructor: the registries a managed handler
    // resolves through are still intact here, the manager is not yet marked as being
    // destroyed, and every element is still whole. One line later none of that holds.
    if (m_Root)
        UI::DispatchDestructionDetach(m_Root.get());
    // ...and the elements this manager announced attached that it does NOT own and will
    // NOT destroy — an externally owned Mount target is the ordinary case. They survive,
    // but they lose their root with this manager, and that is a detach they are owed. Done
    // here, while the instance-id index holds and the manager is not yet marked as being
    // destroyed, because a managed handler resolves through both. Nothing is dispatched for
    // elements whose subscribers were never told attached; that is the same edge rule
    // everywhere else applies.
    AnnounceDetachForSurvivingElements();

    // From here on IsBeingDestroyed() is true and every UIManagerRef reads this manager as
    // gone: element destructors running during the rest of this destructor (m_Root.reset
    // below) must never call back into a partially destroyed UIManager.
    m_Lifetime.reset();

    // Stage 5 Block A part 3e: clear InFocusChain bit on every chain
    // entry. Mount targets / portal-style elements may outlive this
    // UIManager (e.g. stack-allocated Button used by tests + ui owns
    // root but not target), and the bits describe membership in vectors
    // that die with this manager. Other queue bits drop with the m_*Dirty
    // vectors below (no destructor-side cleanup needed because we own
    // those vectors). The focus-leaf bits are a subset of the chain, so
    // they are cleared in the same sweep.
    for (UIElement* e : m_FocusWithinChain)
    {
        if (e)
            e->m_InQueueFlags &= ~(UIElement::InFocusChain |
                UIElement::InFocusLeaf | UIElement::InFocusVisibleLeaf);
    }
    m_FocusWithinChain.clear();

    // Same concern for the attach/detach settle state: an element may outlive this
    // manager (an externally owned Mount target), and its ~UIElement gates on the
    // back-pointer cleared here. This also clears the dispatched-state bit on everything
    // this manager owns, so a survivor can be announced again by its next manager.
    ForgetAttachStateOnTeardown();

    // Tear down the element tree while all UIManager state is still alive.
    // Child destructors may call back (e.g. RemoveExternalTexture).
    m_Root.reset();
    DetachSurvivingElements();

    UninstallAssetReloadCallback();
    m_HotReload.reset();

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    ResetRetainedYogaTree();
#endif
    m_AppliesAsyncResults = false;
    if (m_Scheduler)
    {
        m_Scheduler->ProcessDue();
    }
    // Close before the last drain: everything accepted so far still runs, and a post made after
    // this point, by a worker thread still holding an element's route to this manager, is
    // refused rather than queued into a dispatcher nothing will drain again.
    m_Dispatcher->Close();
    m_Dispatcher->Drain();
    if (m_Device)
    {
        if (m_White1x1)
            m_Device->DestroyTexture(m_White1x1);
        if (m_SamplerRepeat)
            m_Device->DestroySampler(m_SamplerRepeat);
        if (m_SamplerClamp)
            m_Device->DestroySampler(m_SamplerClamp);
        if (m_SamplerRepeatX)
            m_Device->DestroySampler(m_SamplerRepeatX);
        if (m_SamplerRepeatY)
            m_Device->DestroySampler(m_SamplerRepeatY);
        if (m_SamplerNearestClamp)
            m_Device->DestroySampler(m_SamplerNearestClamp);
        for (auto& kv : m_BgTextureCache)
        {
            if (kv.second.Handle && !kv.second.SharedOwned)
                m_Device->DestroyTexture(kv.second.Handle);
        }
    }
}

void UIManager::TraceElementDirtySource(UIElement* el, unsigned flags)
{
    // GE_UI_DIRTY_TRACE_ID=<element id>: backtrace the first few marks on that
    // element so per-frame dirty churn is attributable to its caller. Requires
    // GE_UI_DIRTY_TRACE=1 (the caller gates on m_DirtyTraceEnabled).
    static const std::string s_TraceId = []() -> std::string
    {
        const char* e = std::getenv("GE_UI_DIRTY_TRACE_ID");
        return e ? std::string(e) : std::string();
    }();
    if (s_TraceId.empty() || !el || el->GetId() != s_TraceId)
        return;
    // GE_UI_DIRTY_TRACE_SKIP=N skips the first N marks (e.g. the initial
    // asset-bind burst) so the budget captures steady-state callers.
    static int s_Skip = []() -> int
    {
        const char* e = std::getenv("GE_UI_DIRTY_TRACE_SKIP");
        return e ? std::atoi(e) : 0;
    }();
    if (s_Skip > 0)
    {
        --s_Skip;
        return;
    }
    static int s_Budget = 6;
    if (s_Budget <= 0)
        return;
    --s_Budget;
    std::string out;
    for (const auto& f : Logger::CaptureBacktrace(24, /*skipFrames=*/2))
    {
        out += "    ";
        out += f.Symbol.empty() ? "<unknown>" : std::string(f.Symbol.c_str());
        if (!f.File.empty())
        {
            out += " (";
            out += std::string(f.File.c_str());
            out += ":";
            out += std::to_string(f.Line);
            out += ")";
        }
        out += "\n";
    }
    Logger::Log::Warning("[UI DirtyTraceId] id='{}' flags={}\n{}", s_TraceId, flags, out);
}
