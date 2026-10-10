#include "Panels/VisualProfilerPanel.h"

#include "Platform/SystemMetrics.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Editor/Settings/AxisLayoutSerializer.h"
#include "Editor/Settings/SettingsStore.h"
#include "Input/KeyCodes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/ChartView.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TableView.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

namespace GameEngine {

namespace {

// Semi-transparent amber (ABGR packed), distinct from the faint white peak line.
constexpr std::uint32_t kBudgetLineColor = 0xC000A5FFu;

// Frame-budget presets, selectable in the toolbar dropdown.
constexpr int kBudgetFps[] = {30, 60, 90, 120, 144, 240};
constexpr int kBudgetFpsCount = static_cast<int>(sizeof(kBudgetFps) / sizeof(kBudgetFps[0]));
constexpr int kBudgetFpsDefaultIndex = 1; // 60 fps

const char* ModeLabel(VisualProfilerPanel::Mode mode)
{
    switch (mode) {
    case VisualProfilerPanel::Mode::Both: return "CPU+GPU";
    case VisualProfilerPanel::Mode::Cpu:  return "CPU";
    case VisualProfilerPanel::Mode::Gpu:  return "GPU";
    }
    return "?";
}

std::string FormatMs(double ms)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%7.2f ms", ms);
    return std::string(buf);
}

// Pass-table columns. Three sortable metric columns map 1:1 to the three Modes;
// the % column is the "combined" (max of CPU/GPU) view, so sorting it == Mode::Both.
constexpr StringId kColPass = "profiler.pass.name"_sid;
constexpr StringId kColCpu  = "profiler.pass.cpu"_sid;
constexpr StringId kColGpu  = "profiler.pass.gpu"_sid;
constexpr StringId kColPct  = "profiler.pass.pct"_sid;

StringId KeyForMode(VisualProfilerPanel::Mode m)
{
    switch (m) {
    case VisualProfilerPanel::Mode::Cpu: return kColCpu;
    case VisualProfilerPanel::Mode::Gpu: return kColGpu;
    case VisualProfilerPanel::Mode::Both:
    default:                             return kColPct;
    }
}

VisualProfilerPanel::Mode ModeForKey(StringId key)
{
    if (key == kColCpu) return VisualProfilerPanel::Mode::Cpu;
    if (key == kColGpu) return VisualProfilerPanel::Mode::Gpu;
    return VisualProfilerPanel::Mode::Both;
}

double MetricFor(const Rendering::RenderGraph::RGFrame::RGPassTiming& t, VisualProfilerPanel::Mode m)
{
    switch (m) {
    case VisualProfilerPanel::Mode::Cpu: return t.CpuMs;
    case VisualProfilerPanel::Mode::Gpu: return t.GpuSpanMs;
    case VisualProfilerPanel::Mode::Both:
    default:                             return std::max(t.CpuMs, t.GpuSpanMs);
    }
}

using PassTiming = Rendering::RenderGraph::RGFrame::RGPassTiming;

// Σ of a frame's GPU measurements counting each ONCE. Which passes those are is
// decided by the render graph's fold (RGPassTiming::SpanCounted) — the same
// decision behind resolveStats.distinctSpanGpuMs — so this panel cannot drift
// from the payload. Still an upper bound: distinct spans overlap on the GPU.
double SumDistinctGpuMs(const std::vector<PassTiming>& timings)
{
    double sum = 0.0;
    for (const auto& t : timings)
    {
        if (t.SpanCounted)
            sum += t.GpuSpanMs;
    }
    return sum;
}

// A pass's share of the frame total, in the active metric (single-sources the % math
// shared by the cell binder + the Ctrl+C copy).
std::string FormatPct(const Rendering::RenderGraph::RGFrame::RGPassTiming& t,
                      VisualProfilerPanel::Mode m, double total)
{
    const double pct = total > 0.0 ? MetricFor(t, m) / total * 100.0 : 0.0;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%5.1f%%", pct);
    return buf;
}

} // namespace

VisualProfilerPanel::~VisualProfilerPanel()
{
    // The pending asset callbacks capture this panel; withdraw them before it goes
    // away (destroying an AssetLoadHandle does not).
    if (m_PanelStyleLoadHandle)
        m_PanelStyleLoadHandle->Cancel();
}

VisualProfilerPanel::VisualProfilerPanel()
    : DockPanel("Visual Profiler")
{
    AddClass("visual-profiler-panel");
    BuildUI();
}

