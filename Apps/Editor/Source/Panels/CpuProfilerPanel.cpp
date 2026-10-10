#include "Panels/CpuProfilerPanel.h"

#include "Platform/SystemMetrics.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Core/CpuProfiler.h"
#include "Core/Engine.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/ChartView.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/UIElement.h"
#include "Input/KeyCodes.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

namespace GameEngine {

namespace {

constexpr std::size_t kMaxScopeRows = 64;

std::string FormatMs(double ms)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%7.2f ms", ms);
    return std::string(buf);
}

std::string FormatCount(std::uint32_t count)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "x%u", count);
    return std::string(buf);
}

std::string IndentedName(const std::string& name, int depth)
{
    // Two spaces per depth level — cheap visual hierarchy without a tree widget.
    std::string out;
    out.reserve(name.size() + static_cast<std::size_t>(depth) * 2);
    for (int i = 0; i < depth; ++i)
        out.append("  ");
    out.append(name);
    return out;
}

} // namespace

CpuProfilerPanel::~CpuProfilerPanel()
{
    // The pending asset callbacks capture this panel; withdraw them before it goes
    // away (destroying an AssetLoadHandle does not).
    if (m_PanelStyleLoadHandle)
        m_PanelStyleLoadHandle->Cancel();
}

CpuProfilerPanel::CpuProfilerPanel()
    : DockPanel("CPU Profiler")
{
    AddClass("cpu-profiler-panel");
    BuildUI();
}

void CpuProfilerPanel::BuildUI()
{
    // Toolbar
    auto toolbar = std::make_unique<UIElement>();
    toolbar->AddClass("cpu-profiler-toolbar");

    auto pauseBtn = std::make_unique<Button>();
    pauseBtn->AddClass("small");
    pauseBtn->AddClass("secondary");
    pauseBtn->AddClass("cpu-profiler-btn-pause");
    pauseBtn->SetText("Pause");
    pauseBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { TogglePause(); });
    m_EnableButton = pauseBtn.get();
    toolbar->AddChild(std::move(pauseBtn));

    auto dumpBtn = std::make_unique<Button>();
    dumpBtn->AddClass("small");
    dumpBtn->AddClass("secondary");
    dumpBtn->AddClass("cpu-profiler-btn-dump");
    dumpBtn->SetText("Dump to Log");
    dumpBtn->RegisterEventHandler(kEventButtonClick, [](UIEvent&) { Profiling::CpuProfiler::Get().RequestDump(); });
    toolbar->AddChild(std::move(dumpBtn));

    auto header = std::make_unique<Label>();
    header->AddClass("cpu-profiler-header");
    header->SetText("CPU profiler disabled");
    m_HeaderLabel = header.get();
    toolbar->AddChild(std::move(header));

    AddChild(std::move(toolbar));

    // Frame-time chart fed by CpuProfiler's own per-frame history so the
    // chart length and the scrub cursor map 1:1 onto captured tree frames.
    auto chart = std::make_unique<ChartView>();
    chart->AddClass("cpu-profiler-chart");
    chart->SetMode(ChartView::Mode::Line);
    chart->SetProvider([this](std::vector<float>& out) {
        auto& prof = Profiling::CpuProfiler::Get();
        if (m_Paused)
        {
            out = m_FrozenCpuHistory;
            return;
        }
        if (prof.IsEnabled())
            prof.CopyFrameHistoryTotals(out);
        else
            out.clear();
    });
    chart->SetValueFormatter([](float v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f ms", v);
        return std::string(buf);
    });
    m_Chart = chart.get();
    AddChild(std::move(chart));
    AttachChartScrubHandlers();

    // Scope list header row — clickable column labels for sorting.
    auto listHeader = std::make_unique<UIElement>();
    listHeader->AddClass("cpu-profiler-list-header");
    {
        auto makeSortHeader = [this](const char* baseClass, SortColumn column,
                                     Label*& out) {
            auto label = std::make_unique<Label>();
            label->AddClass(baseClass);
            label->AddClass("cpu-profiler-sort-header");
            label->RegisterEventHandler(kEventMouseDown, [this, column](UIEvent& e) {
                if (e.Button != 0)
                    return;
                OnSortHeaderClicked(column);
                e.Stop();
            });
            out = label.get();
            return label;
        };

        listHeader->AddChild(makeSortHeader("cpu-profiler-col-name",
                                            SortColumn::Name, m_SortNameLabel));
        listHeader->AddChild(makeSortHeader("cpu-profiler-col-total",
                                            SortColumn::Total, m_SortTotalLabel));
        listHeader->AddChild(makeSortHeader("cpu-profiler-col-self",
                                            SortColumn::Self, m_SortSelfLabel));
        listHeader->AddChild(makeSortHeader("cpu-profiler-col-count",
                                            SortColumn::Calls, m_SortCallsLabel));
    }
    AddChild(std::move(listHeader));
    UpdateSortHeaderLabels();

    auto rowsScroll = std::make_unique<ScrollView>();
    rowsScroll->AddClass("cpu-profiler-rows");
    m_RowsScroll = rowsScroll.get();
    m_RowsContainer = rowsScroll->GetViewport();
    if (m_RowsContainer)
    {
        m_RowsContainer->SetFocusable(true);
        m_RowsContainer->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
            OnRowsKeyDown(e);
        });
    }
    AddChild(std::move(rowsScroll));

    RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
        if (e.Key != Input::kKeyCode_Space)
            return;
        TogglePause();
        e.Stop();
    });
}

