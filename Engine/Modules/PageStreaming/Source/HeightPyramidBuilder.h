#pragma once

#include "PageStreaming/PageStoreFormat.h"

#include <atomic>
#include <deque>
#include <functional>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine::PageStreaming
{

class PageStoreWriter;
struct EncodedPage;

/// Builds a height store's pyramid from its level-0 rows, pushed in order, with memory bounded by
/// a few page rows per level. Each level keeps only the rows its next page row and the next
/// level's next rows still read: a page row is written as soon as its rows and apron are in, and
/// a coarser row is made with the [1 2 1] / 4 tent as soon as its three source rows are. Every
/// value is quantized to the store's encoding before anything reads it, so a coarser level is the
/// filter of what the level below stores, and a recook of unchanged input reproduces every page.
class HeightPyramidBuilder
{
public:
    /// Writes into `writer`, whose layout is the store's. When `patching`, a page whose encoded
    /// bytes hash to the entry the writer already holds is not written.
    HeightPyramidBuilder(PageStoreWriter& writer, bool patching, JobSystem::WorkStealingThreadPool* pool);

    /// Appends the next `rowCount` level-0 rows (SamplesX values each, the source's units).
    void PushLevel0Rows(const float32* rows, uint32 rowCount);

    /// True when every row was pushed and every page written.
    bool Finish() const;

    uint64 PagesWritten() const { return m_PagesWritten.load(); }
    uint64 PagesUnchanged() const { return m_PagesUnchanged.load(); }

private:
    struct LevelState
    {
        PageStoreLevel Shape;
        uint32 RowsReceived = 0;
        uint32 FirstKeptRow = 0;
        std::deque<std::vector<float32>> Rows; // rows [FirstKeptRow, RowsReceived)
        uint32 NextPageRow = 0;
        uint32 NextChildRow = 0; // rows of the next coarser level made so far
        std::vector<float32> PageMin; // per page, level-0 truth and the page's own footprint
        std::vector<float32> PageMax;
    };

    const float32* Row(const LevelState& level, int64 row) const;
    void Advance(uint32 level);
    void EmitPageRow(uint32 level, uint32 pageRow);
    void EmitPage(uint32 level, uint32 pageX, uint32 pageZ);
    void MakeCoarserRows(uint32 level, uint32 firstRow, uint32 rowCount);
    void MakeCoarserRow(uint32 level, uint32 row, std::vector<float32>& out) const;
    void Quantize(float32* values, std::size_t count) const;
    void Discard(uint32 level);
    void RunParallel(uint32 count, const std::function<void(uint32)>& body) const;

    PageStoreWriter& m_Writer;
    bool m_Patching = false;
    JobSystem::WorkStealingThreadPool* m_Pool = nullptr;
    std::vector<LevelState> m_Levels;
    std::atomic<uint64> m_PagesWritten{0};
    std::atomic<uint64> m_PagesUnchanged{0};
    std::atomic<bool> m_Failed{false};
};

} // namespace GameEngine::PageStreaming