void VisualProfilerPanel::BuildUI()
{
    // Toolbar
    auto toolbar = std::make_unique<UIElement>();
    toolbar->AddClass("visual-profiler-toolbar");

    auto pauseBtn = std::make_unique<Button>();
    pauseBtn->AddClass("small");
    pauseBtn->AddClass("secondary");
    pauseBtn->AddClass("visual-profiler-btn-pause");
    pauseBtn->SetText("Pause");
    pauseBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { TogglePause(); });
    m_PauseButton = pauseBtn.get();
    toolbar->AddChild(std::move(pauseBtn));

    auto clearBtn = std::make_unique<Button>();
    clearBtn->AddClass("small");
    clearBtn->AddClass("secondary");
    clearBtn->AddClass("visual-profiler-btn-clear");
    clearBtn->SetText("Clear");
    clearBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        m_FrozenCpuHistory.clear();
        m_FrozenGpuHistory.clear();
        m_LiveCpuHistory.clear();
        m_LiveGpuHistory.clear();
        m_LivePassTimings.clear();
        m_CursorFrame = -1;
        m_CursorDirty = true;
        if (m_BoundChart)
            m_BoundChart->SetCursorIndex(-1);
        if (m_CpuChart)
            m_CpuChart->SetCursorIndex(-1);
        if (m_GpuChart)
            m_GpuChart->SetCursorIndex(-1);
        if (m_LastRenderGraph && m_LastRenderGraph->ProfilingEnabled())
        {
            m_LastRenderGraph->SetProfilingEnabled(false);
            m_LastRenderGraph->SetProfilingEnabled(true);
        }
        PostAction([this]() {
            m_PassProvider.SetRows({}, 0.0);
            m_PassSelection.Clear();
            if (m_Table)
                m_Table->Refresh();
        });
    });
    toolbar->AddChild(std::move(clearBtn));

    auto modeBtn = std::make_unique<Button>();
    modeBtn->AddClass("small");
    modeBtn->AddClass("secondary");
    modeBtn->AddClass("visual-profiler-btn-mode");
    modeBtn->SetText(ModeLabel(m_Mode));
    modeBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        SetSortMode(static_cast<Mode>((static_cast<int>(m_Mode) + 1) % 3));
    });
    m_ModeButton = modeBtn.get();
    toolbar->AddChild(std::move(modeBtn));

    auto status = std::make_unique<Label>();
    status->AddClass("visual-profiler-status");
    status->SetText("profiler disarmed");
    m_StatusLabel = status.get();
    toolbar->AddChild(std::move(status));

    // Frame-budget preset dropdown, pinned right next to the toggle.
    auto budgetDropdown = std::make_unique<Dropdown>();
    budgetDropdown->AddClass("visual-profiler-budget-dropdown");
    {
        std::vector<std::string> labels;
        labels.reserve(kBudgetFpsCount);
        for (int i = 0; i < kBudgetFpsCount; ++i)
            labels.push_back(std::to_string(kBudgetFps[i]) + " fps");
        budgetDropdown->SetOptionsFromLabels(labels, kBudgetFpsDefaultIndex);
    }
    budgetDropdown->SetOnValueChanged([this](const std::string&) {
        if (!m_BudgetDropdown)
            return;
        const int idx = m_BudgetDropdown->GetSelectedIndex();
        if (idx >= 0 && idx < kBudgetFpsCount)
            m_BudgetMs = 1000.0f / static_cast<float>(kBudgetFps[idx]);
        ApplyBudgetLine();
    });
    m_BudgetDropdown = budgetDropdown.get();
    toolbar->AddChild(std::move(budgetDropdown));

    // Budget-line toggle, pinned right (the status label's flex:1 pushes it there).
    auto budgetBtn = std::make_unique<Button>();
    budgetBtn->AddClass("small");
    budgetBtn->AddClass("secondary");
    budgetBtn->AddClass("visual-profiler-btn-budget");
    budgetBtn->SetText("Budget: On");
    budgetBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        m_ShowBudgetLine = !m_ShowBudgetLine;
        ApplyBudgetLine();
    });
    m_BudgetButton = budgetBtn.get();
    toolbar->AddChild(std::move(budgetBtn));

    AddChild(std::move(toolbar));

    // Bound-chart label: three-span row so the CPU or GPU word can be tinted
    // depending on which side is currently dominant.
    auto boundLabelRow = std::make_unique<UIElement>();
    boundLabelRow->AddClass("visual-profiler-chart-label");
    boundLabelRow->AddClass("bound-label");
    {
        auto cpuWord = std::make_unique<Label>();
        cpuWord->AddClass("bound-label-word");
        cpuWord->AddClass("cpu");
        cpuWord->SetText("CPU");
        boundLabelRow->AddChild(std::move(cpuWord));

        auto slash = std::make_unique<Label>();
        slash->AddClass("bound-label-sep");
        slash->SetText(" / ");
        boundLabelRow->AddChild(std::move(slash));

        auto gpuWord = std::make_unique<Label>();
        gpuWord->AddClass("bound-label-word");
        gpuWord->AddClass("gpu");
        gpuWord->SetText("GPU");
        boundLabelRow->AddChild(std::move(gpuWord));

        auto suffix = std::make_unique<Label>();
        suffix->AddClass("bound-label-sep");
        suffix->SetText(" bound");
        boundLabelRow->AddChild(std::move(suffix));
    }
    m_BoundLabelRow = boundLabelRow.get();
    AddChild(std::move(boundLabelRow));

    // Bound sparkline: signed delta (cpuMs - gpuMs) — line goes up when the
    // frame is CPU-bound, down when GPU-bound. Y range is symmetric and
    // recomputed every frame in RefreshToolbar.
    auto boundChart = std::make_unique<ChartView>();
    boundChart->AddClass("visual-profiler-bound-chart");
    boundChart->SetMode(ChartView::Mode::Line);
    boundChart->SetLineColor(0xFFFFA060u);        // #60A0FF — CPU-bound (above zero)
    boundChart->SetNegativeLineColor(0xFF4080F0u); // #F08040 — GPU-bound (below zero)
    boundChart->SetShowAxisLabels(false);
    boundChart->SetProvider([this](std::vector<float>& out) {
        out.clear();
        const std::vector<float>& cpu = m_Paused ? m_FrozenCpuHistory : m_LiveCpuHistory;
        const std::vector<float>& gpu = m_Paused ? m_FrozenGpuHistory : m_LiveGpuHistory;
        const std::size_t n = std::min(cpu.size(), gpu.size());
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            out.push_back(cpu[i] - gpu[i]);
    });
    boundChart->SetValueFormatter([](float v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%+.2f ms", v);
        return std::string(buf);
    });
    m_BoundChart = boundChart.get();
    AttachChartScrubHandlers(m_BoundChart);
    AddChild(std::move(boundChart));

    // Frame history charts: CPU on top (blue), GPU below (orange). Series come
    // from AccumulateLiveHistory over LastFrameTimings(); GPU stays flat at 0
    // when the device has no timestamp pool (browser WebGPU).
    auto msFormatter = [](float v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f ms", v);
        return std::string(buf);
    };

    auto cpuLabel = std::make_unique<Label>();
    cpuLabel->AddClass("visual-profiler-chart-label");
    cpuLabel->AddClass("cpu");
    cpuLabel->SetText("CPU frame time (ms)");
    AddChild(std::move(cpuLabel));

    auto cpuChart = std::make_unique<ChartView>();
    cpuChart->AddClass("visual-profiler-chart");
    cpuChart->AddClass("cpu");
    cpuChart->SetMode(ChartView::Mode::Area);
    cpuChart->SetLineColor(0xFFFFA060u); // #60A0FF packed ABGR
    cpuChart->SetFillColor(0x40FFA060u);
    cpuChart->SetProvider([this](std::vector<float>& out) {
        out = m_Paused ? m_FrozenCpuHistory : m_LiveCpuHistory;
    });
    cpuChart->SetValueFormatter(msFormatter);
    m_CpuChart = cpuChart.get();
    AttachChartScrubHandlers(m_CpuChart);
    AddChild(std::move(cpuChart));

    auto gpuLabel = std::make_unique<Label>();
    gpuLabel->AddClass("visual-profiler-chart-label");
    gpuLabel->AddClass("gpu");
    gpuLabel->SetText("GPU frame time (ms)");
    AddChild(std::move(gpuLabel));

    auto gpuChart = std::make_unique<ChartView>();
    gpuChart->AddClass("visual-profiler-chart");
    gpuChart->AddClass("gpu");
    gpuChart->SetMode(ChartView::Mode::Area);
    gpuChart->SetLineColor(0xFF4080F0u); // #F08040 packed ABGR
    gpuChart->SetFillColor(0x404080F0u);
    gpuChart->SetProvider([this](std::vector<float>& out) {
        out = m_Paused ? m_FrozenGpuHistory : m_LiveGpuHistory;
    });
    gpuChart->SetValueFormatter(msFormatter);
    m_GpuChart = gpuChart.get();
    AttachChartScrubHandlers(m_GpuChart);
    AddChild(std::move(gpuChart));

    // Per-pass list: the reusable TableView (resizable/sortable columns, virtualized,
    // persisted widths). Columns Pass / CPU / GPU / %.
    auto table = std::make_unique<TableView>();
    table->AddClass("visual-profiler-table");
    m_Table = table.get();
    AddChild(std::move(table));
    ConfigureProfilerTable();

    RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
        if (e.Key != Input::kKeyCode_Space)
            return;
        TogglePause();
        e.Stop();
    });

    ApplyBudgetLine();
}

