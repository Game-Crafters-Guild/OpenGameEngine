#pragma once

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace GameEngine
{

class DeferredActionQueue
{
  public:
    using Action = std::function<void()>;

    bool Enqueue(Action action);
    bool EnqueueUnique(const std::string& key, Action action);
    void FlushOnce();
    void Clear();

    bool Empty() const { return m_Entries.empty(); }
    size_t Size() const { return m_Entries.size(); }

  private:
    struct Entry
    {
        std::string key;
        Action action;
    };

    std::vector<Entry> m_Entries;
    std::unordered_set<std::string> m_UniqueKeys;
};

} // namespace GameEngine
