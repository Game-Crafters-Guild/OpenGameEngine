#include "Panels/RenderGraphPanel.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "DebugServer/RenderDocCapture.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGGraph.h"
#include "Rendering/Core/RenderGraph/RGTypes.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "Platform/Shell.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Splitter.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

namespace GameEngine {

namespace {

namespace RG = Rendering::RenderGraph;
namespace Pipeline = ::GameEngine::Engine::Renderer::Pipeline;

const char* SeverityPill(Pipeline::PipelineIssueSeverity s)
{
    switch (s)
    {
    case Pipeline::PipelineIssueSeverity::Error:   return "ERROR";
    case Pipeline::PipelineIssueSeverity::Warning: return "WARN";
    case Pipeline::PipelineIssueSeverity::Info:    return "INFO";
    }
    return "?";
}

const char* SeverityRowClass(Pipeline::PipelineIssueSeverity s)
{
    switch (s)
    {
    case Pipeline::PipelineIssueSeverity::Error:   return "error";
    case Pipeline::PipelineIssueSeverity::Warning: return "warn";
    case Pipeline::PipelineIssueSeverity::Info:    return "info";
    }
    return "info";
}

const char* QueueName(std::uint32_t q)
{
    switch (q)
    {
    case 0: return "Graphics";
    case 1: return "Compute";
    case 2: return "Copy";
    default: return "?";
    }
}

const char* LayoutName(RG::RGImageLayout l)
{
    switch (l)
    {
    case RG::RGImageLayout::Undefined:       return "Undefined";
    case RG::RGImageLayout::General:         return "General";
    case RG::RGImageLayout::ColorAttachment: return "ColorAttachment";
    case RG::RGImageLayout::DepthAttachment: return "DepthAttachment";
    case RG::RGImageLayout::DepthReadOnly:   return "DepthReadOnly";
    case RG::RGImageLayout::ShaderReadOnly:  return "ShaderReadOnly";
    case RG::RGImageLayout::TransferSrc:     return "TransferSrc";
    case RG::RGImageLayout::TransferDst:     return "TransferDst";
    case RG::RGImageLayout::Present:         return "Present";
    }
    return "?";
}

const char* AccessName(RG::RGAccess a)
{
    switch (a)
    {
    case RG::RGAccess::Sampled:         return "Sampled";
    case RG::RGAccess::SampledCompute:  return "SampledCompute";
    case RG::RGAccess::SampledVertex:   return "SampledVertex";
    case RG::RGAccess::UniformRead:     return "Uniform";
    case RG::RGAccess::StorageRead:     return "StorageRead";
    case RG::RGAccess::IndirectRead:    return "Indirect";
    case RG::RGAccess::IndexRead:       return "Index";
    case RG::RGAccess::VertexRead:      return "Vertex";
    case RG::RGAccess::CopySrc:         return "CopySrc";
    case RG::RGAccess::DepthRead:       return "DepthRead";
    case RG::RGAccess::ColorLoad:       return "ColorLoad";
    case RG::RGAccess::AccelerationStructureRead: return "AccelerationStructureRead";
    case RG::RGAccess::ColorAttachment: return "ColorAttachment";
    case RG::RGAccess::DepthWrite:      return "DepthWrite";
    case RG::RGAccess::StorageWrite:    return "StorageWrite";
    case RG::RGAccess::CopyDst:         return "CopyDst";
    case RG::RGAccess::AccelerationStructureBuild: return "AccelerationStructureBuild";
    }
    return "?";
}

struct PassInfo
{
    struct Access
    {
        std::string resourceName;
        std::string state;
        bool isWrite = false;
    };
    std::string name;
    std::uint32_t queueType = 0;
    std::uint32_t insertionOrder = 0;
    std::uint32_t barrierCount = 0;
    bool enabled = true;
    std::vector<Access> accesses;
    std::vector<std::uint32_t> dependsOn;
};

struct BarrierDetail
{
    std::string resourceName;
    std::string stateBefore;
    std::string stateAfter;
};

// Sum the barrier counts of every batch emitted before pass `p`.
std::uint32_t BarrierCountForPass(const RG::RGGraph& g, RG::RGPassId p)
{
    std::uint32_t n = 0;
    for (const auto& batch : g.BarrierBatches())
        if (batch.Pass == p)
            n += batch.Count;
    return n;
}

// Snapshot the live graph's scheduled (live, non-culled) passes in execution
// order. Read at editor-update time, when the frame still holds the previous
// frame's compiled graph (cleared only at the next BeginFrame).
std::vector<PassInfo> QueryCompiledPasses(RG::RGFrame* frame)
{
    std::vector<PassInfo> out;
    if (!frame)
        return out;
    const RG::RGGraph& g = frame->Graph();
    const auto& order = g.ScheduledOrder();
    out.reserve(order.size());
    std::uint32_t idx = 0;
    for (RG::RGPassId p : order)
    {
        PassInfo pi;
        const char* name = g.PassName(p);
        pi.name = name ? name : "(unnamed)";
        pi.queueType = static_cast<std::uint32_t>(g.PassQueue(p));
        pi.insertionOrder = idx++;
        pi.barrierCount = BarrierCountForPass(g, p);
        pi.enabled = true;
        for (RG::RGPassId succ : g.PassSuccessors(p))
            pi.dependsOn.push_back(succ);
        for (const RG::RGAccessRecord& a : g.Accesses())
        {
            if (a.Pass != p)
                continue;
            PassInfo::Access acc;
            const char* rn = g.ResourceName(a.Resource);
            acc.resourceName = rn ? rn : "(resource)";
            acc.state = AccessName(a.Access);
            acc.isWrite = RG::IsWrite(a.Access);
            pi.accesses.push_back(std::move(acc));
        }
        out.push_back(std::move(pi));
    }
    return out;
}

// Barrier transitions emitted before the named pass.
std::vector<BarrierDetail> QueryBarrierDetails(RG::RGFrame* frame, const std::string& passName)
{
    std::vector<BarrierDetail> out;
    if (!frame)
        return out;
    const RG::RGGraph& g = frame->Graph();

    RG::RGPassId target = RG::kInvalidId;
    for (RG::RGPassId p : g.ScheduledOrder())
    {
        const char* name = g.PassName(p);
        if (name && passName == name)
        {
            target = p;
            break;
        }
    }
    if (target == RG::kInvalidId)
        return out;

    const auto& barriers = g.Barriers();
    for (const auto& batch : g.BarrierBatches())
    {
        if (batch.Pass != target)
            continue;
        for (std::uint32_t i = 0; i < batch.Count; ++i)
        {
            const std::uint32_t bi = batch.First + i;
            if (bi >= barriers.size())
                break;
            const RG::RGBarrier& b = barriers[bi];
            BarrierDetail d;
            const char* rn = g.ResourceName(b.Resource);
            d.resourceName = rn ? rn : "(resource)";
            d.stateBefore = LayoutName(b.OldLayout);
            d.stateAfter = LayoutName(b.NewLayout);
            out.push_back(std::move(d));
        }
    }
    return out;
}

} // namespace

RenderGraphPanel::~RenderGraphPanel()
{
    // The pending asset callbacks capture this panel; withdraw them before it goes
    // away (destroying an AssetLoadHandle does not).
    if (m_PanelStyleLoadHandle)
        m_PanelStyleLoadHandle->Cancel();
}

RenderGraphPanel::RenderGraphPanel()
    : DockPanel("Render Graph")
{
    AddClass("render-graph-panel");
    BuildUI();
}

void RenderGraphPanel::BuildUI()
{
    // Toolbar row 1 (main actions + overview)
    auto toolbar = std::make_unique<UIElement>();
    toolbar->AddClass("render-graph-toolbar");

    auto profileBtn = std::make_unique<Button>();
    profileBtn->AddClass("small");
    profileBtn->AddClass("secondary");
    profileBtn->AddClass("render-graph-btn-profile");
    profileBtn->SetText("Profile");
    profileBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e) {
        Button& b = static_cast<Button&>(*e.CurrentTarget);
        m_ProfilingEnabled = !m_ProfilingEnabled;
        if (m_LastFrame)
            m_LastFrame->SetProfilingEnabled(m_ProfilingEnabled);
        b.SetText(m_ProfilingEnabled ? "Stop Profiling" : "Profile");
    });
    m_ProfileButton = profileBtn.get();
    toolbar->AddChild(std::move(profileBtn));

    // Profiling-state hint: per-pass timings stay zero until profiling is armed,
    // and GE_ENABLE_GPU_PROFILING=0 makes the Profile button a no-op. Text is
    // reconciled each refresh in RefreshProfilingHint().
    auto profileHint = std::make_unique<Label>();
    profileHint->AddClass("render-graph-profile-hint");
    m_ProfilingHint = profileHint.get();
    toolbar->AddChild(std::move(profileHint));

    auto captureBtn = std::make_unique<Button>();
    captureBtn->AddClass("small");
    captureBtn->AddClass("secondary");
    captureBtn->AddClass("render-graph-btn-capture");
    captureBtn->SetText("Capture in RenderDoc");
    captureBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (!m_LastRenderDoc || !m_LastRenderDoc->IsAvailable())
            return;
        m_CaptureCountAtTrigger = m_LastRenderDoc->GetCaptureCount();
        m_CapturePending = true;
        m_CapturePendingFrames = 0;
        m_LastRenderDoc->TriggerCapture();
        if (m_CaptureHint)
            m_CaptureHint->SetText("capturing...");
    });
    m_CaptureButton = captureBtn.get();
    toolbar->AddChild(std::move(captureBtn));

    // Capture-state hint: the .rdc lands several frames after the click, so the
    // filename is reconciled in RefreshCaptureHint() rather than at click time.
    auto captureHint = std::make_unique<Label>();
    captureHint->AddClass("render-graph-capture-hint");
    m_CaptureHint = captureHint.get();
    toolbar->AddChild(std::move(captureHint));

    auto exportBtn = std::make_unique<Button>();
    exportBtn->AddClass("small");
    exportBtn->AddClass("secondary");
    exportBtn->AddClass("render-graph-btn-export");
    exportBtn->SetText("Export CSV");
    exportBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ExportCsv(); });
    m_ExportCsvButton = exportBtn.get();
    toolbar->AddChild(std::move(exportBtn));

    auto sortBtn = std::make_unique<Button>();
    sortBtn->AddClass("small");
    sortBtn->AddClass("secondary");
    sortBtn->AddClass("render-graph-btn-sort");
    sortBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { CycleSortMode(); });
    m_SortButton = sortBtn.get();
    toolbar->AddChild(std::move(sortBtn));
    UpdateSortButtonText();

    auto search = std::make_unique<TextField>();
    search->AddClass("render-graph-search");
    search->SetOnValueChanging([this](const std::string& v) {
        m_SearchText = v;
        ApplySearchFilter();
    });
    m_SearchField = search.get();
    toolbar->AddChild(std::move(search));

    // Flex spacer so the hardware label is always pushed to the far right
    // edge of the toolbar, even when the toolbar wraps at narrow widths.
    auto spacer = std::make_unique<UIElement>();
    spacer->AddClass("render-graph-toolbar-spacer");
    toolbar->AddChild(std::move(spacer));

    auto hwLabel = std::make_unique<Label>();
    hwLabel->AddClass("render-graph-hardware");
    hwLabel->SetText("—");
    m_HardwareLabel = hwLabel.get();
    toolbar->AddChild(std::move(hwLabel));

    AddChild(std::move(toolbar));

    // Row 2: overview summary (passes / resources / barriers), sits under
    // the toolbar buttons. Built as a sequence of alternating number/word
    // labels so the numbers can render in a brighter color than the words.
    auto overviewRow = std::make_unique<UIElement>();
    overviewRow->AddClass("render-graph-overview-row");

    auto makeNum = [&overviewRow](Label*& slot) {
        auto n = std::make_unique<Label>();
        n->AddClass("render-graph-overview-num");
        n->SetText("—");
        slot = n.get();
        overviewRow->AddChild(std::move(n));
    };
    auto makeWord = [&overviewRow](const char* text) {
        auto w = std::make_unique<Label>();
        w->AddClass("render-graph-overview-word");
        w->SetText(text);
        overviewRow->AddChild(std::move(w));
    };

    makeNum(m_OverviewPassCount);
    makeWord("passes");
    makeNum(m_OverviewResourceCount);
    makeWord("resources");
    makeNum(m_OverviewBarrierCount);
    makeWord("barriers   (");
    makeWord("transient");
    makeNum(m_OverviewTransientCount);
    makeWord("/ persistent");
    makeNum(m_OverviewPersistentCount);
    makeWord("/ imported");
    makeNum(m_OverviewImportedCount);
    makeWord(")");

    AddChild(std::move(overviewRow));

    // Row 3: last-Execute recording stats (frame->Stats()) + the window-scope
    // hint. Same alternating number/word layout as the overview row.
    auto statsRow = std::make_unique<UIElement>();
    statsRow->AddClass("render-graph-overview-row");

    auto makeStatNum = [&statsRow](Label*& slot) {
        auto n = std::make_unique<Label>();
        n->AddClass("render-graph-overview-num");
        n->SetText("—");
        slot = n.get();
        statsRow->AddChild(std::move(n));
    };
    auto makeStatWord = [&statsRow](const char* text) {
        auto w = std::make_unique<Label>();
        w->AddClass("render-graph-overview-word");
        w->SetText(text);
        statsRow->AddChild(std::move(w));
    };

    makeStatNum(m_OverviewSubmissions);
    makeStatWord("submissions");
    makeStatNum(m_OverviewRenderPasses);
    makeStatWord("render passes");
    makeStatNum(m_OverviewBarrierBatches);
    makeStatWord("barrier batches   (");
    makeStatNum(m_OverviewBarriersHoisted);
    makeStatWord("hoisted )");

    auto hintSpacer = std::make_unique<UIElement>();
    hintSpacer->AddClass("render-graph-toolbar-spacer");
    statsRow->AddChild(std::move(hintSpacer));

    auto windowHint = std::make_unique<Label>();
    windowHint->AddClass("render-graph-window-hint");
    windowHint->SetText("main window (0)");
    m_WindowHint = windowHint.get();
    statsRow->AddChild(std::move(windowHint));

    AddChild(std::move(statsRow));

    BuildValidationSection();

    // Body: [ pass list pane | splitter | detail pane ]. Panes are
    // WeightedPanes so drag updates route through the UIManager_Layout
    // fast path (direct YGNodeStyleSetFlexGrow after ApplyStyle), which
    // keeps the split drag smooth instead of going through inline overrides.
    auto body = std::make_unique<UIElement>();
    body->AddClass("render-graph-body");
    m_BodyElement = body.get();

    auto listPane = std::make_unique<WeightedPane>(0.4f);
    listPane->AddClass("render-graph-pane");
    {
        auto listScroll = std::make_unique<ScrollView>();
        listScroll->AddClass("render-graph-passes-list");
        m_PassListScroll = listScroll.get();
        m_PassListContainer = listScroll->GetViewport();
        // Viewport is the stable hit target for bubbled row events; make it
        // focusable so arrow keys route here after a row click.
        m_PassListContainer->SetFocusable(true);
        m_PassListContainer->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
            if (e.Key == Input::kKeyCode_Up)
            {
                MoveSelection(-1);
                e.Stop();
            }
            else if (e.Key == Input::kKeyCode_Down)
            {
                MoveSelection(1);
                e.Stop();
            }
            else if (e.Key == Input::kKeyCode_Space)
            {
                m_ProfilingEnabled = !m_ProfilingEnabled;
                if (m_LastFrame)
                    m_LastFrame->SetProfilingEnabled(m_ProfilingEnabled);
                if (m_ProfileButton)
                    m_ProfileButton->SetText(m_ProfilingEnabled ? "Stop Profiling" : "Profile");
                e.Stop();
            }
        });
        listPane->AddChild(std::move(listScroll));
    }
    m_ListPane = listPane.get();
    body->AddChild(std::move(listPane));

    auto splitter = std::make_unique<Splitter>();
    splitter->AddClass("splitter");
    splitter->AddClass("row");
    splitter->AddClass("render-graph-splitter");
    m_SplitterElement = splitter.get();
    body->AddChild(std::move(splitter));

    auto detailPane = std::make_unique<WeightedPane>(0.6f);
    detailPane->AddClass("render-graph-pane");
    {
        auto detailScroll = std::make_unique<ScrollView>();
        detailScroll->AddClass("render-graph-detail");
        m_DetailContainer = detailScroll->GetViewport();
        detailPane->AddChild(std::move(detailScroll));
    }
    m_DetailPane = detailPane.get();
    body->AddChild(std::move(detailPane));

    AddChild(std::move(body));
}

