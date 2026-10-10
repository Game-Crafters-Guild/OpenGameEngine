#include "Panels/MonitorsPanel.h"

#include "Platform/SystemMetrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Application.h"
#include "Core/DebugMetrics.h"
#include "Core/Engine.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/RenderServices.h"
#include "Input/KeyCodes.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/ChartView.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

namespace GameEngine {

namespace {

std::string FormatValue(float value, Debug::MonitorType type)
{
    char buf[64];
    switch (type)
    {
    case Debug::MonitorType::TimeMs:
        std::snprintf(buf, sizeof(buf), "%.2f ms", value);
        break;
    case Debug::MonitorType::Memory:
    {
        const double bytes = static_cast<double>(value);
        if (bytes >= 1024.0 * 1024.0 * 1024.0)
            std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        else if (bytes >= 1024.0 * 1024.0)
            std::snprintf(buf, sizeof(buf), "%.2f MB", bytes / (1024.0 * 1024.0));
        else if (bytes >= 1024.0)
            std::snprintf(buf, sizeof(buf), "%.2f KB", bytes / 1024.0);
        else
            std::snprintf(buf, sizeof(buf), "%.0f B", bytes);
        break;
    }
    case Debug::MonitorType::Percent:
        std::snprintf(buf, sizeof(buf), "%.1f%%", value);
        break;
    case Debug::MonitorType::Quantity:
    default:
        if (value >= 1000.0f)
            std::snprintf(buf, sizeof(buf), "%.0f", value);
        else
            std::snprintf(buf, sizeof(buf), "%.2f", value);
        break;
    }
    return std::string(buf);
}

// "Time/FPS" -> ("Time", "FPS"); no separator -> ("", name).
std::pair<std::string, std::string> SplitName(const std::string& name)
{
    const auto slash = name.find('/');
    if (slash == std::string::npos)
        return {"", name};
    return {name.substr(0, slash), name.substr(slash + 1)};
}

struct ChartAnnotation
{
    std::string label;        // Chart header text (replaces the bare monitor name).
    std::string_view tooltip; // Hover text on the strip; empty means no tooltip.
};

std::string MonitorDisplayName(const std::string& name)
{
    const auto [group, leaf] = SplitName(name);
    (void)group;
    return leaf.empty() ? name : leaf;
}

std::string JoinColorSpaces(const std::vector<std::string>& colorSpaces)
{
    if (colorSpaces.empty())
        return "none";

    std::string out;
    constexpr std::size_t kMaxVisibleSpaces = 3;
    const std::size_t count = std::min(colorSpaces.size(), kMaxVisibleSpaces);
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!out.empty())
            out += ", ";
        out += colorSpaces[i];
    }
    if (colorSpaces.size() > count)
        out += ", +";
    return out;
}

std::string FormatHdrStatus(Rendering::IDevice* device)
{
    if (!device)
        return "HDR: no device";

    const Rendering::HdrOutputState state = device->GetHdrOutputState();
    const Rendering::HdrDisplayInfo& display = state.display;
    const float paperWhite = std::clamp(state.staticMetadata.paperWhiteNits, 40.0f, 1000.0f);
    const float peakNits = Rendering::GetHdrOutputPeakLuminanceNits(state);
    const float outputMax = Rendering::GetHdrOutputMaxLinearValue(state);
    const int bitDepth = display.swapchainBitDepth == Rendering::HdrSwapchainBitDepth::Float16 ? 16 : 10;

    char buf[512];
    std::snprintf(buf,
                  sizeof(buf),
                  "HDR %s (%s -> %s)  fmt %s/%d-bit  paper %.0f nits  peak %.0f nits  max %.2fx  spaces %s",
                  Rendering::IsHdrOutputModeActive(state.activeMode) ? "on" : "off",
                  Rendering::HdrOutputModeToString(state.requestedMode),
                  Rendering::HdrOutputModeToString(state.activeMode),
                  Rendering::ToString(device->GetSwapchainTextureFormat()),
                  bitDepth,
                  paperWhite,
                  peakNits,
                  outputMax,
                  JoinColorSpaces(display.colorSpaces).c_str());

    std::string status = buf;
    for (const std::string& hint : display.diagnosticHints)
    {
        status += "  ·  ";
        status += hint;
    }
    return status;
}

// Decorate a chart header with how the underlying signal is computed. FPS
// especially is notoriously ambiguous across tools (raw 1/dt, sliding count,
// N-frame mean, ...) so the chart label should disambiguate.
ChartAnnotation ChartAnnotationFor(const std::string& name)
{
    using namespace std::string_view_literals;
    if (name == "Time/FPS")
        return {"FPS  ·  sliding 250 ms", ""sv};
    if (name == "Time/FrameMs")
        return {"FrameMs  ·  per-frame", ""sv};
    return {MonitorDisplayName(name), ""sv};
}

class MonitorsLayoutCommand final : public Editor::IEditorCommand
{
public:
    using ApplyFn = std::function<void(const MonitorsPanel::ChartLayoutSnapshot&)>;

    MonitorsLayoutCommand(std::shared_ptr<bool> token,
                          MonitorsPanel::ChartLayoutSnapshot before,
                          MonitorsPanel::ChartLayoutSnapshot after,
                          ApplyFn apply)
        : m_Token(std::move(token))
        , m_Before(std::move(before))
        , m_After(std::move(after))
        , m_Apply(std::move(apply))
    {
    }

    const char* GetName() const override { return "Change Monitor Layout"; }
    void Do() override { Apply(m_After); }
    void Undo() override { Apply(m_Before); }
    void Redo() override { Apply(m_After); }

private:
    void Apply(const MonitorsPanel::ChartLayoutSnapshot& snapshot)
    {
        if (!m_Apply || !m_Token || !*m_Token)
            return;
        m_Apply(snapshot);
    }

    std::shared_ptr<bool> m_Token;
    MonitorsPanel::ChartLayoutSnapshot m_Before;
    MonitorsPanel::ChartLayoutSnapshot m_After;
    ApplyFn m_Apply;
};

} // namespace

MonitorsPanel::~MonitorsPanel()
{
    // The pending asset callbacks capture this panel; withdraw them before it goes
    // away (destroying an AssetLoadHandle does not).
    if (m_PanelStyleLoadHandle)
        m_PanelStyleLoadHandle->Cancel();

    if (m_LifetimeToken)
        *m_LifetimeToken = false;
}