void VisualProfilerPanel::ConfigureProfilerTable()
{
    std::vector<TrackDef> columns;
    auto addCol = [&columns](StringId key, const char* title, float size, float minSize,
                             bool sortable, TrackDef::Align align) {
        TrackDef t;
        t.Key       = key;
        t.Title     = title;
        t.Size      = size;
        t.MinSize   = minSize;
        t.Sortable  = sortable;
        t.Resizable = true;
        t.Alignment = align;
        columns.push_back(std::move(t));
    };
    addCol(kColPass, "Pass", 240.0f, 120.0f, false, TrackDef::Align::Start);
    addCol(kColCpu,  "CPU",   96.0f,  70.0f, true,  TrackDef::Align::End);
    addCol(kColGpu,  "GPU",   96.0f,  70.0f, true,  TrackDef::Align::End);
    addCol(kColPct,  "%",     64.0f,  48.0f, true,  TrackDef::Align::End);
    columns.push_back(TrackDef::MakeFill());
    m_Table->SetColumns(std::move(columns));

    {
        Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
        prefs.Load();
        Editor::LoadAxisLayout(prefs, m_ColumnLayoutKey, m_Table->Columns());
        m_Table->RefreshColumnLayout();
    }

    m_Table->SetRowProvider(&m_PassProvider);
    m_Table->SetCellBinder(
        [this](UIElement* cell, int, const TrackDef& track, ListId, int rowIndex, IListDataProvider*) {
            if (!cell || rowIndex < 0 || rowIndex >= m_PassProvider.Size())
                return;
            Label* label = nullptr;
            if (cell->GetChildren().empty())
            {
                auto lbl = std::make_unique<Label>();
                const char* cls = (track.Key == kColCpu) ? "profiler-cell-cpu"
                                : (track.Key == kColGpu) ? "profiler-cell-gpu"
                                : (track.Key == kColPct) ? "profiler-cell-pct"
                                                         : "profiler-cell-name";
                lbl->AddClass(cls);
                label = lbl.get();
                cell->AddChild(std::move(lbl));
            }
            else
            {
                label = dynamic_cast<Label*>(cell->GetChildren()[0].get());
            }
            if (!label)
                return;

            const auto& t = m_PassProvider.Row(rowIndex);
            if (track.Key == kColPass)
                label->SetText(t.Name); // char[64], null-terminated
            else if (track.Key == kColCpu)
                label->SetText(FormatMs(t.CpuMs));
            else if (track.Key == kColGpu)
                label->SetText(t.SpanShared ? FormatMs(t.GpuSpanMs) + "*" : FormatMs(t.GpuSpanMs));
            else if (track.Key == kColPct)
                label->SetText(FormatPct(t, m_Mode, m_PassProvider.Total()));
        });

    // A header click selects the metric (CPU/GPU/%); the % column is the combined
    // (max) view == Mode::Both. SetSortMode re-asserts a descending glyph + rebuilds.
    m_Table->SetOnSort([this](StringId key, SortDirection) { SetSortMode(ModeForKey(key)); });
    m_Table->SetOnLayoutChanged([this]() { SaveColumnLayout(); });
    m_Table->SetSortIndicator(KeyForMode(m_Mode), SortDirection::Descending);

    // Selection highlight + keyboard: stable name-hash ids keep the selection across
    // re-sort / scrub. Up/Down navigates; Ctrl/Cmd+C copies the row as TSV.
    ListView& body = m_Table->Body();
    body.SetSelectionModel(&m_PassSelection);
    body.SetFocusable(true);
    body.RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
        if (e.Key == Input::kKeyCode_C && Input::IsPrimaryShortcutModifier(e.Mods))
        {
            const int idx = m_Table->Body().GetSelectedIndex();
            if (idx < 0 || idx >= m_PassProvider.Size())
                return;
            const auto& t = m_PassProvider.Row(idx);
            std::string text = t.Name;
            text += '\t'; text += FormatMs(t.CpuMs);
            text += '\t'; text += FormatMs(t.GpuSpanMs);
            text += '\t'; text += FormatPct(t, m_Mode, m_PassProvider.Total());
            if (UIManager* ui = GetOwnerManager())
            {
                if (auto* platform = ui->GetPlatform())
                    platform->SetClipboardText(text.c_str());
            }
            e.Stop();
            return;
        }

        if (e.Mods != 0 || (e.Key != Input::kKeyCode_Up && e.Key != Input::kKeyCode_Down))
            return;
        ListView& b = m_Table->Body();
        const int count = b.GetItemCount();
        if (count <= 0)
            return;
        int index = b.GetSelectedIndex();
        if (index < 0)
            index = 0;
        index += (e.Key == Input::kKeyCode_Up) ? -1 : 1;
        index = std::clamp(index, 0, count - 1);
        b.SetSelectedIndexKeyboard(index, /*extendRange*/ false, /*scrollIntoView*/ true);
        e.Stop();
    });
}