void CpuProfilerPanel::SelectScope(const std::string& name)
{
    if (m_SelectedScopeName == name)
        m_SelectedScopeName.clear();
    else
        m_SelectedScopeName = name;
    ApplySelectionClasses();
}

void CpuProfilerPanel::ApplySelectionClasses()
{
    for (auto& row : m_Rows)
    {
        if (!row.Root)
            continue;
        if (!m_SelectedScopeName.empty() && row.Name == m_SelectedScopeName)
            row.Root->AddClass("selected");
        else
            row.Root->RemoveClass("selected");
    }
}

void CpuProfilerPanel::OnRowsKeyDown(UIEvent& e)
{
    if (e.Key == Input::kKeyCode_C && Input::IsPrimaryShortcutModifier(e.Mods))
    {
        if (m_SelectedScopeName.empty())
            return;
        const ScopeRow* selected = nullptr;
        for (const auto& row : m_Rows)
        {
            if (row.Name == m_SelectedScopeName) { selected = &row; break; }
        }
        if (!selected)
            return;
        std::string text = selected->Name;
        if (selected->TotalLabel) { text += '\t'; text += selected->TotalLabel->GetText(); }
        if (selected->SelfLabel)  { text += '\t'; text += selected->SelfLabel->GetText(); }
        if (selected->CountLabel) { text += '\t'; text += selected->CountLabel->GetText(); }
        if (UIManager* ui = GetOwnerManager())
        {
            if (auto* platform = ui->GetPlatform())
                platform->SetClipboardText(text.c_str());
        }
        e.Stop();
        return;
    }

    if (e.Key != Input::kKeyCode_Up && e.Key != Input::kKeyCode_Down)
        return;

    std::vector<std::size_t> visible;
    visible.reserve(m_Rows.size());
    for (std::size_t i = 0; i < m_Rows.size(); ++i)
    {
        if (!m_Rows[i].Root)
            continue;
        if (m_Rows[i].Root->HasClass("hidden"))
            continue;
        if (m_Rows[i].Name.empty())
            continue;
        visible.push_back(i);
    }
    if (visible.empty())
        return;

    int cursor = -1;
    if (!m_SelectedScopeName.empty())
    {
        for (std::size_t vi = 0; vi < visible.size(); ++vi)
            if (m_Rows[visible[vi]].Name == m_SelectedScopeName) { cursor = static_cast<int>(vi); break; }
    }

    int target = cursor;
    if (e.Key == Input::kKeyCode_Up)
        target = (cursor <= 0) ? 0 : cursor - 1;
    else
        target = (cursor < 0) ? 0 : std::min(cursor + 1, static_cast<int>(visible.size()) - 1);

    if (target < 0 || target >= static_cast<int>(visible.size()))
        return;

    const std::string& name = m_Rows[visible[target]].Name;
    if (m_SelectedScopeName != name)
    {
        m_SelectedScopeName = name;
        ApplySelectionClasses();
    }

    if (m_RowsScroll && m_RowsContainer)
    {
        UIElement* rowEl = m_Rows[visible[target]].Root;
        const float rowTop = rowEl->GetLayoutY() - m_RowsContainer->GetLayoutY();
        const float rowBottom = rowTop + rowEl->GetLayoutHeight();
        const float viewTop = m_RowsScroll->GetScrollY();
        const float viewHeight = m_RowsScroll->GetViewportHeight();
        const float viewBottom = viewTop + viewHeight;
        if (rowTop < viewTop)
            m_RowsScroll->SetScrollY(rowTop);
        else if (rowBottom > viewBottom)
            m_RowsScroll->SetScrollY(rowBottom - viewHeight);
    }

    e.Stop();
}