MonitorsPanel::MonitorsPanel()
    : DockPanel("Monitors")
{
    AddClass("monitors-panel");

    auto& metrics = Debug::DebugMetrics::Get();
    metrics.RegisterMonitor("Time/FPS", Debug::MonitorType::Quantity, "fps");
    metrics.RegisterMonitor("Time/FrameMs", Debug::MonitorType::TimeMs, "ms");
    metrics.RegisterMonitor("Time/Process", Debug::MonitorType::TimeMs, "ms");
    metrics.RegisterMonitor("Time/Render", Debug::MonitorType::TimeMs, "ms");
    metrics.RegisterMonitor("Time/Input", Debug::MonitorType::TimeMs, "ms");
    metrics.RegisterMonitor("Time/PollEvents", Debug::MonitorType::TimeMs, "ms");
    metrics.RegisterMonitor("Time/Sleep", Debug::MonitorType::TimeMs, "ms");
    metrics.RegisterMonitor("Time/Physics", Debug::MonitorType::TimeMs, "ms");
    metrics.RegisterMonitor("Time/GPUFramePeriod", Debug::MonitorType::TimeMs, "ms");
    metrics.RegisterMonitor("ECS/Entities", Debug::MonitorType::Quantity);
    metrics.RegisterMonitor("ECS/Archetypes", Debug::MonitorType::Quantity);
    metrics.RegisterMonitor("Memory/Static", Debug::MonitorType::Memory, "bytes");
    metrics.RegisterMonitor("Memory/VRAM", Debug::MonitorType::Memory, "bytes");
    metrics.RegisterMonitor("Memory/VRAM/Textures", Debug::MonitorType::Memory, "bytes");
    metrics.RegisterMonitor("Memory/VRAM/Buffers", Debug::MonitorType::Memory, "bytes");
    metrics.RegisterMonitor("Render/Passes", Debug::MonitorType::Quantity);
    metrics.RegisterMonitor("Render/Resources", Debug::MonitorType::Quantity);
    metrics.RegisterMonitor("Render/Barriers", Debug::MonitorType::Quantity);
    metrics.RegisterMonitor("Render/DrawCalls", Debug::MonitorType::Quantity);
    metrics.RegisterMonitor("Render/Instances", Debug::MonitorType::Quantity);
    metrics.RegisterMonitor("Render/Triangles", Debug::MonitorType::Quantity);
    metrics.RegisterMonitor("HDR/Active", Debug::MonitorType::Percent);
    metrics.RegisterMonitor("HDR/PaperWhiteNits", Debug::MonitorType::Quantity, "nits");
    metrics.RegisterMonitor("HDR/PeakNits", Debug::MonitorType::Quantity, "nits");
    metrics.RegisterMonitor("HDR/OutputMaxLinear", Debug::MonitorType::Quantity);
    metrics.RegisterMonitor("Physics/Bodies", Debug::MonitorType::Quantity);
    metrics.RegisterMonitor("Physics/ContactPairs", Debug::MonitorType::Quantity);
    metrics.RegisterMonitor("Audio/OutputLatencyMs", Debug::MonitorType::TimeMs, "ms");
    metrics.RegisterMonitor("Pipeline/Compilations", Debug::MonitorType::Quantity);

    m_Checked.insert("Time/FPS");

    BuildUI();
}

void MonitorsPanel::BuildUI()
{
    // Toolbar
    auto toolbar = std::make_unique<UIElement>();
    toolbar->AddClass("monitors-toolbar");

    auto pauseBtn = std::make_unique<Button>();
    pauseBtn->AddClass("small");
    pauseBtn->AddClass("secondary");
    pauseBtn->AddClass("monitors-btn-pause");
    pauseBtn->SetText("Pause");
    pauseBtn->SetTooltip("Pause monitor updates");
    pauseBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { TogglePause(); });
    m_PauseButton = pauseBtn.get();
    toolbar->AddChild(std::move(pauseBtn));

    auto clearBtn = std::make_unique<Button>();
    clearBtn->AddClass("small");
    clearBtn->AddClass("secondary");
    clearBtn->AddClass("monitors-btn-clear");
    clearBtn->SetText("Clear");
    clearBtn->SetTooltip("Clear recorded monitor samples");
    clearBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        Debug::DebugMetrics::Get().ClearAll();
        ClearCursor();
    });
    m_ClearButton = clearBtn.get();
    toolbar->AddChild(std::move(clearBtn));

    auto hdrStatus = std::make_unique<Label>();
    hdrStatus->AddClass("monitors-hdr-status");
    hdrStatus->SetText("HDR: no device");
    hdrStatus->SetTooltip("Active HDR output diagnostics: requested/resolved mode, swapchain format, paper white, detected peak, output max linear value, and exposed color spaces.");
    m_HdrStatusLabel = hdrStatus.get();
    toolbar->AddChild(std::move(hdrStatus));

    AddChild(std::move(toolbar));

    // Body: [ left list | right charts ]
    auto body = std::make_unique<UIElement>();
    body->AddClass("monitors-body");

    // --- Left: column header + scrollable list ---
    auto left = std::make_unique<UIElement>();
    left->AddClass("monitors-left");

    auto listHeader = std::make_unique<UIElement>();
    listHeader->AddClass("monitors-list-header");
    {
        auto lblMon = std::make_unique<Label>();
        lblMon->AddClass("monitors-list-header-name");
        lblMon->SetText("Monitor");
        listHeader->AddChild(std::move(lblMon));

        auto lblVal = std::make_unique<Label>();
        lblVal->AddClass("monitors-list-header-value");
        lblVal->SetText("Value");
        listHeader->AddChild(std::move(lblVal));
    }
    left->AddChild(std::move(listHeader));

    auto scroll = std::make_unique<ScrollView>();
    scroll->AddClass("monitors-list-scroll");
    m_ListContainer = scroll->GetViewport();
    if (m_ListContainer)
        m_ListContainer->AddClass("monitors-list");
    left->AddChild(std::move(scroll));

    body->AddChild(std::move(left));

    // --- Right: grid of chart strips ---
    auto right = std::make_unique<UIElement>();
    right->AddClass("monitors-right");

    auto grid = std::make_unique<UIElement>();
    grid->AddClass("monitors-charts-grid");
    m_ChartsContainer = grid.get();
    right->AddChild(std::move(grid));

    body->AddChild(std::move(right));

    AddChild(std::move(body));

    // Space toggles pause/resume whenever a child of the panel holds focus.
    RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
        if (e.Key != Input::kKeyCode_Space)
            return;
        TogglePause();
        e.Stop();
    });
}