void VisualProfilerPanel::ApplyBudgetLine()
{
    const float value = m_ShowBudgetLine ? m_BudgetMs : 0.0f;
    if (m_CpuChart)
    {
        m_CpuChart->SetReferenceLine(value, kBudgetLineColor);
        m_CpuChart->SetMinYMax(value);
    }
    if (m_GpuChart)
    {
        m_GpuChart->SetReferenceLine(value, kBudgetLineColor);
        m_GpuChart->SetMinYMax(value);
    }
    if (m_BudgetButton)
        m_BudgetButton->SetText(m_ShowBudgetLine ? "Budget: On" : "Budget: Off");
}

void VisualProfilerPanel::Update(Rendering::RenderGraph::RGFrame* renderGraph)
{
    m_LastRenderGraph = renderGraph;

    if (GetLayoutWidth() <= 0.0f || GetLayoutHeight() <= 0.0f)
        return;

    // Auto-arm only on the first frame the panel sees a render graph — past
    // that, respect external toggles (e.g. the RenderGraph panel's Profile
    // button) instead of clobbering them every frame.
    if (renderGraph && !m_AutoArmedOnce)
    {
        if (!m_Paused && !renderGraph->ProfilingEnabled())
            renderGraph->SetProfilingEnabled(true);
        m_AutoArmedOnce = true;
    }

    // Sample the rolling frame-time series every frame (before the row-rebuild
    // throttle below) so the live charts stay smooth.
    const bool armedLive = renderGraph && renderGraph->ProfilingEnabled() && !m_Paused;
    if (armedLive)
    {
        AccumulateLiveHistory(renderGraph);
        // Repaint the charts so the rolling line animates (the provider is
        // re-pulled on visual-dirty repaint).
        if (m_BoundChart) m_BoundChart->MarkDirty(UIElement::VisualDirty);
        if (m_CpuChart)   m_CpuChart->MarkDirty(UIElement::VisualDirty);
        if (m_GpuChart)   m_GpuChart->MarkDirty(UIElement::VisualDirty);
    }

    // Keep arm/status state responsive every frame.
    RefreshToolbar(renderGraph);

    // When paused without an active cursor, hold the last row snapshot so
    // the user can inspect it. When a cursor is set, RefreshRows still needs
    // to run once on demand (m_CursorDirty), but not every frame.
    if (m_Paused && !m_CursorDirty)
        return;
    m_CursorDirty = false;

    // Throttle live-mode row rebuild to reduce DOM churn.
    if (!m_Paused && (++m_FrameCounter & 7) != 0)
        return;

    const bool armed = renderGraph && renderGraph->ProfilingEnabled();
    if (!armed && !m_Paused)
    {
        if (m_PassProvider.Size() > 0)
        {
            m_PassProvider.SetRows({}, 0.0);
            m_PassSelection.Clear();
            if (m_Table)
                m_Table->Refresh();
        }
        // Drop the stale rolling series so the charts empty out while disarmed.
        if (!m_LiveCpuHistory.empty() || !m_LiveGpuHistory.empty())
        {
            m_LiveCpuHistory.clear();
            m_LiveGpuHistory.clear();
            m_LivePassTimings.clear();
            if (m_BoundChart) m_BoundChart->MarkDirty(UIElement::VisualDirty);
            if (m_CpuChart)   m_CpuChart->MarkDirty(UIElement::VisualDirty);
            if (m_GpuChart)   m_GpuChart->MarkDirty(UIElement::VisualDirty);
        }
        return;
    }

    RebuildPassSnapshot(renderGraph);
}

