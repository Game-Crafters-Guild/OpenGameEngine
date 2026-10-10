#include "Core/DeferredActionQueue.h"

namespace GameEngine
{

bool DeferredActionQueue::Enqueue(Action action)
{
    if (!action)
    {
        return false;
    }

    Entry entry{};
    entry.action = std::move(action);
    m_Entries.emplace_back(std::move(entry));
    return true;
}

bool DeferredActionQueue::EnqueueUnique(const std::string& key, Action action)
{
    if (key.empty())
    {
        return Enqueue(std::move(action));
    }
    if (!action)
    {
        return false;
    }
    if (m_UniqueKeys.find(key) != m_UniqueKeys.end())
    {
        return false;
    }

    m_UniqueKeys.insert(key);
    Entry entry{};
    entry.key = key;
    entry.action = std::move(action);
    m_Entries.emplace_back(std::move(entry));
    return true;
}

void DeferredActionQueue::FlushOnce()
{
    if (m_Entries.empty())
    {
        return;
    }

    std::vector<Entry> batch = std::move(m_Entries);
    m_Entries.clear();

    for (auto& entry : batch)
    {
        if (!entry.key.empty())
        {
            m_UniqueKeys.erase(entry.key);
        }
        if (entry.action)
        {
            entry.action();
        }
    }
}

void DeferredActionQueue::Clear()
{
    m_Entries.clear();
    m_UniqueKeys.clear();
}

} // namespace GameEngine