void MonitorsPanel::RebuildMonitorList()
{
    if (!m_ListContainer)
        return;

    std::vector<Debug::MonitorInfo> snapshot;
    Debug::DebugMetrics::Get().Snapshot(snapshot);
    std::sort(snapshot.begin(), snapshot.end(),
              [](const auto& a, const auto& b) {
                  auto groupOf = [](const std::string& n) {
                      auto pos = n.find('/');
                      return pos == std::string::npos ? std::string() : n.substr(0, pos);
                  };
                  const std::string ga = groupOf(a.Name);
                  const std::string gb = groupOf(b.Name);
                  const bool aTime = (ga == "Time");
                  const bool bTime = (gb == "Time");
                  if (aTime != bTime)
                      return aTime;
                  return a.Name < b.Name;
              });

    std::size_t sig = snapshot.size();
    for (const auto& m : snapshot)
        sig = sig * 131ull + std::hash<std::string>{}(m.Name);
    if (sig == m_LastListSignature)
        return;
    m_LastListSignature = sig;

    m_ListContainer->RemoveAllChildren();
    m_Rows.clear();
    m_RowGroup.clear();
    m_GroupHeaders.clear();

    std::string currentGroup;
    bool firstGroup = true;
    bool currentGroupCollapsed = false;
    for (const auto& info : snapshot)
    {
        auto [group, leaf] = SplitName(info.Name);
        if (group != currentGroup || firstGroup)
        {
            currentGroup = group;
            const std::string groupKey = group.empty() ? std::string("Other") : group;
            currentGroupCollapsed = m_CollapsedGroups.count(groupKey) != 0;

            auto header = std::make_unique<Label>();
            header->AddClass("monitors-group-header");
            const char* chevron = currentGroupCollapsed ? "\xE2\x96\xB6" : "\xE2\x96\xBC"; // ▶ / ▼
            header->SetText(std::string(chevron) + " " + groupKey);
            if (!firstGroup)
                header->AddClass("monitors-group-header-spaced");
            firstGroup = false;

            Label* headerPtr = header.get();
            const std::string keyCopy = groupKey;
            header->RegisterEventHandler(kEventMouseDown, [this, keyCopy](UIEvent& e) {
                if (e.Button != 0)
                    return;
                ToggleGroupCollapsed(keyCopy);
                e.Stop();
            });
            m_GroupHeaders[groupKey] = headerPtr;
            m_ListContainer->AddChild(std::move(header));
        }

        const std::string groupKey = group.empty() ? std::string("Other") : group;
        m_RowGroup[info.Name] = groupKey;
        if (currentGroupCollapsed)
            continue;

        auto row = std::make_unique<UIElement>();
        row->AddClass("monitors-row");

        auto check = std::make_unique<Checkbox>();
        check->AddClass("monitors-row-check");
        check->SetText(leaf.empty() ? info.Name : leaf);
        check->SetChecked(m_Checked.count(info.Name) != 0);
        const std::string nameCopy = info.Name;
        check->SetOnValueChanged([this, nameCopy](bool on) { ToggleMonitor(nameCopy, on); });
        Checkbox* checkPtr = check.get();
        row->AddChild(std::move(check));

        auto spacer = std::make_unique<UIElement>();
        spacer->AddClass("monitors-row-spacer");
        row->AddChild(std::move(spacer));

        auto value = std::make_unique<Label>();
        value->AddClass("monitors-row-value");
        value->SetText("—");
        Label* valuePtr = value.get();
        row->AddChild(std::move(value));

        m_ListContainer->AddChild(std::move(row));
        m_Rows[info.Name] = RowWidgets{checkPtr, valuePtr, info.Type};
    }

    // Make sure any pre-checked monitors have a chart strip.
    for (const auto& name : m_Checked)
    {
        if (m_Charts.find(name) == m_Charts.end())
            AddChartFor(name);
    }
}

void MonitorsPanel::RequestMonitorListRebuild()
{
    m_LastListSignature = 0;
    if (UIElement::IsInEventDispatch())
    {
        if (m_MonitorListRebuildPending)
            return;

        m_MonitorListRebuildPending = true;
        auto token = m_LifetimeToken;
        PostAction([this, token]() {
            if (!token || !*token)
                return;
            m_MonitorListRebuildPending = false;
            RebuildMonitorList();
        });
        return;
    }

    RebuildMonitorList();
}

void MonitorsPanel::ToggleGroupCollapsed(const std::string& groupKey)
{
    if (m_CollapsedGroups.count(groupKey))
        m_CollapsedGroups.erase(groupKey);
    else
        m_CollapsedGroups.insert(groupKey);
    RequestMonitorListRebuild();
}

void MonitorsPanel::UpdateValues()
{
    // Walk only the widgets we actually own (no full Snapshot copy each frame),
    // and skip SetText when the formatted text didn't change so labels don't
    // dirty their parent layouts every tick.
    auto& metrics = Debug::DebugMetrics::Get();

    auto resolveValue = [&](const std::string& name, Debug::MonitorType type, float& outValue) {
        if (m_Paused && m_CursorFrame >= 0)
        {
            auto it = m_FrozenSeries.find(name);
            if (it != m_FrozenSeries.end() && !it->second.empty())
            {
                const std::size_t n = it->second.size();
                const std::size_t idx = std::min(static_cast<std::size_t>(m_CursorFrame), n - 1);
                outValue = it->second[idx];
                return true;
            }
        }
        // The Time/FrameMs chart series is raw per-frame work cost — fine for
        // chart shape, too noisy for at-a-glance reading. Resolve the live
        // row text to 1000/GetFps() so the FPS and FrameMs row values are
        // mathematically consistent against the same display window (FPS=60
        // always displays alongside FrameMs=16.67). Scrubbed mode falls
        // through above and shows the precise per-frame sample at the
        // selected frame.
        //
        // Time/FPS does not need a special case: its chart series already
        // pushes GetFps() per frame, so metrics.GetLatest below returns the
        // same value.
        if (name == "Time/FrameMs")
        {
            if (auto* app = Application::Get())
            {
                const float fps = app->GetFps();
                outValue = fps > 0.0f ? 1000.0f / fps : 0.0f;
                return true;
            }
        }
        return metrics.GetLatest(name, outValue, const_cast<Debug::MonitorType*>(&type));
    };

    for (auto& [name, row] : m_Rows)
    {
        if (!row.Value)
            continue;
        float v = 0.0f;
        if (!resolveValue(name, row.Type, v))
            continue;
        // Row value labels have a fixed 80px width — paint-only update avoids
        // a Yoga relayout pass for the entire panel each frame.
        row.Value->SetText(FormatValue(v, row.Type));
    }

    for (auto& [name, chart] : m_Charts)
    {
        if (!chart.Value)
            continue;
        float v = 0.0f;
        if (!resolveValue(name, chart.Type, v))
            continue;
        chart.Value->SetText(FormatValue(v, chart.Type));
    }
}

