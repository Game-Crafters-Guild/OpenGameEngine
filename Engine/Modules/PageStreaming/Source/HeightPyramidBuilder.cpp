#include "HeightPyramidBuilder.h"

#include "PageStreaming/HeightPageCodec.h"
#include "PageStreaming/PageStoreWriter.h"

#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace GameEngine::PageStreaming
{
namespace
{

uint32 ClampIndex(int64 index, uint32 count)
{
    return static_cast<uint32>(std::clamp<int64>(index, 0, static_cast<int64>(count) - 1));
}

// The tent across a row at coarser sample `i`: [1 2 1] / 4 over the finer samples 2i - 1, 2i, 2i + 1.
float32 TentAcross(const float32* row, uint32 i, uint32 width)
{
    const int64 center = 2 * static_cast<int64>(i);
    return 0.25f * row[ClampIndex(center - 1, width)] + 0.5f * row[ClampIndex(center, width)] +
           0.25f * row[ClampIndex(center + 1, width)];
}

} // namespace

HeightPyramidBuilder::HeightPyramidBuilder(PageStoreWriter& writer, bool patching,
                                           JobSystem::WorkStealingThreadPool* pool)
    : m_Writer(writer)
    , m_Patching(patching)
    , m_Pool(pool)
{
    for (const PageStoreLevel& shape : writer.Layout().Levels)
    {
        LevelState state;
        state.Shape = shape;
        const std::size_t pages = static_cast<std::size_t>(shape.PagesX) * shape.PagesZ;
        state.PageMin.assign(pages, std::numeric_limits<float32>::max());
        state.PageMax.assign(pages, std::numeric_limits<float32>::lowest());
        m_Levels.push_back(std::move(state));
    }
}

void HeightPyramidBuilder::PushLevel0Rows(const float32* rows, uint32 rowCount)
{
    LevelState& level0 = m_Levels.front();
    const uint32 width = level0.Shape.SamplesX;
    assert(level0.RowsReceived + rowCount <= level0.Shape.SamplesZ);
    const std::size_t firstNew = level0.Rows.size();
    for (uint32 r = 0; r < rowCount; ++r)
        level0.Rows.emplace_back(rows + static_cast<std::size_t>(r) * width, rows + static_cast<std::size_t>(r + 1) * width);
    RunParallel(rowCount, [&](uint32 r) { Quantize(level0.Rows[firstNew + r].data(), width); });
    level0.RowsReceived += rowCount;
    Advance(0);
}

bool HeightPyramidBuilder::Finish() const
{
    if (m_Failed.load())
        return false;
    for (const LevelState& level : m_Levels)
        if (level.RowsReceived != level.Shape.SamplesZ || level.NextPageRow != level.Shape.PagesZ)
            return false;
    return true;
}

const float32* HeightPyramidBuilder::Row(const LevelState& level, int64 row) const
{
    const uint32 clamped = ClampIndex(row, level.Shape.SamplesZ);
    assert(clamped >= level.FirstKeptRow && clamped < level.RowsReceived);
    return level.Rows[clamped - level.FirstKeptRow].data();
}

void HeightPyramidBuilder::Advance(uint32 levelIndex)
{
    LevelState& level = m_Levels[levelIndex];
    const uint32 lastRow = level.Shape.SamplesZ - 1u;
    // A page row is complete once its last row (the far apron, or the field's last row) is in.
    while (level.NextPageRow < level.Shape.PagesZ)
    {
        const uint32 needed = std::min(level.NextPageRow * kPageOwnedSamples + kPageOwnedSamples, lastRow);
        if (level.RowsReceived <= needed)
            break;
        EmitPageRow(levelIndex, level.NextPageRow++);
    }
    if (levelIndex + 1u < m_Levels.size())
    {
        // A coarser row j reads rows 2j - 1 to 2j + 1 of this level, clamped to the field.
        const uint32 coarserRows = m_Levels[levelIndex + 1u].Shape.SamplesZ;
        uint32 end = level.NextChildRow;
        while (end < coarserRows && std::min(2u * end + 1u, lastRow) < level.RowsReceived)
            ++end;
        if (end > level.NextChildRow)
        {
            const uint32 first = level.NextChildRow;
            MakeCoarserRows(levelIndex, first, end - first);
            level.NextChildRow = end;
            Advance(levelIndex + 1u);
        }
    }
    Discard(levelIndex);
}

void HeightPyramidBuilder::EmitPageRow(uint32 levelIndex, uint32 pageRow)
{
    RunParallel(m_Levels[levelIndex].Shape.PagesX, [&](uint32 pageX) { EmitPage(levelIndex, pageX, pageRow); });
}

void HeightPyramidBuilder::EmitPage(uint32 levelIndex, uint32 pageX, uint32 pageZ)
{
    LevelState& level = m_Levels[levelIndex];
    const uint32 width = level.Shape.SamplesX;
    std::vector<float32> samples(kPageSampleCount);
    const int64 firstX = static_cast<int64>(pageX) * kPageOwnedSamples - kPageApronSamples;
    const int64 firstZ = static_cast<int64>(pageZ) * kPageOwnedSamples - kPageApronSamples;
    for (uint32 r = 0; r < kPageStrideSamples; ++r)
    {
        const float32* source = Row(level, firstZ + r);
        float32* out = samples.data() + static_cast<std::size_t>(r) * kPageStrideSamples;
        for (uint32 c = 0; c < kPageStrideSamples; ++c)
            out[c] = source[ClampIndex(firstX + c, width)];
    }

    // The footprint: the owned samples and the shared edge after them (stride indices 1 to 129).
    float32 lowest = std::numeric_limits<float32>::max();
    float32 highest = std::numeric_limits<float32>::lowest();
    for (uint32 r = kPageApronSamples; r < kPageStrideSamples; ++r)
    {
        for (uint32 c = kPageApronSamples; c < kPageStrideSamples; ++c)
        {
            const float32 value = samples[static_cast<std::size_t>(r) * kPageStrideSamples + c];
            lowest = std::min(lowest, value);
            highest = std::max(highest, value);
        }
    }
    if (levelIndex > 0)
    {
        // The level-0 truth under the footprint: the finer pages it covers (clamped to the finer
        // level, whose last page also holds the samples past the field's edge).
        const LevelState& finer = m_Levels[levelIndex - 1u];
        for (uint32 dz = 0; dz < 2u; ++dz)
        {
            for (uint32 dx = 0; dx < 2u; ++dx)
            {
                const uint32 childX = std::min(2u * pageX + dx, finer.Shape.PagesX - 1u);
                const uint32 childZ = std::min(2u * pageZ + dz, finer.Shape.PagesZ - 1u);
                const std::size_t child = static_cast<std::size_t>(childZ) * finer.Shape.PagesX + childX;
                lowest = std::min(lowest, finer.PageMin[child]);
                highest = std::max(highest, finer.PageMax[child]);
            }
        }
    }
    const std::size_t pageIndex = static_cast<std::size_t>(pageZ) * level.Shape.PagesX + pageX;
    level.PageMin[pageIndex] = lowest;
    level.PageMax[pageIndex] = highest;

    EncodedPage encoded;
    EncodeHeightPage(m_Writer.Layout().Header, samples, lowest, highest, encoded);
    const PageAddress address{0, static_cast<uint8>(levelIndex), pageX, pageZ};
    const PageIndexEntry* current = m_Writer.Layout().FindPresent(address);
    if (m_Patching && current && current->Hash == encoded.Hash && current->MinWord == encoded.MinWord &&
        current->MaxWord == encoded.MaxWord)
    {
        m_PagesUnchanged.fetch_add(1);
        return;
    }
    if (!m_Writer.WritePage(address, encoded))
    {
        m_Failed.store(true);
        return;
    }
    m_PagesWritten.fetch_add(1);
}

void HeightPyramidBuilder::MakeCoarserRows(uint32 levelIndex, uint32 firstRow, uint32 rowCount)
{
    LevelState& coarser = m_Levels[levelIndex + 1u];
    std::vector<std::vector<float32>> rows(rowCount);
    RunParallel(rowCount, [&](uint32 r) { MakeCoarserRow(levelIndex, firstRow + r, rows[r]); });
    for (std::vector<float32>& row : rows)
        coarser.Rows.push_back(std::move(row));
    coarser.RowsReceived += rowCount;
}

void HeightPyramidBuilder::MakeCoarserRow(uint32 levelIndex, uint32 row, std::vector<float32>& out) const
{
    const LevelState& finer = m_Levels[levelIndex];
    const uint32 finerWidth = finer.Shape.SamplesX;
    const uint32 width = m_Levels[levelIndex + 1u].Shape.SamplesX;
    const int64 center = 2 * static_cast<int64>(row);
    const float32* above = Row(finer, center - 1);
    const float32* middle = Row(finer, center);
    const float32* below = Row(finer, center + 1);
    out.resize(width);
    for (uint32 i = 0; i < width; ++i)
        out[i] = 0.25f * TentAcross(above, i, finerWidth) + 0.5f * TentAcross(middle, i, finerWidth) +
                 0.25f * TentAcross(below, i, finerWidth);
    Quantize(out.data(), out.size());
}

void HeightPyramidBuilder::Quantize(float32* values, std::size_t count) const
{
    const PageStoreHeader& header = m_Writer.Layout().Header;
    if (header.Format != PageFieldFormat::HeightR16)
        return;
    for (std::size_t i = 0; i < count; ++i)
        values[i] = header.Quantum.Decode(header.Quantum.Encode(values[i]));
}

void HeightPyramidBuilder::Discard(uint32 levelIndex)
{
    LevelState& level = m_Levels[levelIndex];
    const int64 rows = level.Shape.SamplesZ;
    int64 keepFrom = level.NextPageRow < level.Shape.PagesZ
                         ? static_cast<int64>(level.NextPageRow) * kPageOwnedSamples - kPageApronSamples
                         : rows;
    if (levelIndex + 1u < m_Levels.size() && level.NextChildRow < m_Levels[levelIndex + 1u].Shape.SamplesZ)
        keepFrom = std::min<int64>(keepFrom, 2 * static_cast<int64>(level.NextChildRow) - 1);
    // A clamped read of a row past the field's end reads the last row, so it stays until the end.
    keepFrom = std::clamp<int64>(keepFrom, 0, std::min<int64>(level.RowsReceived, rows - 1));
    if (level.NextPageRow == level.Shape.PagesZ &&
        (levelIndex + 1u == m_Levels.size() || level.NextChildRow == m_Levels[levelIndex + 1u].Shape.SamplesZ))
        keepFrom = level.RowsReceived;
    while (level.FirstKeptRow < keepFrom && !level.Rows.empty())
    {
        level.Rows.pop_front();
        ++level.FirstKeptRow;
    }
}

void HeightPyramidBuilder::RunParallel(uint32 count, const std::function<void(uint32)>& body) const
{
    JobSystem::ParallelFor(m_Pool, count, [&body](size_t i) { body(static_cast<uint32>(i)); });
}

} // namespace GameEngine::PageStreaming