void RenderGraphPanel::BuildValidationSection()
{
    // The whole section, built once. Banner + error list are shown/hidden and
    // the warn list rebuilt by RefreshValidation; the collapse toggle acts
    // immediately via its own callback (not gated on the refresh throttle).
    auto section = std::make_unique<UIElement>();
    section->AddClass("render-graph-validation");

    auto banner = std::make_unique<UIElement>();
    banner->AddClass("render-graph-validation-banner");
    banner->AddClass("hidden");
    {
        auto label = std::make_unique<Label>();
        label->AddClass("render-graph-validation-banner-label");
        m_ValidationBannerLabel = label.get();
        banner->AddChild(std::move(label));
    }
    m_ValidationBanner = banner.get();
    section->AddChild(std::move(banner));

    auto errorList = std::make_unique<UIElement>();
    errorList->AddClass("render-graph-validation-list");
    m_ValidationErrorList = errorList.get();
    section->AddChild(std::move(errorList));

    auto toggle = std::make_unique<Button>();
    toggle->AddClass("small");
    toggle->AddClass("secondary");
    toggle->AddClass("render-graph-validation-warntoggle");
    toggle->AddClass("hidden");
    toggle->SetText("Show warnings");
    toggle->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { SetWarningsExpanded(!m_WarningsExpanded); });
    m_WarningsToggle = toggle.get();
    section->AddChild(std::move(toggle));

    auto warnList = std::make_unique<UIElement>();
    warnList->AddClass("render-graph-validation-list");
    warnList->AddClass("hidden");
    m_ValidationWarnList = warnList.get();
    section->AddChild(std::move(warnList));

    m_ValidationSection = section.get();
    AddChild(std::move(section));
}