void MonitorsPanel::AttachChartScrubHandlers(ChartView* chart)
{
    if (!chart)
        return;
    chart->RegisterEventHandler(kEventMouseDown, [this, chart](UIEvent& e) {
        if (e.Button != 0 && e.Button != 1)
            return;

        // Double-click resumes live playback. Matches the "click to pause,
        // double-click to resume" interaction used in other editors.
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
            ResumeFromPause();
            m_HasLastChartClick = false;
            e.Stop();
            return;
        }

        m_ScrubbingChart = chart;
        e.Capture(chart);
        const float localX = e.X - chart->GetLayoutX();
        OnChartClicked(chart, localX, chart->GetLayoutWidth());
        e.Stop();
    });
    chart->RegisterEventHandler(kEventMouseMove, [this, chart](UIEvent& e) {
        if (m_ScrubbingChart != chart)
            return;
        const float localX = e.X - chart->GetLayoutX();
        OnChartClicked(chart, localX, chart->GetLayoutWidth());
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

void MonitorsPanel::SnapshotSeriesForFreeze()
{
    m_FrozenSeries.clear();
    auto& metrics = Debug::DebugMetrics::Get();
    // Freeze every monitor we have a chart strip for so scrubbing the cursor
    // updates the value labels even for monitors whose rows are unchecked.
    for (const auto& [name, _] : m_Charts)
    {
        std::vector<float> series;
        metrics.CopyHistory(name, series);
        m_FrozenSeries[name] = std::move(series);
    }
}

void MonitorsPanel::ApplyCursorToCharts()
{
    for (auto& [name, widgets] : m_Charts)
    {
        if (widgets.Chart)
            widgets.Chart->SetCursorIndex(m_CursorFrame);
    }
}

void MonitorsPanel::OnChartClicked(ChartView* chart, float localX, float chartWidth)
{
    if (!chart || chartWidth <= 0.0f)
        return;
    if (!m_Paused)
    {
        auto& metrics = Debug::DebugMetrics::Get();
        if (!metrics.IsPaused())
            metrics.SetPaused(true);
        if (m_PauseButton)
        {
            m_PauseButton->SetText("Resume");
            m_PauseButton->AddClass("paused");
        }
        SnapshotSeriesForFreeze();
        m_Paused = true;
    }
    // Use the clicked chart's own series length to map localX → frame index.
    auto it = m_FrozenSeries.end();
    for (auto sit = m_FrozenSeries.begin(); sit != m_FrozenSeries.end(); ++sit)
    {
        auto ch = m_Charts.find(sit->first);
        if (ch != m_Charts.end() && ch->second.Chart == chart)
        {
            it = sit;
            break;
        }
    }
    if (it == m_FrozenSeries.end() || it->second.empty())
        return;
    const std::size_t n = it->second.size();
    const float t = std::clamp(localX / chartWidth, 0.0f, 1.0f);
    const int frame = static_cast<int>(t * static_cast<float>(n - 1) + 0.5f);
    m_CursorFrame = frame;
    ApplyCursorToCharts();
}

void MonitorsPanel::ClearCursor()
{
    m_Paused = false;
    m_CursorFrame = -1;
    m_ScrubbingChart = nullptr;
    m_FrozenSeries.clear();
    for (auto& [name, widgets] : m_Charts)
    {
        if (widgets.Chart)
            widgets.Chart->SetCursorIndex(-1);
    }
}

void MonitorsPanel::TogglePause()
{
    auto& metrics = Debug::DebugMetrics::Get();
    const bool nowPaused = !metrics.IsPaused();
    metrics.SetPaused(nowPaused);
    if (m_PauseButton)
    {
        m_PauseButton->SetText(nowPaused ? "Resume" : "Pause");
        if (nowPaused)
            m_PauseButton->AddClass("paused");
        else
            m_PauseButton->RemoveClass("paused");
    }
    if (nowPaused)
    {
        SnapshotSeriesForFreeze();
        m_Paused = true;
    }
    else
    {
        ClearCursor();
    }
}

void MonitorsPanel::ResumeFromPause()
{
    auto& metrics = Debug::DebugMetrics::Get();
    if (metrics.IsPaused())
        metrics.SetPaused(false);
    if (m_PauseButton)
    {
        m_PauseButton->SetText("Pause");
        m_PauseButton->RemoveClass("paused");
    }
    ClearCursor();
}

void MonitorsPanel::ToggleMonitor(const std::string& name, bool on)
{
    const ChartLayoutSnapshot before = CaptureChartLayoutSnapshot();
    bool changed = false;
    if (on)
    {
        if (m_Checked.insert(name).second)
        {
            AddChartFor(name);
            changed = true;
        }
    }
    else
    {
        if (m_Checked.erase(name) > 0)
        {
            RemoveChartFor(name);
            changed = true;
        }
    }

    if (changed && !m_SuppressChartLayoutUndo)
        CommitChartLayoutUndo(before, CaptureChartLayoutSnapshot());
}

void MonitorsPanel::AddChartFor(const std::string& name)
{
    if (!m_ChartsContainer || m_Charts.find(name) != m_Charts.end())
        return;
    if (std::find(m_ChartOrder.begin(), m_ChartOrder.end(), name) == m_ChartOrder.end())
        m_ChartOrder.push_back(name);

    auto rowStart = std::make_unique<UIElement>();
    rowStart->AddClass("monitors-chart-row-start");
    UIElement* rowStartPtr = rowStart.get();
    m_ChartsContainer->AddChild(std::move(rowStart));

    auto strip = std::make_unique<UIElement>();
    strip->AddClass("monitors-chart-strip");

    auto header = std::make_unique<UIElement>();
    header->AddClass("monitors-chart-header");
    header->AddClass("monitors-chart-drag-handle");
    UIElement* headerPtr = header.get();

    // Some metrics benefit from a clarifying suffix on the chart so readers
    // know what the line shape actually represents. FPS in particular varies
    // wildly across tools (raw 1/dt, sliding count, N-frame mean, ...) and a
    // shape only makes sense in light of how it's computed.
    const auto annotation = ChartAnnotationFor(name);
    auto nameLbl = std::make_unique<Label>();
    nameLbl->AddClass("monitors-chart-name");
    nameLbl->SetText(annotation.label);
    Label* namePtr = nameLbl.get();
    header->AddChild(std::move(nameLbl));

    auto spacer = std::make_unique<UIElement>();
    spacer->AddClass("monitors-chart-header-spacer");
    header->AddChild(std::move(spacer));

    auto valueLbl = std::make_unique<Label>();
    valueLbl->AddClass("monitors-chart-value");
    valueLbl->SetText("—");
    Label* valuePtr = valueLbl.get();
    header->AddChild(std::move(valueLbl));

    strip->AddChild(std::move(header));

    auto chart = std::make_unique<ChartView>();
    chart->AddClass("monitors-chart");
    chart->SetMode(ChartView::Mode::Line);
    // Smooth axis decay so a one-frame stutter doesn't flick the chart
    // between two wildly different scales when it ages out of the window.
    // ~1.5s half-life: the spike snaps the axis up immediately, then the
    // axis relaxes back over a few seconds once the spike is gone.
    chart->SetYMaxSmoothingHalfLife(1.5f);
    const std::string nameCopy = name;
    chart->SetProvider([this, nameCopy](std::vector<float>& out) {
        if (m_Paused)
        {
            auto it = m_FrozenSeries.find(nameCopy);
            if (it != m_FrozenSeries.end())
                out = it->second;
            else
                out.clear();
            return;
        }
        Debug::DebugMetrics::Get().CopyHistory(nameCopy, out);
    });
    Debug::MonitorType monitorType = Debug::MonitorType::Quantity;
    {
        float ignore = 0.0f;
        Debug::DebugMetrics::Get().GetLatest(name, ignore, &monitorType);
    }
    chart->SetValueFormatter([monitorType](float v) { return FormatValue(v, monitorType); });
    ChartView* chartPtr = chart.get();
    AttachChartScrubHandlers(chartPtr);
    strip->AddChild(std::move(chart));

    UIElement* stripPtr = strip.get();
    if (!annotation.tooltip.empty())
        stripPtr->SetTooltip(std::string(annotation.tooltip));
    m_ChartsContainer->AddChild(std::move(strip));
    m_Charts[name] = ChartWidgets{rowStartPtr, stripPtr, namePtr, valuePtr, chartPtr, monitorType};
    AttachChartDragHandlers(stripPtr, headerPtr, name);
    ReorderChartElements();

    if (m_Paused)
    {
        std::vector<float> series;
        Debug::DebugMetrics::Get().CopyHistory(name, series);
        m_FrozenSeries[name] = std::move(series);
        if (m_CursorFrame >= 0)
            chartPtr->SetCursorIndex(m_CursorFrame);
    }
}

void MonitorsPanel::RemoveChartFor(const std::string& name)
{
    auto it = m_Charts.find(name);
    if (it == m_Charts.end())
        return;
    if (m_DragChartName == name || m_ChartDropTarget.Name == name)
        ClearChartDragState();
    auto orderIt = std::find(m_ChartOrder.begin(), m_ChartOrder.end(), name);
    if (orderIt != m_ChartOrder.end() && m_ChartRowBreaks.count(name) != 0)
    {
        auto nextIt = std::next(orderIt);
        if (nextIt != m_ChartOrder.end())
            m_ChartRowBreaks.insert(*nextIt);
    }
    if (m_ChartsContainer && it->second.RowStart)
        m_ChartsContainer->RemoveChild(it->second.RowStart);
    if (m_ChartsContainer && it->second.Strip)
        m_ChartsContainer->RemoveChild(it->second.Strip);
    m_Charts.erase(it);
    m_ChartOrder.erase(std::remove(m_ChartOrder.begin(), m_ChartOrder.end(), name), m_ChartOrder.end());
    m_ChartRowBreaks.erase(name);
    m_FrozenSeries.erase(name);
    ReorderChartElements();
}

void MonitorsPanel::AttachChartDragHandlers(UIElement* strip, UIElement* header, const std::string& name)
{
    if (!strip || !header)
        return;

    header->RegisterEventHandler(kEventMouseDown, [this, strip, name](UIEvent& e) {
        if (e.Button != 0)
            return;

        ClearChartDragState();
        m_DragChartName = name;
        m_DragStartX = e.X;
        m_DragStartY = e.Y;
        m_ChartDragActive = false;
        strip->AddClass("drag-source");
        e.Capture(e.CurrentTarget ? e.CurrentTarget : strip);
        e.Stop();
    });

    header->RegisterEventHandler(kEventMouseMove, [this, strip, name](UIEvent& e) {
        if (m_DragChartName != name)
            return;

        const float dx = e.X - m_DragStartX;
        const float dy = e.Y - m_DragStartY;
        if (!m_ChartDragActive && (std::abs(dx) > 4.0f || std::abs(dy) > 4.0f))
        {
            m_ChartDragActive = true;
            strip->AddClass("dragging");
        }

        if (m_ChartDragActive)
        {
            ShowChartDragGhost(name, e.X, e.Y);
            SetChartDropTarget(HitTestChartDropTarget(e.X, e.Y, name));
        }
        e.Stop();
    });

    header->RegisterEventHandler(kEventMouseUp, [this, name](UIEvent& e) {
        if (m_DragChartName != name)
            return;

        const bool shouldDrop = m_ChartDragActive && m_ChartDropTarget.Placement != ChartDropPlacement::None;
        const std::string source = m_DragChartName;
        const ChartDropTarget target = m_ChartDropTarget;
        UIElement* postTarget = e.CurrentTarget;
        ClearChartDragState();

        if (shouldDrop && postTarget)
        {
            postTarget->PostAction([this, source, target]() {
                ApplyChartDrop(source, target);
            });
        }
        e.Stop();
    });
}

MonitorsPanel::ChartDropTarget MonitorsPanel::HitTestChartDropTarget(float x, float y, const std::string& ignoreName) const
{
    for (const auto& name : m_ChartOrder)
    {
        auto it = m_Charts.find(name);
        if (it == m_Charts.end() || !it->second.Strip)
            continue;

        const UIElement* strip = it->second.Strip;
        const float sx = strip->GetLayoutX();
        const float sy = strip->GetLayoutY();
        const float sw = strip->GetLayoutWidth();
        const float sh = strip->GetLayoutHeight();
        if (x >= sx && x <= sx + sw && y >= sy && y <= sy + sh)
        {
            if (name == ignoreName)
                return {};

            ChartDropTarget target;
            target.Name = name;
            const float relY = sh > 0.0f ? (y - sy) / sh : 0.5f;
            const float relX = sw > 0.0f ? (x - sx) / sw : 0.5f;
            if (relX < 0.25f)
            {
                target.Placement = ChartDropPlacement::Before;
                target.NewRow = false;
            }
            else if (relX > 0.75f)
            {
                target.Placement = ChartDropPlacement::After;
                target.NewRow = false;
            }
            else if (relY < 0.25f)
            {
                target.Placement = ChartDropPlacement::Before;
                target.NewRow = true;
            }
            else if (relY > 0.75f)
            {
                target.Placement = ChartDropPlacement::After;
                target.NewRow = true;
            }
            else
            {
                target.Placement = relX < 0.5f ? ChartDropPlacement::Before : ChartDropPlacement::After;
                target.NewRow = false;
            }
            target = NormalizeChartDropTarget(ignoreName, target);
            if (IsChartDropNoOp(ignoreName, target))
                return {};
            return target;
        }
    }

    if (m_ChartsContainer)
    {
        const float cx = m_ChartsContainer->GetLayoutX();
        const float cy = m_ChartsContainer->GetLayoutY();
        const float cw = m_ChartsContainer->GetLayoutWidth();
        const float ch = m_ChartsContainer->GetLayoutHeight();
        if (x >= cx && x <= cx + cw && y >= cy && y <= cy + ch)
        {
            constexpr float kGap = 6.0f;
            float appendTop = cy;
            float appendHeight = 160.0f;

            for (auto it = m_ChartOrder.rbegin(); it != m_ChartOrder.rend(); ++it)
            {
                if (*it == ignoreName)
                    continue;
                auto chartIt = m_Charts.find(*it);
                if (chartIt == m_Charts.end() || !chartIt->second.Strip)
                    continue;

                const UIElement* strip = chartIt->second.Strip;
                appendTop = strip->GetLayoutY() + strip->GetLayoutHeight() + kGap;
                appendHeight = strip->GetLayoutHeight();
                break;
            }

            const float appendBottom = std::min(cy + ch, appendTop + appendHeight);
            if (y >= appendTop && y <= appendBottom)
                return ChartDropTarget{{}, ChartDropPlacement::Append, true};
        }
    }
    return {};
}

MonitorsPanel::ChartDropTarget MonitorsPanel::NormalizeChartDropTarget(const std::string& source, ChartDropTarget target) const
{
    if (source.empty() || target.Name.empty())
        return target;
    if (target.Placement != ChartDropPlacement::Before &&
        target.Placement != ChartDropPlacement::After)
        return target;

    auto sourceIt = std::find(m_ChartOrder.begin(), m_ChartOrder.end(), source);
    auto targetIt = std::find(m_ChartOrder.begin(), m_ChartOrder.end(), target.Name);
    if (sourceIt == m_ChartOrder.end() || targetIt == m_ChartOrder.end())
        return target;

    // Adjacent strips are a swap gesture: dropping on either half of the
    // neighbor should exchange left/right positions instead of letting one
    // side resolve to "already before/after" or "drop below" and do nothing.
    if (std::next(sourceIt) == targetIt)
    {
        target.Placement = ChartDropPlacement::After;
        target.NewRow = false;
    }
    else if (std::next(targetIt) == sourceIt)
    {
        target.Placement = ChartDropPlacement::Before;
        target.NewRow = false;
    }

    return target;
}

bool MonitorsPanel::IsChartDropNoOp(const std::string& source, const ChartDropTarget& target) const
{
    if (source.empty() || target.Name.empty() || target.NewRow)
        return false;
    if (target.Placement != ChartDropPlacement::Before &&
        target.Placement != ChartDropPlacement::After)
        return false;

    auto sourceIt = std::find(m_ChartOrder.begin(), m_ChartOrder.end(), source);
    auto targetIt = std::find(m_ChartOrder.begin(), m_ChartOrder.end(), target.Name);
    if (sourceIt == m_ChartOrder.end() || targetIt == m_ChartOrder.end())
        return false;

    const bool targetStartsRow = m_ChartRowBreaks.count(target.Name) != 0;
    const bool sourceStartsRow = m_ChartRowBreaks.count(source) != 0;
    if (target.Placement == ChartDropPlacement::After)
    {
        auto nextIt = std::next(targetIt);
        return nextIt == sourceIt && !sourceStartsRow;
    }
    if (sourceIt == std::next(targetIt))
        return false;
    return std::next(sourceIt) == targetIt && !targetStartsRow;
}

void MonitorsPanel::SetChartDropTarget(const ChartDropTarget& target)
{
    if (m_ChartDropTarget.Name == target.Name &&
        m_ChartDropTarget.Placement == target.Placement &&
        m_ChartDropTarget.NewRow == target.NewRow)
        return;

    if (!m_ChartDropTarget.Name.empty())
    {
        auto oldIt = m_Charts.find(m_ChartDropTarget.Name);
        if (oldIt != m_Charts.end() && oldIt->second.Strip)
        {
            oldIt->second.Strip->RemoveClass("swap-target");
            oldIt->second.Strip->RemoveClass("drop-before");
            oldIt->second.Strip->RemoveClass("drop-after");
            oldIt->second.Strip->RemoveClass("drop-new-row");
        }
    }
    if (m_ChartsContainer)
        m_ChartsContainer->RemoveClass("drop-append");

    m_ChartDropTarget = target;
    if (!m_ChartDropTarget.Name.empty())
    {
        auto newIt = m_Charts.find(m_ChartDropTarget.Name);
        if (newIt != m_Charts.end() && newIt->second.Strip)
        {
            newIt->second.Strip->AddClass("swap-target");
            if (m_ChartDropTarget.Placement == ChartDropPlacement::Before)
                newIt->second.Strip->AddClass("drop-before");
            else if (m_ChartDropTarget.Placement == ChartDropPlacement::After)
                newIt->second.Strip->AddClass("drop-after");
            if (m_ChartDropTarget.NewRow)
                newIt->second.Strip->AddClass("drop-new-row");
        }
    }
    else if (m_ChartDropTarget.Placement == ChartDropPlacement::Append && m_ChartsContainer)
    {
        // Feedback is drawn by the root overlay indicator so the whole chart
        // grid does not flash when appending into empty space.
    }
    ShowChartDropIndicator(m_ChartDropTarget);
}

void MonitorsPanel::ClearChartDragState()
{
    if (!m_DragChartName.empty())
    {
        auto dragIt = m_Charts.find(m_DragChartName);
        if (dragIt != m_Charts.end() && dragIt->second.Strip)
        {
            dragIt->second.Strip->RemoveClass("drag-source");
            dragIt->second.Strip->RemoveClass("dragging");
        }
    }
    SetChartDropTarget({});
    HideChartDragGhost();
    m_DragChartName.clear();
    m_ChartDropTarget = {};
    m_ChartDragActive = false;
}

void MonitorsPanel::ApplyChartDrop(const std::string& source, const ChartDropTarget& target)
{
    if (source.empty() || target.Placement == ChartDropPlacement::None)
        return;

    const ChartLayoutSnapshot before = CaptureChartLayoutSnapshot();

    auto sourceIt = std::find(m_ChartOrder.begin(), m_ChartOrder.end(), source);
    if (sourceIt == m_ChartOrder.end())
        return;

    const bool sourceStartedRow = m_ChartRowBreaks.count(source) != 0;
    if (sourceStartedRow)
    {
        auto nextIt = std::next(sourceIt);
        if (nextIt != m_ChartOrder.end())
            m_ChartRowBreaks.insert(*nextIt);
        m_ChartRowBreaks.erase(source);
    }

    m_ChartOrder.erase(sourceIt);
    if (target.Placement == ChartDropPlacement::Append || target.Name.empty())
    {
        m_ChartOrder.push_back(source);
    }
    else
    {
        auto targetIt = std::find(m_ChartOrder.begin(), m_ChartOrder.end(), target.Name);
        if (targetIt == m_ChartOrder.end())
        {
            m_ChartOrder.push_back(source);
        }
        else
        {
            if (target.Placement == ChartDropPlacement::After)
                ++targetIt;
            m_ChartOrder.insert(targetIt, source);
        }
    }

    if (target.NewRow)
    {
        m_ChartRowBreaks.insert(source);
        auto insertedIt = std::find(m_ChartOrder.begin(), m_ChartOrder.end(), source);
        if (insertedIt != m_ChartOrder.end())
        {
            auto nextIt = std::next(insertedIt);
            if (nextIt != m_ChartOrder.end())
                m_ChartRowBreaks.insert(*nextIt);
        }
    }
    else
    {
        m_ChartRowBreaks.erase(source);
        if (!target.Name.empty() &&
            target.Placement == ChartDropPlacement::Before &&
            m_ChartRowBreaks.count(target.Name) != 0)
        {
            m_ChartRowBreaks.erase(target.Name);
            m_ChartRowBreaks.insert(source);
        }
    }

    if (m_Charts.find(source) == m_Charts.end())
        return;

    ReorderChartElements();

    const ChartLayoutSnapshot after = CaptureChartLayoutSnapshot();
    CommitChartLayoutUndo(before, after);
}

MonitorsPanel::ChartLayoutSnapshot MonitorsPanel::CaptureChartLayoutSnapshot() const
{
    return ChartLayoutSnapshot{m_ChartOrder, m_ChartRowBreaks, m_Checked};
}

void MonitorsPanel::ApplyChartLayoutSnapshot(const ChartLayoutSnapshot& snapshot)
{
    m_SuppressChartLayoutUndo = true;

    std::set<std::string> checked;
    for (const auto& name : snapshot.checked)
    {
        if (m_Rows.find(name) != m_Rows.end() || m_Charts.find(name) != m_Charts.end())
            checked.insert(name);
    }

    std::vector<std::string> toRemove;
    for (const auto& name : m_Checked)
    {
        if (checked.count(name) == 0)
            toRemove.push_back(name);
    }
    for (const auto& name : toRemove)
    {
        m_Checked.erase(name);
        if (auto rowIt = m_Rows.find(name); rowIt != m_Rows.end() && rowIt->second.Check)
            rowIt->second.Check->SetValueWithoutNotify(false);
        RemoveChartFor(name);
    }
    for (const auto& name : checked)
    {
        if (m_Checked.insert(name).second)
        {
            if (auto rowIt = m_Rows.find(name); rowIt != m_Rows.end() && rowIt->second.Check)
                rowIt->second.Check->SetValueWithoutNotify(true);
            AddChartFor(name);
        }
        else if (auto rowIt = m_Rows.find(name); rowIt != m_Rows.end() && rowIt->second.Check)
        {
            rowIt->second.Check->SetValueWithoutNotify(true);
        }
    }

    std::vector<std::string> order;
    order.reserve(m_Charts.size());
    std::set<std::string> seen;
    for (const auto& name : snapshot.order)
    {
        if (m_Charts.find(name) == m_Charts.end() || seen.count(name) != 0)
            continue;
        order.push_back(name);
        seen.insert(name);
    }
    for (const auto& [name, _] : m_Charts)
    {
        if (seen.count(name) != 0)
            continue;
        order.push_back(name);
    }

    std::set<std::string> rowBreaks;
    for (const auto& name : snapshot.rowBreaks)
    {
        if (m_Charts.find(name) != m_Charts.end())
            rowBreaks.insert(name);
    }

    m_ChartOrder = std::move(order);
    m_ChartRowBreaks = std::move(rowBreaks);
    ReorderChartElements();

    m_SuppressChartLayoutUndo = false;
}

void MonitorsPanel::CommitChartLayoutUndo(const ChartLayoutSnapshot& before, const ChartLayoutSnapshot& after)
{
    if (!m_Undo)
        return;
    if (before.order == after.order &&
        before.rowBreaks == after.rowBreaks &&
        before.checked == after.checked)
        return;

    auto token = m_LifetimeToken;
    m_Undo->CommitAlreadyApplied(std::make_unique<MonitorsLayoutCommand>(
        token,
        before,
        after,
        [this, token](const ChartLayoutSnapshot& snapshot) {
            if (!token || !*token)
                return;
            ApplyChartLayoutSnapshot(snapshot);
        }));
}

void MonitorsPanel::ReorderChartElements()
{
    if (!m_ChartsContainer)
        return;

    std::unordered_map<UIElement*, std::size_t> orderByElement;
    std::size_t visibleChartCount = 0;
    for (std::size_t i = 0; i < m_ChartOrder.size(); ++i)
    {
        auto it = m_Charts.find(m_ChartOrder[i]);
        if (it != m_Charts.end() && it->second.Strip)
        {
            ++visibleChartCount;
            if (it->second.RowStart)
            {
                orderByElement[it->second.RowStart] = i * 2;
                if (i > 0 && m_ChartRowBreaks.count(m_ChartOrder[i]) != 0)
                    it->second.RowStart->AddClass("active");
                else
                    it->second.RowStart->RemoveClass("active");
            }
            orderByElement[it->second.Strip] = i * 2 + 1;
            if (m_ChartRowBreaks.count(m_ChartOrder[i]) != 0)
                it->second.Strip->AddClass("row-break");
            else
                it->second.Strip->RemoveClass("row-break");
        }
    }

    if (visibleChartCount > 0 && visibleChartCount <= 4)
        m_ChartsContainer->AddClass("sparse");
    else
        m_ChartsContainer->RemoveClass("sparse");

    auto& children = m_ChartsContainer->GetMutableChildren();
    std::stable_sort(children.begin(), children.end(),
                     [&orderByElement](const std::unique_ptr<UIElement>& a, const std::unique_ptr<UIElement>& b) {
                         const auto ai = orderByElement.find(a.get());
                         const auto bi = orderByElement.find(b.get());
                         const std::size_t av = ai == orderByElement.end() ? static_cast<std::size_t>(-1) : ai->second;
                         const std::size_t bv = bi == orderByElement.end() ? static_cast<std::size_t>(-1) : bi->second;
                         return av < bv;
                     });
    m_ChartsContainer->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
    UIManagerNotifyTreeStructureChanged(m_ChartsContainer->GetOwnerManager());
}

void MonitorsPanel::ShowChartDragGhost(const std::string& name, float x, float y)
{
    if (!m_ChartDragGhost)
    {
        UIManager* ui = GetOwnerManager();
        UIElement* root = ui ? ui->GetRootElement() : nullptr;
        if (!root)
            return;

        auto ghost = std::make_unique<Label>();
        ghost->AddClass("monitors-chart-drag-ghost");
        ghost->SetOverlayLayer(OverlayLayer::DragPreview);
        ghost->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PointerEvents, false)
            .Set(Style::ZIndex, 9400)
            .Set(Style::Display, DisplayMode::None)
            .Set(Style::PaddingTop, StyleLength::Px(6.0f))
            .Set(Style::PaddingRight, StyleLength::Px(10.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(6.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(10.0f))
            .Set(Style::MinWidth, StyleLength::Px(120.0f))
            .Set(Style::MaxWidth, StyleLength::Px(280.0f))
            .Set(Style::BackgroundColor, static_cast<uint32_t>(0xF21E242Cu))
            .Set(Style::Color, static_cast<uint32_t>(0xFFEDEDEDu))
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f})
            .Set(Style::FontSize, StyleLength::Px(12.0f));
        m_ChartDragGhost = ghost.get();
        root->AddChild(std::move(ghost));
    }

    if (!m_ChartDragGhost)
        return;

    m_ChartDragGhost->SetText(MonitorDisplayName(name));
    m_ChartDragGhost->Overrides()
        .Set(Style::Display, DisplayMode::Block)
        .Set(Style::PositionLeft, StyleLength::Px(std::max(0.0f, x + 18.0f)))
        .Set(Style::PositionTop, StyleLength::Px(std::max(0.0f, y - 14.0f)));
    m_ChartDragGhost->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void MonitorsPanel::HideChartDragGhost()
{
    if (m_ChartDragGhost)
    {
        if (UIElement* parent = m_ChartDragGhost->GetParent())
            parent->RemoveChild(m_ChartDragGhost);
        m_ChartDragGhost = nullptr;
    }
}

void MonitorsPanel::ShowChartDropIndicator(const ChartDropTarget& target)
{
    if (target.Placement == ChartDropPlacement::None)
    {
        HideChartDropIndicator();
        return;
    }

    UIManager* ui = GetOwnerManager();
    UIElement* root = ui ? ui->GetRootElement() : nullptr;
    if (!root)
        return;

    if (!m_ChartDropIndicator)
    {
        auto indicator = std::make_unique<UIElement>();
        indicator->AddClass("monitors-chart-drop-indicator");
        indicator->SetOverlayLayer(OverlayLayer::DragPreview);
        indicator->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PointerEvents, false)
            .Set(Style::ZIndex, 9390)
            .Set(Style::Display, DisplayMode::None)
            .Set(Style::BackgroundColor, static_cast<uint32_t>(0x2E3A8FFFu))
            .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
            .Set(Style::BorderTopColor, static_cast<uint32_t>(0xCC3A8FFFu))
            .Set(Style::BorderRightColor, static_cast<uint32_t>(0xCC3A8FFFu))
            .Set(Style::BorderBottomColor, static_cast<uint32_t>(0xCC3A8FFFu))
            .Set(Style::BorderLeftColor, static_cast<uint32_t>(0xCC3A8FFFu))
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f});
        m_ChartDropIndicator = indicator.get();
        root->AddChild(std::move(indicator));
    }

    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool visible = false;

    auto useStrip = [&](const UIElement* strip, ChartDropPlacement placement, bool newRow, bool fullStrip) {
        if (!strip)
            return;

        const float sx = strip->GetLayoutX();
        const float sy = strip->GetLayoutY();
        const float sw = strip->GetLayoutWidth();
        const float sh = strip->GetLayoutHeight();
        const float gap = 6.0f;
        if (fullStrip)
        {
            left = sx;
            top = sy;
            width = sw;
            height = sh;
        }
        else if (newRow)
        {
            left = m_ChartsContainer ? m_ChartsContainer->GetLayoutX() : sx;
            top = placement == ChartDropPlacement::Before ? sy : sy + sh + gap;
            width = m_ChartsContainer ? m_ChartsContainer->GetLayoutWidth() : sw;
            height = sh;
        }
        else
        {
            left = placement == ChartDropPlacement::After ? sx + sw * 0.5f : sx;
            top = sy;
            width = sw * 0.5f;
            height = sh;
        }
        visible = true;
    };

    if (!target.Name.empty())
    {
        auto it = m_Charts.find(target.Name);
        if (it != m_Charts.end())
        {
            bool fullStrip = false;
            if (!m_DragChartName.empty() && !target.NewRow)
            {
                auto sourceIt = std::find(m_ChartOrder.begin(), m_ChartOrder.end(), m_DragChartName);
                auto targetIt = std::find(m_ChartOrder.begin(), m_ChartOrder.end(), target.Name);
                fullStrip = sourceIt != m_ChartOrder.end() &&
                            targetIt != m_ChartOrder.end() &&
                            (std::next(sourceIt) == targetIt || std::next(targetIt) == sourceIt);
            }
            useStrip(it->second.Strip, target.Placement, target.NewRow, fullStrip);
        }
    }
    else if (target.Placement == ChartDropPlacement::Append)
    {
        UIElement* fallback = nullptr;
        for (auto it = m_ChartOrder.rbegin(); it != m_ChartOrder.rend(); ++it)
        {
            auto chartIt = m_Charts.find(*it);
            if (chartIt == m_Charts.end())
                continue;
            fallback = chartIt->second.Strip;
            if (*it != m_DragChartName)
                break;
        }

        if (fallback)
        {
            useStrip(fallback, ChartDropPlacement::After, true, false);
        }
        else if (m_ChartsContainer)
        {
            left = m_ChartsContainer->GetLayoutX();
            top = m_ChartsContainer->GetLayoutY();
            width = m_ChartsContainer->GetLayoutWidth();
            height = 160.0f;
            visible = true;
        }
    }

    if (!visible || !m_ChartDropIndicator)
    {
        HideChartDropIndicator();
        return;
    }

    m_ChartDropIndicator->Overrides()
        .Set(Style::PositionLeft, StyleLength::Px(std::max(0.0f, left)))
        .Set(Style::PositionTop, StyleLength::Px(std::max(0.0f, top)))
        .Set(Style::Width, StyleLength::Px(std::max(32.0f, width)))
        .Set(Style::Height, StyleLength::Px(std::max(32.0f, height)))
        .Set(Style::Display, DisplayMode::Block);
    m_ChartDropIndicator->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void MonitorsPanel::HideChartDropIndicator()
{
    if (m_ChartDropIndicator)
    {
        if (UIElement* parent = m_ChartDropIndicator->GetParent())
            parent->RemoveChild(m_ChartDropIndicator);
        m_ChartDropIndicator = nullptr;
    }
}

