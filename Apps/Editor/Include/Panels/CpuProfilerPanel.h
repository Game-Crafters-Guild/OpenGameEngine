#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/CpuProfiler.h"
#include "UI/Controls/DockPanel.h"

namespace GameEngine {

struct AssetLoadHandle;
struct UIEvent;
class Button;
class ChartView;
class Label;
class ScrollView;
class UIElement;

// Simple CPU profiler panel on top of GameEngine::Profiling::CpuProfiler.
// Enable/Disable toggle, a frame-time ChartView fed by DebugMetrics
// "Time/FrameMs", and a flat list of the top scopes by totalMs (inclusive)
// from CpuProfiler::CopyFrameSamples(). No parent/child tree yet -- the
// existing service aggregates flat samples per frame.
class CpuProfilerPanel : public DockPanel {
  public:
    std::string_view DeclaredTabIconClass() const override { return "timer-icon"; }

    CpuProfilerPanel();
    ~CpuProfilerPanel() override;

    void OnPostLayout() override;
    void OnMountVisibilityChanged(bool isVisible) override;
    void Update();

  private:
    struct ScopeRow {
        UIElement* Root = nullptr;
        Label* NameLabel = nullptr;
        Label* TotalLabel = nullptr;
        Label* SelfLabel = nullptr;
        Label* CountLabel = nullptr;
        std::string Name;
    };

    enum class SortColumn { Name, Total, Self, Calls };

    void BuildUI();
    void RefreshRows();
    void AttachChartScrubHandlers();
    void OnChartClicked(float localX, float chartWidth);
    void SnapshotHistoryForFreeze();
    void ClearCursor();
    void TogglePause();
    void UpdateHeaderLabel();
    void UpdateSortHeaderLabels();
    void OnSortHeaderClicked(SortColumn column);
    void OnRowsKeyDown(UIEvent& e);
    void SelectScope(const std::string& name);
    void ApplySelectionClasses();
    void LoadAndAttachPanelStyle();

    Button* m_EnableButton = nullptr;
    Label* m_HeaderLabel = nullptr;
    Label* m_SortNameLabel = nullptr;
    Label* m_SortTotalLabel = nullptr;
    Label* m_SortSelfLabel = nullptr;
    Label* m_SortCallsLabel = nullptr;
    ScrollView* m_RowsScroll = nullptr;
    UIElement* m_RowsContainer = nullptr;
    ChartView* m_Chart = nullptr;
    std::string m_SelectedScopeName;

    SortColumn m_SortColumn = SortColumn::Total;
    bool m_SortAscending = false;

    std::vector<ScopeRow> m_Rows;

    std::vector<float> m_FrozenCpuHistory;
    std::vector<Profiling::CpuProfiler::FrameHistoryEntry> m_FrozenFrameHistory;
    bool m_ChartFrozen = false;
    bool m_Paused = false;
    bool m_CursorDirty = false;
    int m_CursorFrame = -1;
    bool m_Scrubbing = false;

    std::chrono::steady_clock::time_point m_LastChartClickTime{};
    float m_LastChartClickX = 0.0f;
    float m_LastChartClickY = 0.0f;
    bool m_HasLastChartClick = false;

    int m_FrameCounter = 0;

    bool m_StyleAttached = false;
    bool m_StyleLoadScheduled = false;
    std::unique_ptr<AssetLoadHandle> m_PanelStyleLoadHandle;
};

} // namespace GameEngine