void RenderGraphPanel::Update(RenderDocCapture* renderDoc, Rendering::RenderGraph::RGFrame* frame)
{
    m_LastRenderDoc = renderDoc;
    m_LastFrame = frame;

    // Skip all work when the tab is not the active one in its dock leaf
    // (layout collapses inactive tabs to zero-size).
    if (GetLayoutWidth() <= 0.0f || GetLayoutHeight() <= 0.0f)
        return;

    // Reflect the shared per-frame profiling state (another panel — e.g. the
    // Visual Profiler — may have armed it). The Profile button/Space toggle this
    // panel triggers act on the frame directly, so we never fight here.
    if (frame)
        m_ProfilingEnabled = frame->ProfilingEnabled();

    if (m_CaptureButton)
    {
        const bool enabled = renderDoc && renderDoc->IsAvailable();
        if (enabled)
            m_CaptureButton->RemoveClass("disabled");
        else
            m_CaptureButton->AddClass("disabled");
    }

    if (m_ProfileButton)
        m_ProfileButton->SetText(m_ProfilingEnabled ? "Stop Profiling" : "Profile");
    RefreshProfilingHint();
    RefreshCaptureHint();

    // Rebuild the detail pane on the frame after a click. Runs outside the
    // throttle gate so selection feels instant without doing DOM mutation
    // inside the click event handler.
    if (m_DetailDirty)
    {
        RefreshDetail();
        m_DetailDirty = false;
    }

    // Throttle heavy debug queries. Graph topology changes rarely; polling at
    // ~0.5 Hz keeps the panel responsive without stalling the main thread.
    if ((++m_FrameCounter % 30) != 0)
        return;

    RefreshOverview();
    RefreshValidation();
    RefreshHardwareLabel();
    RefreshTimings();
    RebuildPassList();
    RefreshRowMetas();
}

