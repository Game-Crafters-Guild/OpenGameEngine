#include "ShaderGraph/GraphPreviewAtlasLayout.h"

#include <algorithm>
#include <unordered_set>

namespace GameEngine::Editor
{

bool GraphPreviewAtlasLayout::Reconcile(const std::vector<std::string>& keys)
{
    bool changed = false;

    std::unordered_set<std::string> wanted(keys.begin(), keys.end());
    for (auto it = m_CellOfKey.begin(); it != m_CellOfKey.end();)
    {
        if (wanted.count(it->first) != 0)
        {
            ++it;
            continue;
        }
        m_FreeCells.push_back(it->second);
        it = m_CellOfKey.erase(it);
        changed = true;
    }

    // Lowest free cell first: a graph that churns keys keeps reusing the top of
    // the atlas instead of walking rows it then has to keep allocated.
    std::sort(m_FreeCells.begin(), m_FreeCells.end(), std::greater<uint32_t>());

    for (const std::string& key : keys)
    {
        if (m_CellOfKey.count(key) != 0)
            continue;
        uint32_t cell;
        if (!m_FreeCells.empty())
        {
            cell = m_FreeCells.back();
            m_FreeCells.pop_back();
        }
        else
        {
            cell = m_NextCell++;
        }
        m_CellOfKey.emplace(key, cell);
        changed = true;
    }

    const uint32_t neededCells = m_NextCell;
    uint32_t rows = 0;
    while (rows * kColumns < neededCells)
        rows += kRowsPerPage;
    if (rows != m_Rows)
    {
        m_Rows = rows;
        changed = true;
    }

    return changed;
}

bool GraphPreviewAtlasLayout::TryGetCell(const std::string& key, uint32_t& outCell) const
{
    const auto it = m_CellOfKey.find(key);
    if (it == m_CellOfKey.end())
        return false;
    outCell = it->second;
    return true;
}

void GraphPreviewAtlasLayout::CellOrigin(uint32_t cell, uint32_t& outX, uint32_t& outY) const
{
    outX = (cell % kColumns) * kCellPx;
    outY = (cell / kColumns) * kCellPx;
}

GraphPreviewAtlasLayout::CellRect GraphPreviewAtlasLayout::BackgroundRect(uint32_t cell,
                                                                         uint32_t columns,
                                                                         uint32_t rows)
{
    CellRect rect{};
    if (columns == 0 || rows == 0)
        return rect;

    rect.SizeXPercent = static_cast<float>(columns) * 100.f;
    rect.SizeYPercent = static_cast<float>(rows) * 100.f;

    const uint32_t col = cell % columns;
    const uint32_t row = (cell / columns) % rows;
    rect.PosXPercent =
        (columns > 1) ? (static_cast<float>(col) / static_cast<float>(columns - 1)) * 100.f : 0.f;
    rect.PosYPercent =
        (rows > 1) ? (static_cast<float>(row) / static_cast<float>(rows - 1)) * 100.f : 0.f;
    return rect;
}

} // namespace GameEngine::Editor