void VisualProfilerPanel::RefreshToolbar(Rendering::RenderGraph::RGFrame* renderGraph)
{
    const bool armed = renderGraph && renderGraph->ProfilingEnabled();
    if (m_PauseButton)
    {
        if (m_Paused)
        {
            m_PauseButton->SetText("Resume");
            m_PauseButton->AddClass("paused");
        }
        else
        {
            m_PauseButton->SetText("Pause");
            m_PauseButton->RemoveClass("paused");
        }
    }
    if (m_StatusLabel)
    {
        if (!renderGraph)
            m_StatusLabel->SetText("no render graph");
        else if (!armed)
            m_StatusLabel->SetText("profiler disarmed");
        else
        {
            // LastFrameTimings is the previous resolved slot; the charts roll
            // that locally in AccumulateLiveHistory.
            auto timings = renderGraph->LastFrameTimings();
            double sumCpu = 0.0;
            for (const auto& t : timings) sumCpu += t.CpuMs;
            const bool span = renderGraph->TimingSemantics() == Rendering::TimestampSemantics::EncoderSpan;
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          span ? "cpu %.2f ms  gpu <= %.2f ms (overlapping encoder spans; * = shared)"
                                 "  (%zu passes)"
                               : "cpu %.2f ms  gpu %.2f ms  (%zu passes)",
                          sumCpu, SumDistinctGpuMs(timings), timings.size());
            m_StatusLabel->SetText(buf);
        }
    }

    if (m_BoundChart)
    {
        m_BoundChart->RemoveClass("cpu-bound");
        m_BoundChart->RemoveClass("gpu-bound");
        m_BoundChart->RemoveClass("balanced");
        if (m_BoundLabelRow)
        {
            m_BoundLabelRow->RemoveClass("cpu-bound");
            m_BoundLabelRow->RemoveClass("gpu-bound");
            m_BoundLabelRow->RemoveClass("balanced");
        }
        if (!armed && !m_Paused)
        {
            m_BoundChart->SetYRange(-1.0f, 1.0f);
            return;
        }

        // Classify from the active series: the frozen snapshot when paused, the
        // rolling live series otherwise.
        const std::vector<float>& cpuHist = m_Paused ? m_FrozenCpuHistory : m_LiveCpuHistory;
        const std::vector<float>& gpuHist = m_Paused ? m_FrozenGpuHistory : m_LiveGpuHistory;
        const std::size_t n = std::min(cpuHist.size(), gpuHist.size());

        auto sampleCpu = [&](std::size_t i) -> double { return cpuHist[i]; };
        auto sampleGpu = [&](std::size_t i) -> double { return gpuHist[i]; };

        float maxAbs = 0.0f;
        for (std::size_t i = 0; i < n; ++i)
        {
            const float d = static_cast<float>(std::abs(sampleCpu(i) - sampleGpu(i)));
            if (d > maxAbs) maxAbs = d;
        }
        const float range = std::max(0.25f, maxAbs * 1.1f);
        m_BoundChart->SetYRange(-range, range);

        // Classify the current (scrubbed or latest) frame.
        if (n > 0)
        {
            std::size_t frameIdx = n - 1;
            if (m_CursorFrame >= 0 && static_cast<std::size_t>(m_CursorFrame) < n)
                frameIdx = static_cast<std::size_t>(m_CursorFrame);
            const double cpu = sampleCpu(frameIdx);
            const double gpu = sampleGpu(frameIdx);
            const double maxT = std::max(cpu, gpu);
            const double rel = maxT > 0.0 ? (std::abs(cpu - gpu) / maxT) : 0.0;
            const char* boundClass = "balanced";
            if (rel < 0.05)
                boundClass = "balanced";
            else if (cpu > gpu)
                boundClass = "cpu-bound";
            else
                boundClass = "gpu-bound";
            m_BoundChart->AddClass(boundClass);
            if (m_BoundLabelRow)
                m_BoundLabelRow->AddClass(boundClass);
        }
    }
}