void RenderGraphPanel::RefreshHardwareLabel()
{
    if (!m_HardwareLabel)
        return;
    if (m_LastFrame && m_LastFrame->Device())
    {
        std::string hw = m_LastFrame->Device()->GetHardwareDescription();
        m_HardwareLabel->SetText(hw.empty() ? "—" : hw);
    }
    else
    {
        m_HardwareLabel->SetText("—");
    }
}

void RenderGraphPanel::RefreshTimings()
{
    // Overlay the live graph's last resolved per-pass timings, keyed by name
    // (resolved ~FramesInFlight frames late; absent when profiling is off).
    m_TimingByName.clear();
    if (!m_LastFrame)
        return;
    for (const auto& t : m_LastFrame->LastFrameTimings())
        m_TimingByName[t.Name] = {t.CpuMs, t.GpuSpanMs};
}

void RenderGraphPanel::RefreshRowMetas()
{
    for (auto& r : m_Rows)
    {
        if (r.BarrierNumLabel)
        {
            char n[16];
            std::snprintf(n, sizeof(n), "%u", r.BarrierCount);
            r.BarrierNumLabel->SetText(n);
        }
        if (r.BarrierWordLabel)
            r.BarrierWordLabel->SetText(r.BarrierCount == 1 ? "barrier" : "barriers");

        auto it = m_TimingByName.find(r.Name);
        const bool hasTiming = it != m_TimingByName.end();
        if (r.CpuGroup)
        {
            if (hasTiming)
                r.CpuGroup->RemoveClass("hidden");
            else
                r.CpuGroup->AddClass("hidden");
        }
        if (r.GpuGroup)
        {
            if (hasTiming)
                r.GpuGroup->RemoveClass("hidden");
            else
                r.GpuGroup->AddClass("hidden");
        }
        if (hasTiming)
        {
            char buf[16];
            if (r.CpuNumLabel)
            {
                std::snprintf(buf, sizeof(buf), "%.2f", it->second.first);
                r.CpuNumLabel->SetText(buf);
            }
            if (r.GpuNumLabel)
            {
                std::snprintf(buf, sizeof(buf), "%.2f", it->second.second);
                r.GpuNumLabel->SetText(buf);
            }
        }
    }
}

void RenderGraphPanel::RefreshOverview()
{
    if (!m_OverviewPassCount)
        return;
    auto setNum = [](Label* lbl, std::size_t v) {
        if (!lbl) return;
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%zu", v);
        lbl->SetText(buf);
    };

    if (!m_LastFrame)
    {
        setNum(m_OverviewPassCount, 0);
        setNum(m_OverviewResourceCount, 0);
        setNum(m_OverviewBarrierCount, 0);
        setNum(m_OverviewTransientCount, 0);
        setNum(m_OverviewPersistentCount, 0);
        setNum(m_OverviewImportedCount, 0);
        setNum(m_OverviewSubmissions, 0);
        setNum(m_OverviewRenderPasses, 0);
        setNum(m_OverviewBarrierBatches, 0);
        setNum(m_OverviewBarriersHoisted, 0);
        return;
    }

    const RG::RGGraph& g = m_LastFrame->Graph();
    std::size_t transient = 0, persistent = 0, imported = 0;
    const std::size_t resCount = g.ResourceCount();
    for (RG::RGResourceId r = 0; r < resCount; ++r)
    {
        if (!g.IsResourceUsed(r))
            continue;
        if (g.IsImported(r))
            ++persistent;          // cross-frame pool import
        else if (g.IsExternal(r))
            ++imported;            // external (swapchain / UI-owned / sink)
        else
            ++transient;
    }

    setNum(m_OverviewPassCount, g.ScheduledOrder().size());
    setNum(m_OverviewResourceCount, resCount);
    setNum(m_OverviewBarrierCount, g.BarrierCount());
    setNum(m_OverviewTransientCount, transient);
    setNum(m_OverviewPersistentCount, persistent);
    setNum(m_OverviewImportedCount, imported);

    // Last-Execute recording stats (submissions, render passes begun, barrier
    // batches, hoisted barriers) — a scheduling view the graph topology can't show.
    const RG::RGFrame::FrameStats& stats = m_LastFrame->Stats();
    setNum(m_OverviewSubmissions, stats.SubmissionsMade);
    setNum(m_OverviewRenderPasses, stats.RenderPassesBegun);
    setNum(m_OverviewBarrierBatches, stats.BarrierBatchesEmitted);
    setNum(m_OverviewBarriersHoisted, stats.BarriersHoisted);
}