void CpuProfilerPanel::Update()
{
    if (GetLayoutWidth() <= 0.0f || GetLayoutHeight() <= 0.0f)
        return;

    // Auto-arm whenever the panel is visible and not explicitly paused —
    // matches the Monitors panel's "always running while open" model.
    auto& prof = Profiling::CpuProfiler::Get();
    if (!m_Paused && !prof.IsEnabled())
        prof.SetEnabled(true);

    UpdateHeaderLabel();

    // When paused, only rebuild rows when the cursor moved. Rows otherwise
    // hold their state so the user can read a snapshot instead of watching
    // live values overwrite the scrubbed frame.
    if (m_Paused)
    {
        if (!m_CursorDirty)
            return;
        m_CursorDirty = false;
        RefreshRows();
        return;
    }

    if ((++m_FrameCounter & 7) != 0)
        return;

    if (!prof.IsEnabled())
    {
        // Clear rows so stale data doesn't linger after disabling.
        if (m_RowsContainer && !m_Rows.empty())
        {
            m_RowsContainer->RemoveAllChildren();
            m_Rows.clear();
        }
        return;
    }

    RefreshRows();
}

void CpuProfilerPanel::UpdateHeaderLabel()
{
    auto& prof = Profiling::CpuProfiler::Get();

    if (m_EnableButton)
    {
        if (m_Paused)
        {
            m_EnableButton->SetText("Resume");
            m_EnableButton->AddClass("paused");
        }
        else
        {
            m_EnableButton->SetText("Pause");
            m_EnableButton->RemoveClass("paused");
        }
    }

    if (!m_HeaderLabel)
        return;
    if (m_Paused && m_CursorFrame >= 0 &&
        static_cast<std::size_t>(m_CursorFrame) < m_FrozenCpuHistory.size())
    {
        const float v = m_FrozenCpuHistory[m_CursorFrame];
        const int offsetFromEnd = static_cast<int>(m_FrozenCpuHistory.size()) - 1 - m_CursorFrame;
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "scrub: %.2f ms  (frame -%d)",
                      v, offsetFromEnd);
        m_HeaderLabel->SetText(buf);
        return;
    }
    m_HeaderLabel->SetText(prof.IsEnabled() ? "CPU profiler enabled"
                                            : "CPU profiler disabled");
}

void CpuProfilerPanel::TogglePause()
{
    if (m_Paused)
    {
        ClearCursor();
    }
    else
    {
        SnapshotHistoryForFreeze();
        m_Paused = true;
        Profiling::CpuProfiler::Get().SetEnabled(false);
    }
}

