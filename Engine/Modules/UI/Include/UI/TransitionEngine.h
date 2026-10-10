#pragma once

#include "UI/UIStyle.h"
#include "Mathematics/Easing.h"

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace GameEngine
{

struct ResolvedStyle;
class UIElement;

class TransitionEngine
{
public:
    struct CompletedTransition
    {
        UIElement* Element;
        StylePropertyId Property;
    };

    // `declared` is UIManager's registry of elements whose ResolvedStyle
    // currently declares transitions (mirrored at cascade time in
    // ResolveCascadeForElement). Iterating it replaces the old full-tree
    // collect walk — typically 0..few entries vs N nodes per heavy frame.
    void Advance(UIElement* root, const std::unordered_set<UIElement*>& declared, float currentTime);
    bool HasActiveTransitions() const { return m_ActiveCount > 0; }
    const std::vector<UIElement*>& GetActiveLayoutElements() const { return m_ActiveLayoutElements; }
    const std::vector<CompletedTransition>& GetCompletedTransitions() const { return m_CompletedThisFrame; }
    void Clear();

private:
    struct TransitionSlot
    {
        uint64_t InstanceId;
        StylePropertyId Property;
        float StartTime;
        float DurationSec;
        float DelaySec;
        Math::EasingFunction Easing;
        StyleValue StartValue;
        StyleValue TargetValue;
        bool AffectsLayout;
        UIElement* Element = nullptr;
    };

    struct SnapshotEntry
    {
        uint64_t InstanceId;
        StylePropertyId Property;
        StyleValue Value;
    };

    std::vector<TransitionSlot> m_Slots;
    size_t m_ActiveCount = 0;
    std::vector<SnapshotEntry> m_Snapshots;
    std::vector<std::pair<uint64_t, UIElement*>> m_TransitionElements;
    std::vector<UIElement*> m_ActiveLayoutElements;
    std::vector<CompletedTransition> m_CompletedThisFrame;

    void CollectTransitionElements(UIElement* root, const std::unordered_set<UIElement*>& declared);
    void DetectChangesAndStart(UIElement* el, float currentTime);
    void AdvanceSlots(float currentTime);
    void TakeSnapshots();
    void RemoveCompletedSlots(float currentTime);
    void RebuildActiveLayoutElements();

    SnapshotEntry* FindSnapshot(uint64_t instanceId, StylePropertyId prop);
    TransitionSlot* FindSlot(uint64_t instanceId, StylePropertyId prop);
};

} // namespace GameEngine