void RenderGraphPanel::RefreshProfilingHint()
{
    if (!m_ProfilingHint)
        return;
#if GE_ENABLE_GPU_PROFILING
    m_ProfilingHint->SetText(m_ProfilingEnabled ? "profiling — timings resolve a few frames late"
                                                : "timings 0 until Profile is armed");
#else
    m_ProfilingHint->SetText("GPU profiling disabled at build (GE_ENABLE_GPU_PROFILING=0)");
#endif
}

void RenderGraphPanel::RefreshCaptureHint()
{
    if (!m_CaptureHint)
        return;

    if (!m_LastRenderDoc || !m_LastRenderDoc->IsAvailable())
    {
        // Reconciled every frame, so the text is a constant rather than a
        // temporary rebuilt from a literal each time.
        static const std::string kUnavailable = "run under RenderDoc to capture";
        m_CaptureHint->SetText(kUnavailable);
        return;
    }

    // Nothing in flight: leave the last capture's name on screen.
    if (!m_CapturePending)
        return;

    if (m_LastRenderDoc->GetCaptureCount() <= m_CaptureCountAtTrigger)
    {
        // RenderDoc is still writing the .rdc. Give up on the same budget the
        // trigger_capture handler uses, so a capture that never lands stops
        // claiming to be in flight.
        if (++m_CapturePendingFrames > kCapturePendingBudgetFrames)
        {
            m_CapturePending = false;
            static const std::string kNoCapture = "capture did not complete";
            m_CaptureHint->SetText(kNoCapture);
        }
        return;
    }

    m_CapturePending = false;
    const std::string path = m_LastRenderDoc->GetLastCapturePath();
    m_CaptureHint->SetText(std::filesystem::path(path).filename().string());
    Logger::Log::Info("RenderDoc: Capture written to '{}'", path);
}

void RenderGraphPanel::SetWarningsExpanded(bool expanded)
{
    m_WarningsExpanded = expanded;
    if (m_ValidationWarnList)
    {
        if (expanded)
            m_ValidationWarnList->RemoveClass("hidden");
        else
            m_ValidationWarnList->AddClass("hidden");
    }
    // Rebuild the button text (count is appended in RefreshValidation, so keep
    // the current count by re-reading it there on the next refresh; here we only
    // flip the caret). RefreshValidation owns the authoritative label.
    RefreshValidation();
}

void RenderGraphPanel::RefreshValidation()
{
    if (!m_ValidationSection)
        return;

    Engine::Renderer::RenderServices* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
    {
        m_ValidationSection->AddClass("hidden");
        return;
    }
    const Engine::Renderer::PipelineCompileReport& report = rs->Spine().LastCompileReport();

    // Re-key only the caret text when nothing changed but the collapse state
    // did: SetWarningsExpanded calls back into here, so always reconcile the
    // toggle label; skip the expensive list rebuild unless Generation moved.
    const bool contentChanged = report.Generation != m_LastValidationGen;
    m_LastValidationGen = report.Generation;

    // Tally severities: errors are shown by default, non-errors collapse behind
    // the toggle.
    std::size_t errorCount = 0, warnCount = 0;
    for (const auto& iss : report.Issues)
    {
        if (iss.severity == Pipeline::PipelineIssueSeverity::Error)
            ++errorCount;
        else
            ++warnCount;
    }
    const bool hasBanner = report.Rejected || !report.LoadFailureReason.empty();

    // Nothing to surface (healthy pipeline, no issues) — collapse the strip.
    if (!hasBanner && errorCount == 0 && warnCount == 0)
    {
        m_ValidationSection->AddClass("hidden");
        return;
    }
    m_ValidationSection->RemoveClass("hidden");

    // Banner: Rejected (compile errors, last-good kept) or a load/parse failure.
    if (m_ValidationBanner && m_ValidationBannerLabel)
    {
        if (report.Rejected)
        {
            m_ValidationBannerLabel->SetText(
                "Edit refused — active pipeline unchanged (last-good kept).");
            m_ValidationBanner->RemoveClass("hidden");
        }
        else if (!report.LoadFailureReason.empty())
        {
            m_ValidationBannerLabel->SetText("Pipeline failed to load (" + report.LoadFailureReason +
                                             ") — rendering on last-good.");
            m_ValidationBanner->RemoveClass("hidden");
        }
        else
        {
            m_ValidationBanner->AddClass("hidden");
        }
    }

    if (m_WarningsToggle)
    {
        if (warnCount == 0)
        {
            m_WarningsToggle->AddClass("hidden");
        }
        else
        {
            m_WarningsToggle->RemoveClass("hidden");
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%s warnings (%zu)",
                          m_WarningsExpanded ? "Hide" : "Show", warnCount);
            m_WarningsToggle->SetText(buf);
        }
    }

    if (!contentChanged)
        return;

    auto addIssueRow = [](UIElement* list, const Pipeline::PipelineIssue& iss) {
        auto row = std::make_unique<UIElement>();
        row->AddClass("render-graph-validation-row");
        row->AddClass(SeverityRowClass(iss.severity));

        auto pill = std::make_unique<Label>();
        pill->AddClass("render-graph-validation-pill");
        pill->SetText(SeverityPill(iss.severity));
        row->AddChild(std::move(pill));

        if (!iss.nodeId.empty())
        {
            auto node = std::make_unique<Label>();
            node->AddClass("render-graph-validation-node");
            node->SetText(iss.nodeId);
            row->AddChild(std::move(node));
        }

        auto msg = std::make_unique<Label>();
        msg->AddClass("render-graph-validation-msg");
        msg->SetText(iss.message);
        row->AddChild(std::move(msg));

        list->AddChild(std::move(row));
    };

    if (m_ValidationErrorList)
    {
        m_ValidationErrorList->RemoveAllChildren();
        for (const auto& iss : report.Issues)
            if (iss.severity == Pipeline::PipelineIssueSeverity::Error)
                addIssueRow(m_ValidationErrorList, iss);
    }
    if (m_ValidationWarnList)
    {
        m_ValidationWarnList->RemoveAllChildren();
        for (const auto& iss : report.Issues)
            if (iss.severity != Pipeline::PipelineIssueSeverity::Error)
                addIssueRow(m_ValidationWarnList, iss);
    }
}

