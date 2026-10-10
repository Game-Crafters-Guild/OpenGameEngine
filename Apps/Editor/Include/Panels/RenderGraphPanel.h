#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "UI/Controls/DockPanel.h"

namespace GameEngine {

struct AssetLoadHandle;
class Button;
class Label;
class Splitter;
class TextField;
class UIElement;
class ScrollView;
class WeightedPane;
class RenderDocCapture;
namespace Rendering::RenderGraph { class RGFrame; }

// Live render-graph inspector: scrollable list of compiled passes on the left,
// detail pane on the right showing resource reads/writes and barrier counts for
// the selected pass. A Validation section surfaces the last render-pipeline
// blueprint compile/load diagnostics (S2.5) from FrameOrchestrator via rs.Spine().
class RenderGraphPanel : public DockPanel {
  public:
    std::string_view DeclaredTabIconClass() const override { return "pie-chart-icon"; }

    RenderGraphPanel();
    ~RenderGraphPanel() override;

    void OnPostLayout() override;
    void OnMountVisibilityChanged(bool isVisible) override;

    // Called from EditorApplication::Update with the RenderDoc capture helper
    // and the main window's live render graph (both may be null).
    void Update(RenderDocCapture* renderDoc, Rendering::RenderGraph::RGFrame* frame);

  private:
    enum class SortMode : std::uint8_t {
        Order = 0,
        Name,
        CpuDesc,
        GpuDesc,
        BarriersDesc,
    };

    void BuildUI();
    void BuildValidationSection();
    void RebuildPassList();
    void RefreshOverview();
    void RefreshValidation();
    void RefreshProfilingHint();
    void RefreshCaptureHint();
    void SetWarningsExpanded(bool expanded);
    void RefreshDetail();
    void RefreshTimings();
    void RefreshHardwareLabel();
    void RefreshRowMetas();
    void ApplySearchFilter();
    void ExportCsv();
    void SelectPass(const std::string& name);
    void MoveSelection(int delta);
    void CycleSortMode();
    void UpdateSortButtonText();
    void LoadAndAttachPanelStyle();

    struct PassRow {
        UIElement* Root = nullptr;
        Label* NameLabel = nullptr;
        Label* QueueLabel = nullptr;
        Label* BarrierNumLabel = nullptr;
        Label* BarrierWordLabel = nullptr;
        UIElement* CpuGroup = nullptr;
        Label* CpuNumLabel = nullptr;
        UIElement* GpuGroup = nullptr;
        Label* GpuNumLabel = nullptr;
        std::string Name;
        std::uint32_t QueueType = 0;
        std::uint32_t BarrierCount = 0;
    };

    // Toolbar
    Button* m_CaptureButton = nullptr;
    Button* m_ProfileButton = nullptr;
    Button* m_ExportCsvButton = nullptr;
    Button* m_SortButton = nullptr;
    TextField* m_SearchField = nullptr;
    Label* m_HardwareLabel = nullptr;

    // Overview row: separate number/label segments so counts can render
    // in a brighter color than the surrounding words.
    Label* m_OverviewPassCount = nullptr;
    Label* m_OverviewResourceCount = nullptr;
    Label* m_OverviewBarrierCount = nullptr;
    Label* m_OverviewTransientCount = nullptr;
    Label* m_OverviewPersistentCount = nullptr;
    Label* m_OverviewImportedCount = nullptr;

    // Second overview row: recording stats from the last Execute (frame->Stats()).
    Label* m_OverviewSubmissions = nullptr;
    Label* m_OverviewBarrierBatches = nullptr;
    Label* m_OverviewBarriersHoisted = nullptr;
    Label* m_OverviewRenderPasses = nullptr;
    // Panel inspects the main window's frame only (m_Windows[0]->RenderGraphStream.Frame).
    Label* m_WindowHint = nullptr;
    // Sits next to the Profile control: timings stay zero until profiling is
    // armed, and GE_ENABLE_GPU_PROFILING=0 makes the button a no-op.
    Label* m_ProfilingHint = nullptr;
    // Sits next to the Capture control: names the .rdc file once RenderDoc has
    // finished writing the capture this panel triggered.
    Label* m_CaptureHint = nullptr;

    // Validation section: banner + issue rows sourced from the FrameOrchestrator's
    // last compile/load report (rs.Spine().LastCompileReport()).
    UIElement* m_ValidationSection = nullptr;
    UIElement* m_ValidationBanner = nullptr;
    Label* m_ValidationBannerLabel = nullptr;
    UIElement* m_ValidationErrorList = nullptr;
    Button* m_WarningsToggle = nullptr;
    UIElement* m_ValidationWarnList = nullptr;
    // Errors are shown by default; warnings collapse behind the toggle so the
    // fleet's duplicate-publish warnings can't wall the section.
    bool m_WarningsExpanded = false;
    // Refresh key: the report Generation last rendered. Sentinel forces the
    // first Update to reconcile the section (even on a healthy, no-issue boot).
    std::uint64_t m_LastValidationGen = ~0ull;

    SortMode m_SortMode = SortMode::Order;

    // Body
    UIElement* m_PassListContainer = nullptr;
    ScrollView* m_PassListScroll = nullptr;
    UIElement* m_DetailContainer = nullptr;
    UIElement* m_BodyElement = nullptr;
    WeightedPane* m_ListPane = nullptr;
    WeightedPane* m_DetailPane = nullptr;
    Splitter* m_SplitterElement = nullptr;

    std::vector<PassRow> m_Rows;
    std::string m_SelectedPass;
    std::string m_SearchText;
    std::size_t m_LastPassSignature = 0;
    std::size_t m_LastDetailSignature = 0;
    std::string m_LastDetailPass;
    int m_FrameCounter = 0;
    bool m_DetailDirty = false;

    // Latest per-pass timings (name -> {cpuMs, gpuMs}), overlaid from the live
    // graph's resolved profiling results when profiling is armed.
    std::unordered_map<std::string, std::pair<double, double>> m_TimingByName;

    // Latest Update() pointers, captured so button callbacks can act on them.
    RenderDocCapture* m_LastRenderDoc = nullptr;
    Rendering::RenderGraph::RGFrame* m_LastFrame = nullptr;
    // Per-pass profiling toggle state shown on the Profile button; drives
    // RGFrame::SetProfilingEnabled on the live graph.
    bool m_ProfilingEnabled = false;

    // RenderDoc's capture count when the Capture button was last clicked. The
    // capture is only on disk once the count moves past it, which is several
    // frames later for a large scene.
    std::uint32_t m_CaptureCountAtTrigger = 0;
    bool m_CapturePending = false;
    // Frames waited since that click; bounded so a capture that never lands
    // does not leave the hint claiming one is in flight forever.
    std::uint32_t m_CapturePendingFrames = 0;
    static constexpr std::uint32_t kCapturePendingBudgetFrames = 300;

    bool m_StyleAttached = false;
    bool m_StyleLoadScheduled = false;
    std::unique_ptr<AssetLoadHandle> m_PanelStyleLoadHandle;
};

} // namespace GameEngine
