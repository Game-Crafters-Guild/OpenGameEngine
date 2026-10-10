#include "VulkanSharedInstance.h"

#include <algorithm>
#include <cassert>

namespace GameEngine
{
namespace Rendering
{

std::optional<SharedInstanceState> SharedInstanceRegistry::Acquire(const SharedInstanceKey& key)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (Entry& entry : m_Entries)
    {
        if (entry.Key == key)
        {
            ++entry.RefCount;
            return entry.State;
        }
    }
    return std::nullopt;
}

void SharedInstanceRegistry::Publish(const SharedInstanceKey& key, const SharedInstanceState& state)
{
    assert(state.Instance != VK_NULL_HANDLE && "publish a created instance, never a null handle");
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Entries.push_back(Entry{key, state, /*RefCount=*/1u});
}

std::optional<SharedInstanceState> SharedInstanceRegistry::Release(VkInstance instance)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto entry = std::find_if(m_Entries.begin(),
                                    m_Entries.end(),
                                    [instance](const Entry& e) { return e.State.Instance == instance; });
    if (entry == m_Entries.end())
        return std::nullopt;

    assert(entry->RefCount > 0 && "a registered entry always holds at least one reference");
    if (--entry->RefCount > 0)
        return std::nullopt;

    const SharedInstanceState state = entry->State;
    m_Entries.erase(entry);
    return state;
}

SharedInstanceRegistry& SharedInstanceRegistry::Get()
{
    // Deliberately never destroyed. Hosts and test harnesses keep a device behind
    // a leaked singleton of their own, so a device can be torn down during static
    // destruction — and a registry destroyed before it would be read by that
    // teardown's Release. Same reason EngineCore::GetInstance leaks its default.
    static SharedInstanceRegistry* registry = new SharedInstanceRegistry();
    return *registry;
}

} // namespace Rendering
} // namespace GameEngine