void RenderGraphPanel::RebuildPassList()
{
    if (!m_PassListContainer)
        return;

    std::vector<PassInfo> passes = QueryCompiledPasses(m_LastFrame);

    // Apply the current sort mode. CpuDesc/GpuDesc rely on m_TimingByName
    // having been refreshed earlier in Update(). Stable sort preserves
    // insertion order as the tiebreaker.
    auto timingOf = [this](const std::string& n) -> std::pair<double, double> {
        auto it = m_TimingByName.find(n);
        return it == m_TimingByName.end() ? std::pair<double, double>{0.0, 0.0} : it->second;
    };
    switch (m_SortMode)
    {
    case SortMode::Order:
        break;
    case SortMode::Name:
        std::stable_sort(passes.begin(), passes.end(),
                         [](const auto& a, const auto& b) { return a.name < b.name; });
        break;
    case SortMode::CpuDesc:
        std::stable_sort(passes.begin(), passes.end(),
                         [&](const auto& a, const auto& b) {
                             return timingOf(a.name).first > timingOf(b.name).first;
                         });
        break;
    case SortMode::GpuDesc:
        std::stable_sort(passes.begin(), passes.end(),
                         [&](const auto& a, const auto& b) {
                             return timingOf(a.name).second > timingOf(b.name).second;
                         });
        break;
    case SortMode::BarriersDesc:
        std::stable_sort(passes.begin(), passes.end(),
                         [](const auto& a, const auto& b) { return a.barrierCount > b.barrierCount; });
        break;
    }

    // Signature hash keyed on sorted name order + enabled flag + mode so we
    // rebuild the DOM only when the visible order actually changes.
    std::size_t sig = passes.size();
    sig = sig * 131ull + static_cast<std::size_t>(m_SortMode);
    for (const auto& p : passes)
    {
        sig = sig * 131ull + std::hash<std::string>{}(p.name);
        sig = sig * 131ull + (p.enabled ? 1u : 0u);
    }
    if (sig == m_LastPassSignature)
        return;
    m_LastPassSignature = sig;
    m_DetailDirty = true;

    m_PassListContainer->RemoveAllChildren();
    m_Rows.clear();
    m_Rows.reserve(passes.size());

    for (const auto& p : passes)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("render-graph-pass-row");
        if (p.name == m_SelectedPass)
            row->AddClass("selected");
        if (!p.enabled)
            row->AddClass("disabled");

        auto nameLbl = std::make_unique<Label>();
        nameLbl->AddClass("render-graph-pass-name");
        nameLbl->SetText(p.name);
        Label* namePtr = nameLbl.get();
        row->AddChild(std::move(nameLbl));

        // Meta row: queue (word) + barrier num (white) + "barrier[s]" (word)
        // + optional "cpu <num> ms" and "gpu <num> ms" groups.
        auto metaRow = std::make_unique<UIElement>();
        metaRow->AddClass("render-graph-pass-meta-row");

        auto makeWord = [&metaRow](const char* text) -> Label* {
            auto l = std::make_unique<Label>();
            l->AddClass("render-graph-pass-meta-word");
            l->SetText(text);
            Label* ptr = l.get();
            metaRow->AddChild(std::move(l));
            return ptr;
        };
        auto makeNum = [&metaRow](const char* text) -> Label* {
            auto l = std::make_unique<Label>();
            l->AddClass("render-graph-pass-meta-num");
            l->SetText(text);
            Label* ptr = l.get();
            metaRow->AddChild(std::move(l));
            return ptr;
        };

        Label* queuePtr = makeWord(QueueName(p.queueType));
        Label* barrierNumPtr = makeNum("0");
        Label* barrierWordPtr = makeWord("barriers");

        auto metaSpacer = std::make_unique<UIElement>();
        metaSpacer->AddClass("render-graph-pass-meta-spacer");
        metaRow->AddChild(std::move(metaSpacer));

        auto cpuGroup = std::make_unique<UIElement>();
        cpuGroup->AddClass("render-graph-pass-meta-group");
        cpuGroup->AddClass("cpu");
        cpuGroup->AddClass("hidden");
        Label* cpuNumPtr = nullptr;
        {
            auto cpuLabel = std::make_unique<Label>();
            cpuLabel->AddClass("render-graph-pass-meta-word");
            cpuLabel->AddClass("label");
            cpuLabel->SetText("cpu");
            cpuGroup->AddChild(std::move(cpuLabel));
            auto cpuNum = std::make_unique<Label>();
            cpuNum->AddClass("render-graph-pass-meta-num");
            cpuNum->SetText("0.00");
            cpuNumPtr = cpuNum.get();
            cpuGroup->AddChild(std::move(cpuNum));
            auto cpuMs = std::make_unique<Label>();
            cpuMs->AddClass("render-graph-pass-meta-word");
            cpuMs->AddClass("unit");
            cpuMs->SetText("ms");
            cpuGroup->AddChild(std::move(cpuMs));
        }
        UIElement* cpuGroupPtr = cpuGroup.get();
        metaRow->AddChild(std::move(cpuGroup));

        auto gpuGroup = std::make_unique<UIElement>();
        gpuGroup->AddClass("render-graph-pass-meta-group");
        gpuGroup->AddClass("gpu");
        gpuGroup->AddClass("hidden");
        Label* gpuNumPtr = nullptr;
        {
            auto gpuLabel = std::make_unique<Label>();
            gpuLabel->AddClass("render-graph-pass-meta-word");
            gpuLabel->AddClass("label");
            gpuLabel->SetText("gpu");
            gpuGroup->AddChild(std::move(gpuLabel));
            auto gpuNum = std::make_unique<Label>();
            gpuNum->AddClass("render-graph-pass-meta-num");
            gpuNum->SetText("0.00");
            gpuNumPtr = gpuNum.get();
            gpuGroup->AddChild(std::move(gpuNum));
            auto gpuMs = std::make_unique<Label>();
            gpuMs->AddClass("render-graph-pass-meta-word");
            gpuMs->AddClass("unit");
            gpuMs->SetText("ms");
            gpuGroup->AddChild(std::move(gpuMs));
        }
        UIElement* gpuGroupPtr = gpuGroup.get();
        metaRow->AddChild(std::move(gpuGroup));

        row->AddChild(std::move(metaRow));

        const std::string nameCopy = p.name;
        row->RegisterEventHandler(kEventMouseDown, [this, nameCopy](UIEvent& e) {
            if (e.Button != 0)
                return;
            SelectPass(nameCopy);
            e.Stop();
        });

        UIElement* rowPtr = row.get();
        m_PassListContainer->AddChild(std::move(row));
        PassRow rec;
        rec.Root = rowPtr;
        rec.NameLabel = namePtr;
        rec.QueueLabel = queuePtr;
        rec.BarrierNumLabel = barrierNumPtr;
        rec.BarrierWordLabel = barrierWordPtr;
        rec.CpuGroup = cpuGroupPtr;
        rec.CpuNumLabel = cpuNumPtr;
        rec.GpuGroup = gpuGroupPtr;
        rec.GpuNumLabel = gpuNumPtr;
        rec.Name = nameCopy;
        rec.QueueType = p.queueType;
        rec.BarrierCount = p.barrierCount;
        m_Rows.push_back(std::move(rec));
    }

    ApplySearchFilter();

    // If the current selection disappeared, default to first pass.
    if (!passes.empty())
    {
        bool found = false;
        for (const auto& p : passes)
        {
            if (p.name == m_SelectedPass)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            m_SelectedPass = passes.front().name;
            for (auto& r : m_Rows)
            {
                if (r.Name == m_SelectedPass && r.Root)
                    r.Root->AddClass("selected");
            }
        }
    }
}

