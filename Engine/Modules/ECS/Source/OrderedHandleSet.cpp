#include "ECS/OrderedHandleSet.h"

#include <algorithm>
#include <bit>

namespace GameEngine::ECS
{

namespace
{
bool IndexThenId(EntityHandle a, EntityHandle b)
{
    return a.index != b.index ? a.index < b.index : a.id < b.id;
}
} // namespace

void OrderedHandleSet::OrderUnique(std::vector<EntityHandle>& handles, std::size_t indexBound)
{
    m_ComparisonSorted = 0;
    if (handles.empty())
        return;
    if (m_SeenId.size() < indexBound)
    {
        m_SeenId.resize(indexBound, 0u);
        m_Present.resize((indexBound + 63u) / 64u, 0ull);
    }

    // Pass 1: the first id per index sets its bit and is recorded; anything
    // else that is not a repeat goes to the comparison-sorted remainder. The
    // bitset is all zero on entry (pass 2 clears every word it reads), so a set
    // bit means "seen in this call".
    m_Extra.clear();
    for (const EntityHandle handle : handles)
    {
        const uint32_t index = handle.index;
        if (index >= indexBound)
        {
            m_Extra.push_back(handle);
            continue;
        }
        uint64_t& word = m_Present[index / 64u];
        const uint64_t bit = 1ull << (index % 64u);
        if ((word & bit) == 0ull)
        {
            word |= bit;
            m_SeenId[index] = handle.id;
            continue;
        }
        if (m_SeenId[index] != handle.id)
            m_Extra.push_back(handle);
    }
    std::sort(m_Extra.begin(), m_Extra.end(), IndexThenId);
    m_Extra.erase(std::unique(m_Extra.begin(), m_Extra.end()), m_Extra.end());
    m_ComparisonSorted = m_Extra.size();

    // Pass 2: walk the bitset in index order, clearing it for the next call,
    // and merge the remainder in.
    handles.clear();
    std::size_t extra = 0;
    const std::size_t words = (indexBound + 63u) / 64u;
    for (std::size_t word = 0; word < words; ++word)
    {
        uint64_t bits = m_Present[word];
        if (bits == 0ull)
            continue;
        m_Present[word] = 0ull;
        while (bits != 0ull)
        {
            const std::size_t index = word * 64u + static_cast<std::size_t>(std::countr_zero(bits));
            bits &= bits - 1ull;
            const EntityHandle handle{m_SeenId[index]};
            while (extra < m_Extra.size() && IndexThenId(m_Extra[extra], handle))
                handles.push_back(m_Extra[extra++]);
            handles.push_back(handle);
        }
    }
    handles.insert(handles.end(), m_Extra.begin() + static_cast<std::ptrdiff_t>(extra), m_Extra.end());
}

} // namespace GameEngine::ECS