void VisualProfilerPanel::RebuildPassSnapshot(Rendering::RenderGraph::RGFrame* renderGraph)
{
    std::vector<Rendering::RenderGraph::RGFrame::RGPassTiming> timings;
    if (m_Paused && !m_FrozenPassTimings.empty())
    {
        // Scrubbing drives which frame's pass list is shown (unchanged from RefreshRows).
        const std::size_t idx = (m_CursorFrame >= 0)
            ? std::min(static_cast<std::size_t>(m_CursorFrame), m_FrozenPassTimings.size() - 1)
            : m_FrozenPassTimings.size() - 1;
        timings = m_FrozenPassTimings[idx];
    }
    else if (renderGraph)
    {
        // RenderGraph exposes only the last resolved frame's per-pass timings; the
        // per-history-index scrub source is a Stage 2e item, so a live cursor
        // still reads the latest frame.
        timings = renderGraph->LastFrameTimings();
    }
    if (timings.empty())
        return; // hold the last snapshot (matches the old RefreshRows early-out)

    // Sort descending by the metric currently in focus.
    std::sort(timings.begin(), timings.end(),
              [this](const auto& a, const auto& b) { return MetricFor(a, m_Mode) > MetricFor(b, m_Mode); });

    const double total = (m_Mode == Mode::Gpu)
                             ? SumDistinctGpuMs(timings)
                             : [&] {
                                   double sum = 0.0;
                                   for (const auto& t : timings)
                                       sum += MetricFor(t, m_Mode);
                                   return sum;
                               }();

    m_PassProvider.SetRows(std::move(timings), total);
    if (m_Table)
        m_Table->Refresh();
}

void VisualProfilerPanel::SetSortMode(Mode mode)
{
    m_Mode = mode;
    if (m_ModeButton)
        m_ModeButton->SetText(ModeLabel(m_Mode));
    if (m_Table)
        m_Table->SetSortIndicator(KeyForMode(m_Mode), SortDirection::Descending);
    m_CursorDirty = true;
    m_FrameCounter = 0;
    RebuildPassSnapshot(m_LastRenderGraph);
}