void RenderGraphPanel::ApplySearchFilter()
{
    // Case-insensitive substring filter on pass names. Empty query shows all.
    std::string needle = m_SearchText;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (auto& r : m_Rows)
    {
        if (!r.Root)
            continue;
        bool visible = true;
        if (!needle.empty())
        {
            std::string hay = r.Name;
            std::transform(hay.begin(), hay.end(), hay.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            visible = hay.find(needle) != std::string::npos;
        }
        if (visible)
            r.Root->RemoveClass("hidden");
        else
            r.Root->AddClass("hidden");
    }
}

void RenderGraphPanel::ExportCsv()
{
    auto passes = QueryCompiledPasses(m_LastFrame);
    const std::unordered_map<std::string, std::pair<double, double>>& byName = m_TimingByName;

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmBuf);

    // Write to the user's Desktop so the file is easy to find, then reveal
    // it in the OS file manager. Falls back to the system temp dir if the
    // Desktop cannot be located.
    std::filesystem::path dir;
#if defined(_WIN32)
    if (const char* userProfile = std::getenv("USERPROFILE"))
        dir = std::filesystem::path(userProfile) / "Desktop";
#else
    if (const char* home = std::getenv("HOME"))
        dir = std::filesystem::path(home) / "Desktop";
#endif
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec))
        dir = std::filesystem::temp_directory_path(ec);
    if (ec || dir.empty())
    {
        Logger::Log::Error("Render graph CSV export: no writable directory");
        return;
    }
    const std::filesystem::path out = dir / (std::string("render_graph_") + stamp + ".csv");

    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        Logger::Log::Error("Render graph CSV export: failed to open {}", out.string());
        return;
    }

    f << "pass,queue,order,barriers,cpu_ms,gpu_ms,enabled\n";
    for (const auto& p : passes)
    {
        double cpu = 0.0;
        double gpu = 0.0;
        auto it = byName.find(p.name);
        if (it != byName.end())
        {
            cpu = it->second.first;
            gpu = it->second.second;
        }
        f << '"' << p.name << "\","
          << QueueName(p.queueType) << ','
          << p.insertionOrder << ','
          << p.barrierCount << ','
          << cpu << ','
          << gpu << ','
          << (p.enabled ? 1 : 0) << '\n';
    }
    f.close();

    Logger::Log::Info("Render graph CSV exported to {}", out.string());
    Platform::ShowInFileManager(out);
}

void RenderGraphPanel::SelectPass(const std::string& name)
{
    if (m_SelectedPass == name)
        return;
    m_SelectedPass = name;
    UIElement* selectedRoot = nullptr;
    for (auto& r : m_Rows)
    {
        if (!r.Root)
            continue;
        if (r.Name == name)
        {
            r.Root->AddClass("selected");
            selectedRoot = r.Root;
        }
        else
        {
            r.Root->RemoveClass("selected");
        }
    }
    m_DetailDirty = true;

    if (UIManager* ui = GetOwnerManager(); ui && m_PassListContainer)
        ui->FocusElement(m_PassListContainer);
}

void RenderGraphPanel::CycleSortMode()
{
    switch (m_SortMode)
    {
    case SortMode::Order:        m_SortMode = SortMode::Name; break;
    case SortMode::Name:         m_SortMode = SortMode::CpuDesc; break;
    case SortMode::CpuDesc:      m_SortMode = SortMode::GpuDesc; break;
    case SortMode::GpuDesc:      m_SortMode = SortMode::BarriersDesc; break;
    case SortMode::BarriersDesc: m_SortMode = SortMode::Order; break;
    }
    UpdateSortButtonText();
    // Force the next RebuildPassList tick to rebuild.
    m_LastPassSignature = 0;
}

void RenderGraphPanel::UpdateSortButtonText()
{
    if (!m_SortButton)
        return;
    const char* label = "Sort: Insertion";
    switch (m_SortMode)
    {
    case SortMode::Order:        label = "Sort: Insertion"; break;
    case SortMode::Name:         label = "Sort: Name"; break;
    case SortMode::CpuDesc:      label = "Sort: CPU ms"; break;
    case SortMode::GpuDesc:      label = "Sort: GPU ms"; break;
    case SortMode::BarriersDesc: label = "Sort: Barriers"; break;
    }
    m_SortButton->SetText(label);
}