void MonitorsPanel::PushBuiltInSamples()
{
    auto& metrics = Debug::DebugMetrics::Get();

    // Time/FPS and Time/FrameMs (and the CPU phase timers) are pushed per-frame
    // by CollectEditorDebugMetrics into the editor-visible DebugMetrics instance:
    // FPS is the shared sliding-window estimate, while FrameMs is raw per-frame
    // cost so individual frame events register as distinct samples. They can't be
    // read from Application::Tick's own pushes here — that copy of the Engine
    // static lib (linked into GameEngine.Native.dll) writes a different singleton.
    // The row text for FrameMs is resolved separately in UpdateValues: live reads
    // from Application::GetFps() so it stays consistent with the FPS row, scrub
    // reads the frozen series sample at the selected frame.
    if (auto* world = EngineCore::GetInstance().GetPrimaryWorld())
    {
        metrics.PushSample("ECS/Entities", static_cast<float>(world->GetEntityCount()));
        metrics.PushSample("ECS/Archetypes", static_cast<float>(world->GetArchetypeCount()));
    }

    auto* rs = EngineCore::GetInstance().GetRenderServices();
    Rendering::IDevice* device = rs ? rs->GetDevice() : nullptr;
    if (!device)
        return;

    const Rendering::HdrOutputState state = device->GetHdrOutputState();
    const float paperWhite = std::clamp(state.staticMetadata.paperWhiteNits, 40.0f, 1000.0f);
    metrics.PushSample("HDR/Active", Rendering::IsHdrOutputModeActive(state.activeMode) ? 100.0f : 0.0f);
    metrics.PushSample("HDR/PaperWhiteNits", paperWhite);
    metrics.PushSample("HDR/PeakNits", Rendering::GetHdrOutputPeakLuminanceNits(state));
    metrics.PushSample("HDR/OutputMaxLinear", Rendering::GetHdrOutputMaxLinearValue(state));
}