void CpuProfilerPanel::AttachChartScrubHandlers()
{
    if (!m_Chart)
        return;
    m_Chart->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
        if (e.Button != 0 && e.Button != 1)
            return;
        if (!m_Chart)
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
            ClearCursor();
            m_HasLastChartClick = false;
            e.Stop();
            return;
        }

        m_Scrubbing = true;
        e.Capture(m_Chart);
        const float localX = e.X - m_Chart->GetLayoutX();
        OnChartClicked(localX, m_Chart->GetLayoutWidth());
        e.Stop();
    });
    m_Chart->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) {
        if (!m_Scrubbing || !m_Chart)
            return;
        const float localX = e.X - m_Chart->GetLayoutX();
        OnChartClicked(localX, m_Chart->GetLayoutWidth());
        e.Stop();
    });
    m_Chart->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
        if (!m_Scrubbing)
            return;
        if (e.Button != 0 && e.Button != 1)
            return;
        m_Scrubbing = false;
        e.Stop();
    });
}

void CpuProfilerPanel::SnapshotHistoryForFreeze()
{
    auto& prof = Profiling::CpuProfiler::Get();
    prof.SnapshotFrameHistory(m_FrozenFrameHistory);
    m_FrozenCpuHistory.clear();
    m_FrozenCpuHistory.reserve(m_FrozenFrameHistory.size());
    for (const auto& e : m_FrozenFrameHistory)
        m_FrozenCpuHistory.push_back(static_cast<float>(e.totalMs));
    m_ChartFrozen = true;
}

void CpuProfilerPanel::OnChartClicked(float localX, float chartWidth)
{
    if (chartWidth <= 0.0f)
        return;
    if (!m_Paused)
    {
        SnapshotHistoryForFreeze();
        m_Paused = true;
        Profiling::CpuProfiler::Get().SetEnabled(false);
    }
    const std::size_t n = m_FrozenCpuHistory.size();
    if (n == 0)
        return;
    const float t = std::clamp(localX / chartWidth, 0.0f, 1.0f);
    const int frame = static_cast<int>(t * static_cast<float>(n - 1) + 0.5f);
    m_CursorFrame = frame;
    m_CursorDirty = true;
    if (m_Chart)
        m_Chart->SetCursorIndex(frame);
    UpdateHeaderLabel();
}

void CpuProfilerPanel::ClearCursor()
{
    m_Paused = false;
    m_CursorFrame = -1;
    m_CursorDirty = true;
    m_ChartFrozen = false;
    m_FrozenCpuHistory.clear();
    m_FrozenFrameHistory.clear();
    if (m_Chart)
        m_Chart->SetCursorIndex(-1);
    UpdateHeaderLabel();
}

void CpuProfilerPanel::OnSortHeaderClicked(SortColumn column)
{
    if (m_SortColumn == column)
    {
        m_SortAscending = !m_SortAscending;
    }
    else
    {
        m_SortColumn = column;
        // Numeric columns default to descending (largest first); name is ascending.
        m_SortAscending = (column == SortColumn::Name);
    }
    UpdateSortHeaderLabels();
    RefreshRows();
}

void CpuProfilerPanel::UpdateSortHeaderLabels()
{
    auto apply = [this](Label* label, const char* base, SortColumn column) {
        if (!label)
            return;
        std::string text = base;
        if (m_SortColumn == column)
            text += m_SortAscending ? "  \xE2\x96\xB2" : "  \xE2\x96\xBC";
        label->SetText(text);
    };
    apply(m_SortNameLabel, "Scope", SortColumn::Name);
    apply(m_SortTotalLabel, "Total", SortColumn::Total);
    apply(m_SortSelfLabel, "Self", SortColumn::Self);
    apply(m_SortCallsLabel, "Calls", SortColumn::Calls);
}

