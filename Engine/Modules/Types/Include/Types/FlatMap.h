#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace GameEngine
{

// Sorted std::vector<std::pair<K,V>> with binary search. Optimal for <50 entries
// where cache locality matters more than asymptotic complexity.
template <typename K, typename V, typename Compare = std::less<K>>
class FlatMap
{
  public:
    using value_type = std::pair<K, V>;
    using iterator = typename std::vector<value_type>::iterator;
    using const_iterator = typename std::vector<value_type>::const_iterator;

    V& InsertOrAssign(const K& key, V value)
    {
        auto it = LowerBound(key);
        if (it != m_Data.end() && !Compare{}(key, it->first))
        {
            it->second = std::move(value);
            return it->second;
        }
        it = m_Data.insert(it, {key, std::move(value)});
        return it->second;
    }

    // Insert a default-constructed V if key is missing; return reference.
    V& GetOrInsert(const K& key)
    {
        auto it = LowerBound(key);
        if (it != m_Data.end() && !Compare{}(key, it->first))
            return it->second;
        it = m_Data.insert(it, {key, V{}});
        return it->second;
    }

    V* Find(const K& key)
    {
        auto it = LowerBound(key);
        if (it != m_Data.end() && !Compare{}(key, it->first))
            return &it->second;
        return nullptr;
    }

    const V* Find(const K& key) const
    {
        auto it = LowerBound(key);
        if (it != m_Data.end() && !Compare{}(key, it->first))
            return &it->second;
        return nullptr;
    }

    bool Erase(const K& key)
    {
        auto it = LowerBound(key);
        if (it != m_Data.end() && !Compare{}(key, it->first))
        {
            m_Data.erase(it);
            return true;
        }
        return false;
    }

    bool Contains(const K& key) const { return Find(key) != nullptr; }

    size_t Size() const { return m_Data.size(); }
    bool Empty() const { return m_Data.empty(); }
    void Clear() { m_Data.clear(); }
    void Reserve(size_t n) { m_Data.reserve(n); }

    iterator begin() { return m_Data.begin(); }
    iterator end() { return m_Data.end(); }
    const_iterator begin() const { return m_Data.begin(); }
    const_iterator end() const { return m_Data.end(); }

    template <typename Fn>
    void ForEach(Fn&& fn) const
    {
        for (const auto& [k, v] : m_Data)
            fn(k, v);
    }

    // Erases every entry for which `predicate(key, value)` is true, in one pass that keeps the
    // rest in order and allocates nothing. Returns how many were erased.
    template <typename Predicate>
    size_t EraseIf(Predicate&& predicate)
    {
        return std::erase_if(m_Data, [&predicate](value_type& entry) { return predicate(entry.first, entry.second); });
    }

  private:
    std::vector<value_type> m_Data;

    iterator LowerBound(const K& key)
    {
        return std::lower_bound(m_Data.begin(), m_Data.end(), key,
                                [](const value_type& p, const K& k) { return Compare{}(p.first, k); });
    }

    const_iterator LowerBound(const K& key) const
    {
        return std::lower_bound(m_Data.begin(), m_Data.end(), key,
                                [](const value_type& p, const K& k) { return Compare{}(p.first, k); });
    }
};

} // namespace GameEngine
