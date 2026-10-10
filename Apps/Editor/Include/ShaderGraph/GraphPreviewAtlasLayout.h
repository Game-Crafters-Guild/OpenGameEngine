#pragma once

// Cell bookkeeping for the material-graph node preview atlas: which node owns
// which cell, how many rows the atlas needs, and the CSS sprite addressing a UI
// element uses to show one cell.
//
// Pure logic — no GPU state — so the assignment/growth/addressing rules are
// testable without a device.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::Editor
{

class GraphPreviewAtlasLayout
{
  public:
    // One cell per node preview. 256 px matches the slot thumbnails the atlas
    // replaces, so the on-screen sampling density is unchanged.
    static constexpr uint32_t kCellPx = 256;
    static constexpr uint32_t kColumns = 4;
    // Rows are added a page at a time: a graph that gains one node does not
    // reallocate the atlas texture per node.
    static constexpr uint32_t kRowsPerPage = 4;

    // Background addressing for one cell, in the percentages CSS uses:
    // background-size scales the atlas so one cell fills the element, and
    // background-position slides the wanted cell into it.
    struct CellRect
    {
        float SizeXPercent = 100.f;
        float SizeYPercent = 100.f;
        float PosXPercent = 0.f;
        float PosYPercent = 0.f;

        bool operator==(const CellRect&) const = default;
    };

    /// Assigns a stable cell to every key in `keys` and releases the cells of
    /// keys that are gone. Returns true when any assignment (or the row count)
    /// moved, which is the signal to redraw and rebind.
    bool Reconcile(const std::vector<std::string>& keys);

    /// Cell index for a key, or false when the key owns no cell.
    bool TryGetCell(const std::string& key, uint32_t& outCell) const;

    uint32_t RowCount() const { return m_Rows; }
    uint32_t Capacity() const { return m_Rows * kColumns; }
    uint32_t WidthPx() const { return kColumns * kCellPx; }
    uint32_t HeightPx() const { return m_Rows * kCellPx; }

    /// Top-left pixel of a cell's viewport within the atlas.
    void CellOrigin(uint32_t cell, uint32_t& outX, uint32_t& outY) const;

    CellRect BackgroundRectForCell(uint32_t cell) const
    {
        return BackgroundRect(cell, kColumns, m_Rows);
    }

    /// Sprite addressing for `cell` in a `columns` x `rows` grid. A single-cell
    /// axis has no travel, so its position collapses to 0 rather than dividing
    /// by zero.
    static CellRect BackgroundRect(uint32_t cell, uint32_t columns, uint32_t rows);

  private:
    std::unordered_map<std::string, uint32_t> m_CellOfKey;
    std::vector<uint32_t> m_FreeCells;
    uint32_t m_Rows = 0;
    uint32_t m_NextCell = 0;
};

} // namespace GameEngine::Editor