void CpuProfilerPanel::RefreshRows()
{
    if (!m_RowsContainer)
        return;

    // Pull the preorder-flattened call tree. When paused, read from the
    // frame-history snapshot at the current cursor frame (the frozen
    // history is captured at the moment scrubbing starts). Live mode uses
    // CopyFrameTree which reads the profiler's "last completed frame" buffer
    // so rows are never mid-scope.
    std::vector<Profiling::CpuProfiler::TreeRow> rows;
    if (m_Paused && !m_FrozenFrameHistory.empty())
    {
        const std::size_t n = m_FrozenFrameHistory.size();
        const std::size_t idx = (m_CursorFrame >= 0)
            ? std::min(static_cast<std::size_t>(m_CursorFrame), n - 1)
            : (n - 1);
        rows = m_FrozenFrameHistory[idx].rows;
    }
    else
    {
        Profiling::CpuProfiler::Get().CopyFrameTree(rows);
    }

    // When sorting by a non-default column, flatten the tree (drop depth so
    // rows aren't indented in confusing order) and sort globally. The default
    // Total/descending sort with depth-preserved indentation gives a normal
    // call-tree view; clicking a header switches to a hot-list.
    const bool useFlatSort = !(m_SortColumn == SortColumn::Total && !m_SortAscending);
    if (useFlatSort && !rows.empty())
    {
        for (auto& r : rows)
            r.depth = 0;
        auto cmp = [this](const Profiling::CpuProfiler::TreeRow& a,
                          const Profiling::CpuProfiler::TreeRow& b) {
            switch (m_SortColumn)
            {
            case SortColumn::Name:
                return m_SortAscending ? (a.name < b.name) : (a.name > b.name);
            case SortColumn::Total:
                return m_SortAscending ? (a.totalMs < b.totalMs) : (a.totalMs > b.totalMs);
            case SortColumn::Self:
                return m_SortAscending ? (a.selfMs < b.selfMs) : (a.selfMs > b.selfMs);
            case SortColumn::Calls:
                return m_SortAscending ? (a.callCount < b.callCount) : (a.callCount > b.callCount);
            }
            return false;
        };
        std::sort(rows.begin(), rows.end(), cmp);
    }

    // Fall back to the flat aggregate when the tree is empty: the completed
    // frame ran its scopes on other threads only, so the main-thread tree has
    // no rows while the aggregate does. Both views describe the same completed
    // frame, so an empty tree is not a "too early" state to wait out.
    std::vector<std::pair<std::string, Profiling::CpuProfiler::Sample>> flatFallback;
    if (rows.empty())
    {
        std::vector<std::pair<std::string_view, Profiling::CpuProfiler::Sample>> samples;
        Profiling::CpuProfiler::Get().CopyFrameSamples(samples);
        auto cmp = [this](const auto& a, const auto& b) {
            switch (m_SortColumn)
            {
            case SortColumn::Name:
                return m_SortAscending ? (a.first < b.first) : (a.first > b.first);
            case SortColumn::Total:
            case SortColumn::Self:
                return m_SortAscending ? (a.second.totalMs < b.second.totalMs)
                                       : (a.second.totalMs > b.second.totalMs);
            case SortColumn::Calls:
                return m_SortAscending ? (a.second.count < b.second.count)
                                       : (a.second.count > b.second.count);
            }
            return false;
        };
        std::sort(samples.begin(), samples.end(), cmp);
        flatFallback.reserve(samples.size());
        for (const auto& s : samples)
        {
            flatFallback.emplace_back(std::string(s.first), s.second);
        }
    }

    const std::size_t n = std::min(rows.empty() ? flatFallback.size() : rows.size(),
                                   kMaxScopeRows);

    // Grow row pool if needed.
    while (m_Rows.size() < n)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("cpu-profiler-row");

        auto name = std::make_unique<Label>();
        name->AddClass("cpu-profiler-col-name");
        Label* namePtr = name.get();
        row->AddChild(std::move(name));

        auto total = std::make_unique<Label>();
        total->AddClass("cpu-profiler-col-total");
        Label* totalPtr = total.get();
        row->AddChild(std::move(total));

        auto self = std::make_unique<Label>();
        self->AddClass("cpu-profiler-col-self");
        Label* selfPtr = self.get();
        row->AddChild(std::move(self));

        auto count = std::make_unique<Label>();
        count->AddClass("cpu-profiler-col-count");
        Label* countPtr = count.get();
        row->AddChild(std::move(count));

        UIElement* rowPtr = row.get();
        rowPtr->RegisterEventHandler(kEventMouseDown, [this, rowPtr](UIEvent& e) {
            if (e.Button != 0)
                return;
            for (const auto& r : m_Rows)
            {
                if (r.Root == rowPtr)
                {
                    SelectScope(r.Name);
                    break;
                }
            }
            if (m_RowsContainer)
            {
                PostSafeAction([this]() {
                    if (UIManager* ui = GetOwnerManager())
                        ui->FocusElement(m_RowsContainer);
                });
            }
            e.Stop();
        });
        m_RowsContainer->AddChild(std::move(row));
        m_Rows.push_back(ScopeRow{rowPtr, namePtr, totalPtr, selfPtr, countPtr, {}});
    }

    // Populate in-use rows.
    for (std::size_t i = 0; i < n; ++i)
    {
        auto& row = m_Rows[i];
        if (!row.Root)
            continue;
        row.Root->RemoveClass("hidden");
        if (!rows.empty())
        {
            const auto& r = rows[i];
            row.Name = r.name;
            if (row.NameLabel)
                row.NameLabel->SetText(IndentedName(r.name, r.depth));
            if (row.TotalLabel)
                row.TotalLabel->SetText(FormatMs(r.totalMs));
            if (row.SelfLabel)
                row.SelfLabel->SetText(FormatMs(r.selfMs));
            if (row.CountLabel)
                row.CountLabel->SetText(FormatCount(r.callCount));
        }
        else
        {
            const auto& s = flatFallback[i];
            row.Name = s.first;
            if (row.NameLabel)
                row.NameLabel->SetText(s.first);
            if (row.TotalLabel)
                row.TotalLabel->SetText(FormatMs(s.second.totalMs));
            if (row.SelfLabel)
                row.SelfLabel->SetText("");
            if (row.CountLabel)
                row.CountLabel->SetText(FormatCount(s.second.count));
        }
        if (!m_SelectedScopeName.empty() && row.Name == m_SelectedScopeName)
            row.Root->AddClass("selected");
        else
            row.Root->RemoveClass("selected");
    }

    // Hide unused rows (don't destroy to avoid DOM churn).
    for (std::size_t i = n; i < m_Rows.size(); ++i)
    {
        auto& row = m_Rows[i];
        if (row.Root)
            row.Root->AddClass("hidden");
    }
}

void CpuProfilerPanel::OnPostLayout()
{
    if (!m_StyleAttached && !m_StyleLoadScheduled && GetOwnerManager())
    {
        m_StyleLoadScheduled = true;
        PostAction([this]() { LoadAndAttachPanelStyle(); });
    }
}

void CpuProfilerPanel::OnMountVisibilityChanged(bool isVisible)
{
    // CpuProfiler::SetEnabled(true) adds per-scope overhead across the engine.
    // Disable while the panel is hidden; the auto-arm path in Update() turns
    // it back on the next time the panel becomes visible.
    if (!isVisible)
    {
        auto& prof = Profiling::CpuProfiler::Get();
        if (prof.IsEnabled())
            prof.SetEnabled(false);
    }
}

void CpuProfilerPanel::LoadAndAttachPanelStyle()
{
    m_StyleLoadScheduled = false;
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;
    auto& am = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path styleAssetPath =
        std::filesystem::path("UI") / "panels" / "CpuProfilerPanel.css";
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
