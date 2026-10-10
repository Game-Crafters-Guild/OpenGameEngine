#include "UI/TransitionEngine.h"

#include "Mathematics/Interpolation.h"
#include "StyleApplier.h"
#include "StyleInterpolation.h"
#include "UI/ResolvedStyle.h"
#include "UI/TransitionSpec.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <cmath>

namespace GameEngine
{

namespace
{

// Leaf interpolable properties for "transition: all". Excludes compound aggregates
// (BorderWidth, BorderRadius, BorderColor) that overlap with per-edge variants.
static constexpr StylePropertyId kLeafInterpolableProperties[] = {
    StylePropertyId::Opacity,
    StylePropertyId::FontSize,
    StylePropertyId::LineHeight,
    StylePropertyId::LetterSpacing,
    StylePropertyId::FlexGrow,
    StylePropertyId::FlexShrink,
    StylePropertyId::AspectRatio,
    StylePropertyId::Gap,
    StylePropertyId::RowGap,
    StylePropertyId::ColumnGap,
    StylePropertyId::BackgroundColor,
    StylePropertyId::Color,
    StylePropertyId::BackgroundTint,
    StylePropertyId::BorderTopColor,
    StylePropertyId::BorderRightColor,
    StylePropertyId::BorderBottomColor,
    StylePropertyId::BorderLeftColor,
    StylePropertyId::Width,
    StylePropertyId::Height,
    StylePropertyId::MinWidth,
    StylePropertyId::MinHeight,
    StylePropertyId::MaxWidth,
    StylePropertyId::MaxHeight,
    StylePropertyId::FlexBasis,
    StylePropertyId::PositionLeft,
    StylePropertyId::PositionTop,
    StylePropertyId::PositionRight,
    StylePropertyId::PositionBottom,
    StylePropertyId::MarginTop,
    StylePropertyId::MarginRight,
    StylePropertyId::MarginBottom,
    StylePropertyId::MarginLeft,
    StylePropertyId::PaddingTop,
    StylePropertyId::PaddingRight,
    StylePropertyId::PaddingBottom,
    StylePropertyId::PaddingLeft,
    StylePropertyId::BorderTopWidth,
    StylePropertyId::BorderRightWidth,
    StylePropertyId::BorderBottomWidth,
    StylePropertyId::BorderLeftWidth,
    StylePropertyId::BorderTopLeftRadius,
    StylePropertyId::BorderTopRightRadius,
    StylePropertyId::BorderBottomRightRadius,
    StylePropertyId::BorderBottomLeftRadius,
    StylePropertyId::BoxShadow,
    StylePropertyId::Glow,
};

bool StyleValuesEqual(const StyleValue& a, const StyleValue& b)
{
    if (a.index() != b.index())
        return false;
    if (std::holds_alternative<std::monostate>(a))
        return true;
    if (std::holds_alternative<DisplayMode>(a))
        return std::get<DisplayMode>(a) == std::get<DisplayMode>(b);
    if (std::holds_alternative<float>(a))
        return std::get<float>(a) == std::get<float>(b);
    if (std::holds_alternative<uint32_t>(a))
        return std::get<uint32_t>(a) == std::get<uint32_t>(b);
    if (std::holds_alternative<StyleLength>(a))
        return std::get<StyleLength>(a) == std::get<StyleLength>(b);
    if (std::holds_alternative<Box4>(a))
        return std::get<Box4>(a) == std::get<Box4>(b);
    if (std::holds_alternative<CornerRadiiTLTRBRBL>(a))
        return std::get<CornerRadiiTLTRBRBL>(a) == std::get<CornerRadiiTLTRBRBL>(b);
    if (std::holds_alternative<CornerRadiusValue>(a))
        return std::get<CornerRadiusValue>(a) == std::get<CornerRadiusValue>(b);
    if (std::holds_alternative<BorderColorsTRBL>(a))
    {
        const auto& ca = std::get<BorderColorsTRBL>(a);
        const auto& cb = std::get<BorderColorsTRBL>(b);
        return ca.Top == cb.Top && ca.Right == cb.Right
            && ca.Bottom == cb.Bottom && ca.Left == cb.Left;
    }
    if (std::holds_alternative<BoxShadowValue>(a))
        return std::get<BoxShadowValue>(a) == std::get<BoxShadowValue>(b);
    if (std::holds_alternative<GlowValue>(a))
        return std::get<GlowValue>(a) == std::get<GlowValue>(b);
    return false;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void TransitionEngine::Advance(UIElement* root, const std::unordered_set<UIElement*>& declared, float currentTime)
{
    if (!root)
    {
        Clear();
        return;
    }

    CollectTransitionElements(root, declared);

    // Sort by InstanceId for binary search during slot validation.
    std::sort(m_TransitionElements.begin(), m_TransitionElements.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Validate existing slots: discard any whose element is no longer alive.
    {
        size_t writeIdx = 0;
        for (size_t i = 0; i < m_Slots.size(); ++i)
        {
            auto it = std::lower_bound(
                m_TransitionElements.begin(), m_TransitionElements.end(),
                m_Slots[i].InstanceId,
                [](const auto& p, uint64_t id) { return p.first < id; });

            if (it != m_TransitionElements.end() && it->first == m_Slots[i].InstanceId)
            {
                m_Slots[i].Element = it->second;
                if (writeIdx != i)
                    m_Slots[writeIdx] = std::move(m_Slots[i]);
                ++writeIdx;
            }
        }
        m_Slots.resize(writeIdx);
    }

    // Detect cascade changes and start / retarget transitions.
    for (auto& [id, el] : m_TransitionElements)
        DetectChangesAndStart(el, currentTime);

    // Snapshot cascade targets BEFORE interpolation so next frame's change
    // detection compares cascade-to-cascade (not cascade-to-interpolated).
    TakeSnapshots();

    AdvanceSlots(currentTime);
    RemoveCompletedSlots(currentTime);
    RebuildActiveLayoutElements();

    m_ActiveCount = m_Slots.size();
}

void TransitionEngine::Clear()
{
    m_Slots.clear();
    m_ActiveCount = 0;
    m_Snapshots.clear();
    m_TransitionElements.clear();
    m_ActiveLayoutElements.clear();
    m_CompletedThisFrame.clear();
}

// ---------------------------------------------------------------------------
// Tree collection
// ---------------------------------------------------------------------------

void TransitionEngine::CollectTransitionElements(UIElement* root,
                                                 const std::unordered_set<UIElement*>& declared)
{
    m_TransitionElements.clear();
    if (declared.empty())
        return;

    // Pre-collect IDs with active display transition slots (typically 0-2).
    static thread_local std::vector<uint64_t> displayHoldIds;
    displayHoldIds.clear();
    for (const auto& slot : m_Slots)
    {
        if (slot.Property == StylePropertyId::Display)
            displayHoldIds.push_back(slot.InstanceId);
    }

    // "Held": a display:none element kept alive by an active (or
    // just-starting) display transition, so its fade-out can play and
    // descendant transitions keep running during the hold.
    const auto isHeld = [&](UIElement* el, const ResolvedStyle& style) {
        const uint64_t id = el->GetInstanceId();
        if (std::find(displayHoldIds.begin(), displayHoldIds.end(), id) != displayHoldIds.end())
            return true;
        // First frame: display just changed to none, spec covers display.
        if (!style.Transitions.IsEmpty() && style.Transitions.Find(StylePropertyId::Display))
        {
            SnapshotEntry* snap = FindSnapshot(id, StylePropertyId::Display);
            if (snap && std::holds_alternative<DisplayMode>(snap->Value)
                && std::get<DisplayMode>(snap->Value) != DisplayMode::None)
            {
                return true;
            }
        }
        return false;
    };

    for (UIElement* el : declared)
    {
        const ResolvedStyle& style = el->GetResolvedStyle();
        // Registry lag guard: the mirror is written at cascade time, so an
        // entry whose latest cascade cleared its spec shouldn't appear —
        // but a stale entry must not start transitions.
        if (style.Transitions.IsEmpty())
            continue;

        // The old tree walk pruned display:none subtrees (unless held) and,
        // being a downward walk from root, could only ever reach ATTACHED
        // elements. Reproduce both per entry with a DFS-ancestor scan: skip
        // entries hidden by an unheld display:none ancestor, and entries
        // whose chain does not terminate at root (detached-but-owned Mount
        // targets — inactive dock tabs keep their owner, so they stay
        // registered; their transitions must not advance while unmounted).
        bool skip = false;
        UIElement* top = el;
        for (UIElement* p = el; p; p = p->GetDfsParent())
        {
            top = p;
            const ResolvedStyle& ps = p->GetResolvedStyle();
            if (ps.Layout.DisplayMode == DisplayMode::None && !isHeld(p, ps))
            {
                skip = true;
                break;
            }
        }
        if (skip || top != root)
            continue;

        m_TransitionElements.push_back({el->GetInstanceId(), el});
    }
}

// ---------------------------------------------------------------------------
// Change detection
// ---------------------------------------------------------------------------

void TransitionEngine::DetectChangesAndStart(UIElement* el, float currentTime)
{
    const uint64_t id = el->GetInstanceId();
    const ResolvedStyle& style = el->GetResolvedStyle();
    const TransitionSpec& spec = style.Transitions;
    if (spec.IsEmpty())
        return;

    bool hasAll = false;
    spec.ForEach([&](const TransitionEntry& e) {
        if (e.Property == kTransitionAll)
            hasAll = true;
    });

    auto checkProperty = [&](StylePropertyId propId, const TransitionEntry& entry) {
        StyleValue cascadeTarget = ReadProperty(style, propId);
        if (std::holds_alternative<std::monostate>(cascadeTarget))
            return;

        SnapshotEntry* snap = FindSnapshot(id, propId);
        if (!snap)
            return; // first frame — will be snapshotted after this pass

        if (StyleValuesEqual(snap->Value, cascadeTarget))
            return;

        TransitionSlot* existing = FindSlot(id, propId);
        if (existing)
        {
            // Retarget: compute current interpolated value as new start.
            float elapsed = currentTime - existing->StartTime;
            float progress = 0.0f;
            if (elapsed >= existing->DelaySec)
            {
                progress = Math::Clamp01(
                    (elapsed - existing->DelaySec)
                    / std::max(existing->DurationSec, 0.001f));
            }
            float easedT = Math::EvalEasing(existing->Easing, progress);
            StyleValue interpolated = InterpolateProperty(
                propId, existing->StartValue, existing->TargetValue, easedT);

            existing->StartValue = std::move(interpolated);
            existing->TargetValue = cascadeTarget;
            existing->StartTime = currentTime;
            existing->DurationSec = entry.DurationSec;
            existing->DelaySec = entry.DelaySec;
            existing->Easing = entry.Easing;
        }
        else
        {
            TransitionSlot slot{};
            slot.InstanceId = id;
            slot.Property = propId;
            slot.StartTime = currentTime;
            slot.DurationSec = entry.DurationSec;
            slot.DelaySec = entry.DelaySec;
            slot.Easing = entry.Easing;
            slot.StartValue = snap->Value;
            slot.TargetValue = std::move(cascadeTarget);
            // Canonical classification (GetStylePropertyImpact): a
            // transition-local layout list here had drifted — border widths
            // and line-height animated without relayouting, snapping at the
            // end of the transition.
            slot.AffectsLayout = GetStylePropertyImpact(propId).Layout;
            slot.Element = el;
            auto insertPos = std::lower_bound(
                m_Slots.begin(), m_Slots.end(), slot,
                [](const TransitionSlot& a, const TransitionSlot& b) {
                    if (a.InstanceId != b.InstanceId) return a.InstanceId < b.InstanceId;
                    return static_cast<uint16_t>(a.Property) < static_cast<uint16_t>(b.Property);
                });
            m_Slots.insert(insertPos, std::move(slot));
        }
    };

    if (hasAll)
    {
        for (StylePropertyId propId : kLeafInterpolableProperties)
        {
            const TransitionEntry* entry = spec.Find(propId);
            if (entry)
                checkProperty(propId, *entry);
        }
    }
    else
    {
        spec.ForEach([&](const TransitionEntry& entry) {
            if (IsTransitionable(entry.Property))
                checkProperty(entry.Property, entry);
        });
    }
}

// ---------------------------------------------------------------------------
// Interpolation and write-back
// ---------------------------------------------------------------------------

void TransitionEngine::AdvanceSlots(float currentTime)
{
    for (auto& slot : m_Slots)
    {
        if (!slot.Element)
            continue;

        float elapsed = currentTime - slot.StartTime;
        if (elapsed < slot.DelaySec)
        {
            ResolvedStyle& rs = slot.Element->GetMutableResolvedStyle();
            WriteProperty(rs, slot.Property, slot.StartValue);
            // Write-back bypasses the cascade, so the used-value rules that
            // FinalizeElementStyle applies have to be re-applied here: an
            // animated border-width still computes to 0 under border-style:none.
            ResolveUsedBorderWidths(rs);
            slot.Element->MarkDirty(UIElement::StyleDirty);
            if (slot.AffectsLayout)
                slot.Element->MarkDirty(UIElement::LayoutDirty);
            continue;
        }

        float progress = Math::Clamp01(
            (elapsed - slot.DelaySec) / std::max(slot.DurationSec, 0.001f));
        float easedT = Math::EvalEasing(slot.Easing, progress);

        StyleValue interpolated = InterpolateProperty(
            slot.Property, slot.StartValue, slot.TargetValue, easedT);

        ResolvedStyle& rs = slot.Element->GetMutableResolvedStyle();
        WriteProperty(rs, slot.Property, interpolated);
        ResolveUsedBorderWidths(rs);

        // Interpolation overwrites ResolvedStyle with intermediate values.
        // Force the cascade to re-run next frame so DetectChangesAndStart
        // reads clean cascade targets, not stale interpolated ones.
        slot.Element->MarkDirty(UIElement::StyleDirty);

        if (slot.AffectsLayout)
            slot.Element->MarkDirty(UIElement::LayoutDirty);
    }
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

void TransitionEngine::RemoveCompletedSlots(float currentTime)
{
    m_CompletedThisFrame.clear();
    auto it = std::remove_if(m_Slots.begin(), m_Slots.end(),
        [&](const TransitionSlot& slot) {
            float elapsed = currentTime - slot.StartTime;
            float raw = (elapsed - slot.DelaySec) / std::max(slot.DurationSec, 0.001f);
            if (raw >= 1.0f)
            {
                if (slot.Element)
                    m_CompletedThisFrame.push_back({slot.Element, slot.Property});
                return true;
            }
            return false;
        });
    m_Slots.erase(it, m_Slots.end());
}

// Cached list of elements with at least one layout-affecting slot, consumed
// once per Update by the post-transition Yoga re-solve. Building it here (vs.
// walking the tree for LayoutDirty marks) keeps that pass O(active slots)
// instead of O(tree). clear() retains the vector's capacity, so once the
// working set is warm this reuses its buffer with no per-frame allocation.
//
// The consecutive-InstanceId dedup is correct because m_Slots is kept sorted
// by (InstanceId, Property): inserts go through lower_bound and RemoveCompleted
// uses a stable remove_if, so every element's slots are contiguous. An element
// with several layout slots therefore appears once, and the re-solve never
// re-applies the same node twice in a frame.
void TransitionEngine::RebuildActiveLayoutElements()
{
    m_ActiveLayoutElements.clear();
    uint64_t previousInstanceId = 0;
    for (const auto& slot : m_Slots)
    {
        if (!slot.AffectsLayout || !slot.Element)
            continue;
        if (m_ActiveLayoutElements.empty() || slot.InstanceId != previousInstanceId)
        {
            m_ActiveLayoutElements.push_back(slot.Element);
            previousInstanceId = slot.InstanceId;
        }
    }
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

void TransitionEngine::TakeSnapshots()
{
    m_Snapshots.clear();

    for (const auto& [id, el] : m_TransitionElements)
    {
        const ResolvedStyle& style = el->GetResolvedStyle();
        const TransitionSpec& spec = style.Transitions;

        bool hasAll = false;
        spec.ForEach([&](const TransitionEntry& e) {
            if (e.Property == kTransitionAll)
                hasAll = true;
        });

        if (hasAll)
        {
            for (StylePropertyId propId : kLeafInterpolableProperties)
            {
                StyleValue val = ReadProperty(style, propId);
                if (!std::holds_alternative<std::monostate>(val))
                    m_Snapshots.push_back({id, propId, std::move(val)});
            }
        }
        else
        {
            spec.ForEach([&](const TransitionEntry& entry) {
                if (IsTransitionable(entry.Property))
                {
                    StyleValue val = ReadProperty(style, entry.Property);
                    if (!std::holds_alternative<std::monostate>(val))
                        m_Snapshots.push_back({id, entry.Property, std::move(val)});
                }
            });
        }
    }

    std::sort(m_Snapshots.begin(), m_Snapshots.end(),
              [](const SnapshotEntry& a, const SnapshotEntry& b) {
                  if (a.InstanceId != b.InstanceId)
                      return a.InstanceId < b.InstanceId;
                  return static_cast<uint16_t>(a.Property)
                       < static_cast<uint16_t>(b.Property);
              });
}

// ---------------------------------------------------------------------------
// Lookup helpers
// ---------------------------------------------------------------------------

TransitionEngine::SnapshotEntry* TransitionEngine::FindSnapshot(
    uint64_t instanceId, StylePropertyId prop)
{
    struct Key
    {
        uint64_t id;
        uint16_t prop;
    };
    Key key{instanceId, static_cast<uint16_t>(prop)};

    auto it = std::lower_bound(
        m_Snapshots.begin(), m_Snapshots.end(), key,
        [](const SnapshotEntry& entry, const Key& k) {
            if (entry.InstanceId != k.id)
                return entry.InstanceId < k.id;
            return static_cast<uint16_t>(entry.Property) < k.prop;
        });

    if (it != m_Snapshots.end()
        && it->InstanceId == instanceId && it->Property == prop)
        return &(*it);
    return nullptr;
}

TransitionEngine::TransitionSlot* TransitionEngine::FindSlot(
    uint64_t instanceId, StylePropertyId prop)
{
    // m_Slots is kept sorted by (InstanceId, Property) after each Advance pass.
    struct Key { uint64_t Id; uint16_t Prop; };
    Key key{instanceId, static_cast<uint16_t>(prop)};
    auto it = std::lower_bound(
        m_Slots.begin(), m_Slots.end(), key,
        [](const TransitionSlot& slot, const Key& k) {
            if (slot.InstanceId != k.Id) return slot.InstanceId < k.Id;
            return static_cast<uint16_t>(slot.Property) < k.Prop;
        });
    if (it != m_Slots.end() && it->InstanceId == instanceId && it->Property == prop)
        return &(*it);
    return nullptr;
}

} // namespace GameEngine
