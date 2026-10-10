#pragma once

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/DebugMetrics.h"
#include "UI/Controls/DockPanel.h"

namespace GameEngine {

struct AssetLoadHandle;
struct UIEvent;
class ChartView;
class Checkbox;
class Label;
class Button;
class UIElement;

namespace Editor { class UndoRedoService; }

// Debugger "Monitors" panel: a grouped, checkbox-driven list of performance
// counters on the left and a grid of mini charts on the right — one strip
// per checked monitor. Samples come from GameEngine::Debug::DebugMetrics.
class MonitorsPanel : public DockPanel {
  public:
    std::string_view DeclaredTabIconClass() const override { return "stats-icon"; }

    MonitorsPanel();
    ~MonitorsPanel() override;

    void OnPostLayout() override;
    void Update();
    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_Undo = undo; }

    struct ChartLayoutSnapshot {
        std::vector<std::string> order;
        std::set<std::string> rowBreaks;
        std::set<std::string> checked;
    };

  private:
    struct RowWidgets {
        Checkbox* Check = nullptr;
        Label* Value = nullptr;
        Debug::MonitorType Type = Debug::MonitorType::Quantity;
    };

    struct ChartWidgets {
        UIElement* RowStart = nullptr;
        UIElement* Strip = nullptr;
        Label* Name = nullptr;
        Label* Value = nullptr;
        ChartView* Chart = nullptr;
        Debug::MonitorType Type = Debug::MonitorType::Quantity;
    };

    enum class ChartDropPlacement {
        None,
        Before,
        After,
        Append
    };

    struct ChartDropTarget {
        std::string Name;
        ChartDropPlacement Placement = ChartDropPlacement::None;
        bool NewRow = false;
    };

    void BuildUI();
    void RebuildMonitorList();
    void RequestMonitorListRebuild();
    void ToggleGroupCollapsed(const std::string& groupKey);
    void UpdateValues();
    void ToggleMonitor(const std::string& name, bool on);
    void AddChartFor(const std::string& name);
    void RemoveChartFor(const std::string& name);
    void AttachChartDragHandlers(UIElement* strip, UIElement* header, const std::string& name);
    ChartDropTarget HitTestChartDropTarget(float x, float y, const std::string& ignoreName) const;
    ChartDropTarget NormalizeChartDropTarget(const std::string& source, ChartDropTarget target) const;
    bool IsChartDropNoOp(const std::string& source, const ChartDropTarget& target) const;
    void SetChartDropTarget(const ChartDropTarget& target);
    void ClearChartDragState();
    void ApplyChartDrop(const std::string& source, const ChartDropTarget& target);
    ChartLayoutSnapshot CaptureChartLayoutSnapshot() const;
    void ApplyChartLayoutSnapshot(const ChartLayoutSnapshot& snapshot);
    void CommitChartLayoutUndo(const ChartLayoutSnapshot& before, const ChartLayoutSnapshot& after);
    void ReorderChartElements();
    void ShowChartDragGhost(const std::string& name, float x, float y);
    void HideChartDragGhost();
    void ShowChartDropIndicator(const ChartDropTarget& target);
    void HideChartDropIndicator();

    void AttachChartScrubHandlers(ChartView* chart);
    void OnChartClicked(ChartView* chart, float localX, float chartWidth);
    void SnapshotSeriesForFreeze();
    void ApplyCursorToCharts();
    void ClearCursor();
    void TogglePause();
    void ResumeFromPause();

    void PushBuiltInSamples();
    void UpdateHdrStatusLabel();
    void LoadAndAttachPanelStyle();

    // UI roots
    UIElement* m_ListContainer = nullptr;
    UIElement* m_ChartsContainer = nullptr;
    Label* m_ChartDragGhost = nullptr;
    UIElement* m_ChartDropIndicator = nullptr;
    Button* m_PauseButton = nullptr;
    Button* m_ClearButton = nullptr;
    Label* m_HdrStatusLabel = nullptr;

    // Per-row widgets, keyed by full monitor name ("Time/FPS" ...).
    std::unordered_map<std::string, RowWidgets> m_Rows;

    // Per-row group membership (full monitor name -> group prefix). Used to
    // hide/show rows when their group is collapsed.
    std::unordered_map<std::string, std::string> m_RowGroup;
    // Group prefixes that the user has collapsed (children hidden).
    std::set<std::string> m_CollapsedGroups;
    // Group header element pointers, keyed by group prefix.
    std::unordered_map<std::string, UIElement*> m_GroupHeaders;

    // Per-chart strip widgets for currently displayed monitors.
    std::unordered_map<std::string, ChartWidgets> m_Charts;
    std::vector<std::string> m_ChartOrder;
    std::set<std::string> m_ChartRowBreaks;

    // Chart reorder drag state.
    std::string m_DragChartName;
    ChartDropTarget m_ChartDropTarget;
    float m_DragStartX = 0.0f;
    float m_DragStartY = 0.0f;
    bool m_ChartDragActive = false;

    // Which monitors are currently checked / displayed.
    std::set<std::string> m_Checked;

    // Scrub state: when m_Paused is true, charts and value labels read from
    // m_FrozenSeries[name] at m_CursorFrame instead of live history / Latest.
    bool m_Paused = false;
    int m_CursorFrame = -1;
    ChartView* m_ScrubbingChart = nullptr;
    std::unordered_map<std::string, std::vector<float>> m_FrozenSeries;

    // Double-click detection on chart strips (resume from pause on double-click).
    std::chrono::steady_clock::time_point m_LastChartClickTime{};
    float m_LastChartClickX = 0.0f;
    float m_LastChartClickY = 0.0f;
    bool m_HasLastChartClick = false;

    // Rebuild cadence + signature-based change detection.
    int m_FrameCounter = 0;
    std::size_t m_LastListSignature = 0;
    bool m_MonitorListRebuildPending = false;

    bool m_StyleAttached = false;
    bool m_StyleLoadScheduled = false;
    bool m_SuppressChartLayoutUndo = false;
    std::string m_LastHdrStatusText;
    std::unique_ptr<AssetLoadHandle> m_PanelStyleLoadHandle;
    Editor::UndoRedoService* m_Undo = nullptr;
    std::shared_ptr<bool> m_LifetimeToken = std::make_shared<bool>(true);
};

} // namespace GameEngine
