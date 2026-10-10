#include "UI/Controls/Mount.h"

#include <unordered_map>
#include <vector>

using namespace GameEngine;

namespace {

// Reverse map: target UIElement → live Mounts whose m_Target == that element.
// At steady state each target has at most one Mount; SetTarget/Swap enforce
// exclusivity so primitive generation never visits the same subtree twice.
//
// Held inside a local singleton to avoid exposing the std::unordered_multimap
// in a public header.
struct Storage
{
    std::unordered_multimap<UIElement*, Mount*> byTarget;

    static Storage& Get()
    {
        static Storage s;
        return s;
    }
};

} // namespace

MountRegistry& MountRegistry::Instance()
{
    static MountRegistry s;
    return s;
}

void MountRegistry::OnTargetChanged(Mount* mount, UIElement* oldTarget, UIElement* newTarget)
{
    if (!mount)
        return;
    if (oldTarget == newTarget)
        return;

    auto& m = Storage::Get().byTarget;

    if (oldTarget)
    {
        // Erase the (oldTarget, mount) entry. Range is small (usually 1), so a
        // linear scan in the equal-range is fine.
        auto range = m.equal_range(oldTarget);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (it->second == mount)
            {
                m.erase(it);
                break;
            }
        }
    }

    if (newTarget)
    {
        m.emplace(newTarget, mount);
    }
}

void MountRegistry::OnElementDestroyed(UIElement* element)
{
    if (!element)
        return;

    auto& m = Storage::Get().byTarget;
    auto range = m.equal_range(element);
    if (range.first == range.second)
        return;

    // Collect matched Mounts first — we're about to erase entries and can't
    // iterate the multimap while mutating it.
    // The set is typically of size 1.
    std::vector<Mount*> matched;
    matched.reserve(4);
    for (auto it = range.first; it != range.second; ++it)
        matched.push_back(it->second);

    m.erase(element);

    for (Mount* mount : matched)
    {
        if (!mount) continue;
        // Set the target to nullptr directly (not via SetTarget, which would
        // try to re-register / re-notify). The element is mid-destruction.
        mount->SetTargetToNullDueToDestruction();
    }
}

void MountRegistry::NotifyTargetOverlayBitChanged(UIElement* target)
{
    if (!target)
        return;
    auto& m = Storage::Get().byTarget;
    auto range = m.equal_range(target);
    if (range.first == range.second)
        return;
    // Snapshot first — RefreshOverlaySubtreeBitAndPropagate may walk back
    // into MountRegistry (transitively, via another target's bit flip)
    // and we cannot iterate the multimap while it mutates.
    std::vector<Mount*> hosts;
    hosts.reserve(2);
    for (auto it = range.first; it != range.second; ++it)
        hosts.push_back(it->second);
    for (Mount* mount : hosts)
    {
        if (mount)
            mount->RefreshOverlaySubtreeBitAndPropagate();
    }
}

size_t MountRegistry::Size() const
{
    return Storage::Get().byTarget.size();
}

void MountRegistry::DetachTargetFromOtherMountsExcept(UIElement* target, Mount* keeper)
{
    if (!target || !keeper)
        return;

    auto& m = Storage::Get().byTarget;
    std::vector<Mount*> others;
    auto range = m.equal_range(target);
    others.reserve(4);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second && it->second != keeper)
            others.push_back(it->second);
    }
    for (Mount* o : others)
    {
        if (o)
            o->SetTarget(nullptr);
    }
}