void VisualProfilerPanel::TogglePause()
{
    if (m_Paused)
    {
        if (m_LastRenderGraph)
            m_LastRenderGraph->SetProfilingEnabled(true);
        ClearCursor();
    }
    else
    {
        if (!m_LastRenderGraph)
            return;
        SnapshotHistoryForFreeze(m_LastRenderGraph);
        m_Paused = true;
        m_LastRenderGraph->SetProfilingEnabled(false);
    }
}

void VisualProfilerPanel::AttachChartScrubHandlers(ChartView* chart)
{
    if (!chart)
        return;
    chart->RegisterEventHandler(kEventMouseDown, [this, chart](UIEvent& e) {
        if (e.Button != 0 && e.Button != 1)
            return;

        constexpr float kTolerancePx = 8.0f;
        const auto now = std::chrono::steady_clock::now();
        bool isDouble = false;
        if (m_HasLastChartClick)
        {
            isDouble = ((now - m_LastChartClickTime) < GameEngine::Platform::GetDoubleClickInterval()) &&
                       (std::abs(e.X - m_LastChartClickX) <= kTolerancePx) &&
                       (std::abs(e.Y - m_LastChartClickY) <= kTolerancePx);
        }
        m_LastChartClickTime = now;
        m_LastChartClickX = e.X;
        m_LastChartClickY = e.Y;
        m_HasLastChartClick = true;

        if (isDouble)
        {
            if (m_LastRenderGraph)
                m_LastRenderGraph->SetProfilingEnabled(true);
            ClearCursor();
            m_HasLastChartClick = false;
            e.Stop();
            return;
        }

        m_ScrubbingChart = chart;
        e.Capture(chart);
        const float localX = e.X - chart->GetLayoutX();
        OnChartClicked(localX, chart->GetLayoutWidth());
        e.Stop();
    });
    chart->RegisterEventHandler(kEventMouseMove, [this, chart](UIEvent& e) {
        if (m_ScrubbingChart != chart)
            return;
        const float localX = e.X - chart->GetLayoutX();
        OnChartClicked(localX, chart->GetLayoutWidth());
        e.Stop();
    });
    chart->RegisterEventHandler(kEventMouseUp, [this, chart](UIEvent& e) {
        if (m_ScrubbingChart != chart)
            return;
        if (e.Button != 0 && e.Button != 1)
            return;
        m_ScrubbingChart = nullptr;
        e.Stop();
    });
}

void VisualProfilerPanel::AccumulateLiveHistory(Rendering::RenderGraph::RGFrame* renderGraph)
{
    if (!renderGraph)
        return;
    auto timings = renderGraph->LastFrameTimings();
    if (timings.empty())
        return;
    double sumCpu = 0.0;
    for (const auto& t : timings) sumCpu += t.CpuMs;
    m_LiveCpuHistory.push_back(static_cast<float>(sumCpu));
    m_LiveGpuHistory.push_back(static_cast<float>(SumDistinctGpuMs(timings)));
    // Keep the per-pass breakdown aligned 1:1 with the chart samples so scrubbing
    // a frozen frame can show that frame's passes (not just the latest).
    m_LivePassTimings.push_back(std::move(timings));
    if (m_LiveCpuHistory.size() > kMaxLiveHistory)
        m_LiveCpuHistory.erase(m_LiveCpuHistory.begin(),
                               m_LiveCpuHistory.end() - kMaxLiveHistory);
    if (m_LiveGpuHistory.size() > kMaxLiveHistory)
        m_LiveGpuHistory.erase(m_LiveGpuHistory.begin(),
                               m_LiveGpuHistory.end() - kMaxLiveHistory);
    if (m_LivePassTimings.size() > kMaxLiveHistory)
        m_LivePassTimings.erase(m_LivePassTimings.begin(),
                                m_LivePassTimings.end() - kMaxLiveHistory);
}

void VisualProfilerPanel::SnapshotHistoryForFreeze(Rendering::RenderGraph::RGFrame* renderGraph)
{
    // Freeze the rolling live curves AND their per-frame pass breakdowns so the
    // charts keep drawing and scrubbing any frame shows that frame's passes.
    // m_LivePassTimings is index-aligned with the CPU/GPU history.
    m_FrozenCpuHistory = m_LiveCpuHistory;
    m_FrozenGpuHistory = m_LiveGpuHistory;
    m_FrozenPassTimings = m_LivePassTimings;
    if (!m_FrozenPassTimings.empty())
        return;

    // Fallback: no rolling history yet (e.g. just armed) — capture the single
    // latest frame so the list isn't empty.
    if (!renderGraph)
        return;
    auto timings = renderGraph->LastFrameTimings();
    if (timings.empty())
        return;
    double sumCpu = 0.0;
    for (const auto& t : timings) sumCpu += t.CpuMs;
    m_FrozenCpuHistory.assign(1, static_cast<float>(sumCpu));
    m_FrozenGpuHistory.assign(1, static_cast<float>(SumDistinctGpuMs(timings)));
    m_FrozenPassTimings.push_back(std::move(timings));
}

