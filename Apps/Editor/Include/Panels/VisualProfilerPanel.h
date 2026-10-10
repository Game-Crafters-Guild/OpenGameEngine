#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Full include (not a forward decl): m_FrozenPassTimings + the row provider store
// the nested RGFrame::RGPassTiming by value, which needs the complete type here.
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Types/StringId.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/ListView.h"  // IListDataProvider, ListId, ListChangeSet
#include "UI/Interaction/Selection.h"

namespace GameEngine {

struct AssetLoadHandle;
class Button;
class ChartView;
class Dropdown;
class Label;
class TableView;
class UIElement;
struct UIEvent;

// Live GPU visual profiler. When armed, pulls per-pass CPU + GPU timings
// from Rendering::RenderGraph::RGFrame::LastFrameTimings() and rolls them
// into frame-time charts locally. GPU timestamps need a query pool; browser
// WebGPU has none (no encoder writeTimestamp), so those series stay at 0
// while CPU charts and the pass table still populate.
class VisualProfilerPanel : public DockPanel {
  public:
    std::string_view DeclaredTabIconClass() const override { return "pulse-icon"; }

    enum class Mode : std::uint8_t {
        Both = 0,
        Cpu = 1,
        Gpu = 2,
    };

    VisualProfilerPanel();
    ~VisualProfilerPanel() override;

    void OnPostLayout() override;
    void OnMountVisibilityChanged(bool isVisible) override;

    void Update(Rendering::RenderGraph::RGFrame* renderGraph);

  private:
    // Snapshot of the rows currently displayed in the pass table (already sorted by
    // the active metric). The TableView body renders directly against this; a version
    // bump on every SetRows makes the virtualized body rebind. Row id is a stable
    // hash of the pass name so selection survives a re-sort / scrub.
    class PassTimingsProvider : public IListDataProvider {
      public:
        int GetItemCount() const override { return static_cast<int>(m_Rows.size()); }
        ListId GetItemId(int index) const override
        {
            if (index < 0 || index >= static_cast<int>(m_Rows.size()))
                return 0;
            return HashStringId(std::string_view(m_Rows[static_cast<std::size_t>(index)].Name));
        }
        float GetItemHeight(int /*index*/) const override { return m_RowHeight; }
        void ConsumeChanges(std::uint64_t sinceVersion, ListChangeSet& out) const override
        {
            out.Version = m_Version;
            out.Ids.clear();
            out.Kind = (sinceVersion < m_Version) ? ChangeSetKind::All : ChangeSetKind::None;
        }

        void SetRows(std::vector<Rendering::RenderGraph::RGFrame::RGPassTiming> rows, double total)
        {
            m_Rows = std::move(rows);
            m_Total = total;
            ++m_Version;
        }
        int Size() const { return static_cast<int>(m_Rows.size()); }
        const Rendering::RenderGraph::RGFrame::RGPassTiming& Row(int i) const { return m_Rows[i]; }
        double Total() const { return m_Total; }

      private:
        std::vector<Rendering::RenderGraph::RGFrame::RGPassTiming> m_Rows;
        double m_Total = 0.0;
        std::uint64_t m_Version = 1;
        float m_RowHeight = 20.0f;
    };

    void BuildUI();
    void ConfigureProfilerTable();
    void RefreshToolbar(Rendering::RenderGraph::RGFrame* renderGraph);
    // Feeds the pass table's provider from the live/frozen timings (preserving the
    // scrub coupling: when paused with a cursor, reads m_FrozenPassTimings[cursor]).
    void RebuildPassSnapshot(Rendering::RenderGraph::RGFrame* renderGraph);
    void OnChartClicked(float localX, float chartWidth);
    void SelectCursorFrame(int frame);
    void ClearCursor();
    void SnapshotHistoryForFreeze(Rendering::RenderGraph::RGFrame* renderGraph);
    // Sample the graph's per-frame CPU/GPU totals into the rolling live series
    // that the frame-time charts draw in live (unpaused) mode.
    void AccumulateLiveHistory(Rendering::RenderGraph::RGFrame* renderGraph);
    void AttachChartScrubHandlers(ChartView* chart);
    // Apply the current m_ShowBudgetLine state to the CPU/GPU charts.
    void ApplyBudgetLine();
    void TogglePause();
    // Sets the active metric (Both/Cpu/Gpu), syncs the toolbar button + the table's
    // sort glyph, and rebuilds the snapshot. Driven by the Mode button + header clicks.
    void SetSortMode(Mode mode);
    void SaveColumnLayout();
    void LoadAndAttachPanelStyle();

    Button* m_PauseButton = nullptr;
    Button* m_ModeButton = nullptr;
    Button* m_BudgetButton = nullptr;
    Dropdown* m_BudgetDropdown = nullptr;
    Label* m_StatusLabel = nullptr;
    UIElement* m_BoundLabelRow = nullptr;
    ChartView* m_BoundChart = nullptr;
    ChartView* m_CpuChart = nullptr;
    ChartView* m_GpuChart = nullptr;
    TableView* m_Table = nullptr;

    PassTimingsProvider m_PassProvider;
    UI::Interaction::SelectionModel m_PassSelection; // name-keyed via stable ids; survives re-sort/scrub
    const std::string m_ColumnLayoutKey = "ui.profiler.columns";

    Mode m_Mode = Mode::Both;
    bool m_Paused = false;
    bool m_ShowBudgetLine = true; // frame-budget reference line on the charts
    float m_BudgetMs = 1000.0f / 60.0f; // user-editable budget (default 60 fps)
    // -1 == live tail; otherwise selects a frame inside the frozen history.
    int m_CursorFrame = -1;
    bool m_CursorDirty = false;
    ChartView* m_ScrubbingChart = nullptr;

    std::vector<float> m_FrozenCpuHistory;
    std::vector<float> m_FrozenGpuHistory;
    std::vector<std::vector<Rendering::RenderGraph::RGFrame::RGPassTiming>> m_FrozenPassTimings;

    // Rolling per-frame CPU/GPU totals for the live frame-time charts. Capped
    // at kMaxLiveHistory samples (oldest dropped).
    static constexpr std::size_t kMaxLiveHistory = 240;
    std::vector<float> m_LiveCpuHistory;
    std::vector<float> m_LiveGpuHistory;
    // Per-frame pass breakdowns, index-aligned with the histories above, so a
    // frozen scrub can show the passes of the selected frame.
    std::vector<std::vector<Rendering::RenderGraph::RGFrame::RGPassTiming>> m_LivePassTimings;

    std::chrono::steady_clock::time_point m_LastChartClickTime{};
    float m_LastChartClickX = 0.0f;
    float m_LastChartClickY = 0.0f;
    bool m_HasLastChartClick = false;

    Rendering::RenderGraph::RGFrame* m_LastRenderGraph = nullptr;
    int m_FrameCounter = 0;
    // Auto-arm profiling once when the panel first sees a render graph; after
    // that, leave the engine state alone so external toggles (RenderGraph
    // panel's Profile button, MCP) stick instead of being clobbered each frame.
    bool m_AutoArmedOnce = false;

    bool m_StyleAttached = false;
    bool m_StyleLoadScheduled = false;
    std::unique_ptr<AssetLoadHandle> m_PanelStyleLoadHandle;
};

} // namespace GameEngine