void RenderGraphPanel::MoveSelection(int delta)
{
    if (m_Rows.empty() || delta == 0)
        return;

    // Find current index, ignoring hidden (filtered) rows. Up/down should
    // skip over rows that the search filter hid.
    auto isVisible = [](const PassRow& r) {
        return r.Root && !r.Root->HasClass("hidden");
    };

    int currentIdx = -1;
    for (std::size_t i = 0; i < m_Rows.size(); ++i)
    {
        if (m_Rows[i].Name == m_SelectedPass)
        {
            currentIdx = static_cast<int>(i);
            break;
        }
    }

    const int n = static_cast<int>(m_Rows.size());
    int idx = currentIdx;
    const int step = delta > 0 ? 1 : -1;
    const int maxSteps = n; // bound the walk
    for (int i = 0; i < maxSteps; ++i)
    {
        idx += step;
        if (idx < 0 || idx >= n)
            return;
        if (isVisible(m_Rows[idx]))
        {
            SelectPass(m_Rows[idx].Name);
            if (m_PassListScroll && m_Rows[idx].Root)
            {
                const float rowY = m_Rows[idx].Root->GetLayoutY();
                const float rowH = m_Rows[idx].Root->GetLayoutHeight();
                const float viewH = m_PassListScroll->GetViewportHeight();
                const float scrollY = m_PassListScroll->GetScrollY();
                if (rowY < scrollY || rowY + rowH > scrollY + viewH)
                    m_PassListScroll->SetScrollY(rowY + rowH * 0.5f - viewH * 0.5f);
            }
            return;
        }
    }
}

void RenderGraphPanel::RefreshDetail()
{
    if (!m_DetailContainer)
        return;

    m_DetailContainer->RemoveAllChildren();

    if (m_SelectedPass.empty())
    {
        auto empty = std::make_unique<Label>();
        empty->AddClass("render-graph-detail-empty");
        empty->SetText("Select a pass to inspect.");
        m_DetailContainer->AddChild(std::move(empty));
        return;
    }

    auto passes = QueryCompiledPasses(m_LastFrame);
    const PassInfo* found = nullptr;
    for (const auto& p : passes)
    {
        if (p.name == m_SelectedPass)
        {
            found = &p;
            break;
        }
    }
    if (!found)
    {
        auto miss = std::make_unique<Label>();
        miss->AddClass("render-graph-detail-empty");
        miss->SetText("Pass no longer in graph.");
        m_DetailContainer->AddChild(std::move(miss));
        return;
    }

    // Header
    auto header = std::make_unique<Label>();
    header->AddClass("render-graph-detail-header");
    header->SetText(found->name);
    m_DetailContainer->AddChild(std::move(header));

    {
        auto info = std::make_unique<Label>();
        info->AddClass("render-graph-detail-subheader");
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "queue: %s   order: %u   barriers: %u   deps: %zu   %s",
                      QueueName(found->queueType),
                      found->insertionOrder,
                      found->barrierCount,
                      found->dependsOn.size(),
                      found->enabled ? "enabled" : "DISABLED");
        info->SetText(buf);
        m_DetailContainer->AddChild(std::move(info));
    }

    // Accesses
    {
        auto sectionTitle = std::make_unique<Label>();
        sectionTitle->AddClass("render-graph-detail-section");
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Resource accesses (%zu)", found->accesses.size());
        sectionTitle->SetText(buf);
        m_DetailContainer->AddChild(std::move(sectionTitle));
    }
    for (const auto& a : found->accesses)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("render-graph-access-row");
        if (a.isWrite)
            row->AddClass("write");
        else
            row->AddClass("read");

        auto tag = std::make_unique<Label>();
        tag->AddClass("render-graph-access-tag");
        tag->SetText(a.isWrite ? "W" : "R");
        row->AddChild(std::move(tag));

        auto name = std::make_unique<Label>();
        name->AddClass("render-graph-access-name");
        name->SetText(a.resourceName);
        row->AddChild(std::move(name));

        auto state = std::make_unique<Label>();
        state->AddClass("render-graph-access-state");
        state->SetText(a.state);
        row->AddChild(std::move(state));

        m_DetailContainer->AddChild(std::move(row));
    }

    // Barriers
    auto barriers = QueryBarrierDetails(m_LastFrame, m_SelectedPass);
    {
        auto sectionTitle = std::make_unique<Label>();
        sectionTitle->AddClass("render-graph-detail-section");
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Barriers (%zu)", barriers.size());
        sectionTitle->SetText(buf);
        m_DetailContainer->AddChild(std::move(sectionTitle));
    }
    for (const auto& b : barriers)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("render-graph-barrier-row");

        auto name = std::make_unique<Label>();
        name->AddClass("render-graph-barrier-name");
        name->SetText(b.resourceName);
        row->AddChild(std::move(name));

        auto transition = std::make_unique<Label>();
        transition->AddClass("render-graph-barrier-transition");
        std::string text = b.stateBefore;
        text += "  ->  ";
        text += b.stateAfter;
        transition->SetText(text);
        row->AddChild(std::move(transition));

        m_DetailContainer->AddChild(std::move(row));
    }
}

void RenderGraphPanel::OnPostLayout()
{
    if (!m_StyleAttached && !m_StyleLoadScheduled && GetOwnerManager())
    {
        m_StyleLoadScheduled = true;
        PostAction([this]() { LoadAndAttachPanelStyle(); });
    }
}

void RenderGraphPanel::OnMountVisibilityChanged(bool isVisible)
{
    // Per-pass profiling inserts a GPU timestamp query around every render pass
    // and reads it back each frame, so it should only run while this panel is
    // visible. Drop the local toggle on hide so reopening starts from "off".
    if (!isVisible)
        m_ProfilingEnabled = false;
}

void RenderGraphPanel::LoadAndAttachPanelStyle()
{
    m_StyleLoadScheduled = false;
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;
    auto& am = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path styleAssetPath =
        std::filesystem::path("UI") / "panels" / "RenderGraphPanel.css";
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