void VisualProfilerPanel::OnChartClicked(float localX, float chartWidth)
{
    if (chartWidth <= 0.0f)
        return;
    if (!m_Paused)
    {
        SnapshotHistoryForFreeze(m_LastRenderGraph);
        m_Paused = true;
        if (m_LastRenderGraph && m_LastRenderGraph->ProfilingEnabled())
            m_LastRenderGraph->SetProfilingEnabled(false);
    }
    const std::size_t n = m_FrozenCpuHistory.size();
    if (n == 0)
        return;
    const float t = std::clamp(localX / chartWidth, 0.0f, 1.0f);
    const int frame = static_cast<int>(t * static_cast<float>(n - 1) + 0.5f);
    SelectCursorFrame(frame);
}

void VisualProfilerPanel::SelectCursorFrame(int frame)
{
    m_CursorFrame = frame;
    m_CursorDirty = true;
    if (m_BoundChart)
        m_BoundChart->SetCursorIndex(frame);
    if (m_CpuChart)
        m_CpuChart->SetCursorIndex(frame);
    if (m_GpuChart)
        m_GpuChart->SetCursorIndex(frame);
}

void VisualProfilerPanel::ClearCursor()
{
    m_Paused = false;
    m_CursorFrame = -1;
    m_CursorDirty = true;
    m_FrozenCpuHistory.clear();
    m_FrozenGpuHistory.clear();
    m_FrozenPassTimings.clear();
    if (m_BoundChart)
        m_BoundChart->SetCursorIndex(-1);
    if (m_CpuChart)
        m_CpuChart->SetCursorIndex(-1);
    if (m_GpuChart)
        m_GpuChart->SetCursorIndex(-1);
}

void VisualProfilerPanel::SaveColumnLayout()
{
    if (!m_Table)
        return;
    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    prefs.Load();
    Editor::SaveAxisLayout(prefs, m_ColumnLayoutKey, m_Table->Columns());
    prefs.Save();
}

void VisualProfilerPanel::OnPostLayout()
{
    if (!m_StyleAttached && !m_StyleLoadScheduled && GetOwnerManager())
    {
        m_StyleLoadScheduled = true;
        PostAction([this]() { LoadAndAttachPanelStyle(); });
    }
}

void VisualProfilerPanel::OnMountVisibilityChanged(bool isVisible)
{
    // Per-pass GPU profiling inserts a timestamp query around every render pass
    // and reads it back each frame — costs several ms per frame. Disable when
    // the panel is hidden; the auto-arm path in Update() re-enables it on show.
    if (!isVisible)
    {
        if (m_LastRenderGraph && m_LastRenderGraph->ProfilingEnabled())
            m_LastRenderGraph->SetProfilingEnabled(false);
        m_AutoArmedOnce = false;
    }
}

void VisualProfilerPanel::LoadAndAttachPanelStyle()
{
    m_StyleLoadScheduled = false;
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;
    auto& am = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path styleAssetPath =
        std::filesystem::path("UI") / "panels" / "VisualProfilerPanel.css";
    const GUID styleGuid = am.ResolveAssetGuid(styleAssetPath, GameEngine::kAssetSourceAliasEditor);
    if (styleGuid.IsNull() || m_PanelStyleLoadHandle)
        return;
    m_PanelStyleLoadHandle = std::make_unique<AssetLoadHandle>(
        am.LoadAsset(styleGuid,
                     [this, post = GetPostHandle(), styleGuid](Result<SharedPtr<Asset>, AssetError> r)
                     {
                         if (!r.IsOk() || !r.Value() || r.Value()->GetType() != AssetType::UIStyle)
                         {
                             post.Post([this]() { m_PanelStyleLoadHandle.reset(); });
                             return;
                         }
                         post.Post([this, styleGuid]()
                         {
                             UIManager* ui2 = GetOwnerManager();
                             if (!ui2)
                                 return;
                             auto& am2 = EngineCore::GetInstance().GetAssetManager();
                             auto a2 = am2.GetAsset(styleGuid);
                             if (a2 && a2->GetType() == AssetType::UIStyle)
                             {
                                 (void)ui2->AttachStyleToSubtreeFromAsset(
                                     this, *static_cast<UIStyleAsset*>(a2.get()));
                                 m_StyleAttached = true;
                             }
                         });
                     },
                     AssetLoadPriority::High));
}

} // namespace GameEngine