void MonitorsPanel::UpdateHdrStatusLabel()
{
    if (!m_HdrStatusLabel)
        return;

    auto* rs = EngineCore::GetInstance().GetRenderServices();
    Rendering::IDevice* device = rs ? rs->GetDevice() : nullptr;
    const std::string text = FormatHdrStatus(device);
    if (text == m_LastHdrStatusText)
        return;

    m_LastHdrStatusText = text;
    m_HdrStatusLabel->SetText(text);
}

void MonitorsPanel::Update()
{
    PushBuiltInSamples();
    UpdateHdrStatusLabel();

    if (GetLayoutWidth() <= 0.0f || GetLayoutHeight() <= 0.0f)
        return;

    ++m_FrameCounter;

    if ((m_FrameCounter & 3) == 0)
        RebuildMonitorList();

    UpdateValues();

    // Charts no longer ride on SetText invalidation (SetText infers
    // content-only impact for fixed-size labels). Repaint every other
    // frame (~30Hz) so the line animates without per-frame primitive churn.
    if (!m_Paused && (m_FrameCounter & 1) == 0)
    {
        for (auto& [name, chart] : m_Charts)
        {
            // Content-only: a chart line redraw rewrites the element's own
            // primitive bytes at a fixed rect and never feeds layout or
            // style — the render-side drain re-emits it while the Update
            // side idle-gates the frame.
            if (chart.Chart)
                chart.Chart->MarkContentDirty();
        }
    }
}

void MonitorsPanel::OnPostLayout()
{
    if (!m_StyleAttached && !m_StyleLoadScheduled && GetOwnerManager())
    {
        m_StyleLoadScheduled = true;
        PostAction([this]() { LoadAndAttachPanelStyle(); });
    }
}

void MonitorsPanel::LoadAndAttachPanelStyle()
{
    m_StyleLoadScheduled = false;
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;
    auto& am = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path styleAssetPath =
        std::filesystem::path("UI") / "panels" / "MonitorsPanel.css";
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
