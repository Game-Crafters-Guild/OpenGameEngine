#include "Panels/ShaderGraphPanel.h"

#include "Editor/Settings/SettingsStore.h"
#include "EditorContext.h"
#include "Graph/GraphModel.h"
#include "ShaderGraph/MaterialGraphDiagnosticsSummary.h"
#include "ShaderGraph/MaterialGraphPreviewHost.h"
#include "ShaderGraph/MaterialGraphVariables.h"
#include "UI/ActivateOnRelease.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Scheduler/Scheduler.h"

#include "Logger/Logger.h"

#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>

namespace GameEngine {
namespace {

constexpr const char* kNodeGraphAutoCompilePreference = "nodeGraph.autoCompile";

bool LoadAutoCompilePreference()
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    bool enabled = true;
    (void)prefs.TryGetBool(kNodeGraphAutoCompilePreference, enabled);
    return enabled;
}

void SaveAutoCompilePreference(bool enabled)
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    prefs.SetBool(kNodeGraphAutoCompilePreference, enabled);
    (void)prefs.Save(&err);
}

} // namespace

ShaderGraphPanel::ShaderGraphPanel()
    : GraphPanel(Graph::kKindIdMaterial)
{
    /* Before SetupUI: the panel calls the chrome slots while it builds, so the
       registration has to be in place first. */
    Init();
    BindShaderController();
    SetupUI();
    SetTitle("Shader Graph");
}

void ShaderGraphPanel::Init()
{
    GraphKindHooks hooks;
    hooks.ContributeChrome = [this](GraphPanelRegion region, UIElement& host)
    { ContributeChrome(region, host); };
    hooks.ToolbarEntries = [this] { return ToolbarEntries(); };
    hooks.OnColorPickerChanged = [this] { OnColorPickerChanged(); };
    hooks.NodeCatalog = [this] { return NodeCatalog(); };
    hooks.Sample.Label = "Material Test Graph";
    hooks.Sample.SearchKey = "PreviewSphereTest";
    hooks.Sample.AssetPath = [this](const std::filesystem::path& root)
    { return SampleGraphAssetPath(root); };
    hooks.Sample.Build = [this](Graph::Model& m) { return BuildSampleGraph(m); };
    hooks.OnBeforeSave = [this](Graph::Model& m) { OnBeforeSave(m); };
    hooks.DefaultSaveDirectory = [this](const std::filesystem::path& root)
    { return DefaultSaveDirectory(root); };
    hooks.OnUpdate = [this](float dt) { OnUpdate(dt); };
    hooks.SyncGraphVariables = [this] { SyncGraphVariables(); };
    hooks.OnPublicVariablesEdited = [this](bool syncDisk) { OnPublicVariableChanged(syncDisk); };
    hooks.OnNodeParamsEdited = [this](const Graph::Node& n) { return OnNodeParamsEdited(n); };
    hooks.CollectImpliedVariables =
        [this](std::vector<Graph::Variable>& variables, std::unordered_set<std::string>& names)
    { CollectImpliedVariables(variables, names); };
    // A shader graph's variables are its material properties; the global toggle
    // has no meaning for them.
    hooks.ShowsGlobalVariableToggle = false;
    SetKindHooks(std::move(hooks));

    Listen(GraphPanelEvent::ModelChanged, [this]() { OnModelChanged(); });
    Listen(GraphPanelEvent::Shown, [this]() { OnModelShown(); });
    Listen(GraphPanelEvent::Saved, [this]() { OnSaved(); });
    Listen(GraphPanelEvent::ContextChanged, [this]() { OnContextChanged(); });
    Listen(GraphPanelEvent::PostLayout, [this]() { OnPanelPostLayout(); });
    Listen(GraphPanelEvent::ExpandedNodesToggled, [this]() { SyncNodePreviews(); });
    Listen(GraphPanelEvent::VariablesEdited, [this]() { RefreshBoundPreviewProperties(); });
}

void ShaderGraphPanel::ContributeChrome(GraphPanelRegion region, UIElement& host)
{
    switch (region)
    {
    case GraphPanelRegion::StatusLabel:
        ContributeStatusLabel(host);
        break;
    case GraphPanelRegion::BelowToolbar:
        ContributeDiagnosticsStrip(host);
        break;
    case GraphPanelRegion::Body:
        ContributeBodyChrome(host);
        break;
    case GraphPanelRegion::CanvasOverlays:
        break;
    }
}

ShaderGraphPanel::~ShaderGraphPanel()
{
    Shutdown();
}

void ShaderGraphPanel::BindShaderController()
{
    MaterialGraphController::Host host;
    host.PreviewHost = [this]() { return m_PreviewHost; };
    host.RefreshPreview = [this]() { Compile(); };
    host.RequestCompileRefreshIfStale = [this]() { OnModelChanged(); };
    host.CurrentPath = [this]() { return GetCurrentGraphPath(); };
    host.Model = [this]() { return &RootModel(); };
    host.RecompileAndReport = [this]() { Compile(); };
    host.SaveAutoCompilePreference = [](bool enabled) { SaveAutoCompilePreference(enabled); };
    host.ShowDiagnostics = [this](const std::vector<ShaderGraph::SgDiagnostic>& errors)
    { ShowCompileDiagnostics(errors); };
    host.ClearDiagnostics = [this](std::size_t onDiskErrorCount)
    { ClearCompileDiagnostics(onDiskErrorCount); };
    m_Shader = std::make_unique<MaterialGraphController>(std::move(host));
    m_Shader->SetOpenColorPicker(OpenColorPickerWindow());
    m_Shader->SetAutoCompileEnabled(LoadAutoCompilePreference());
    m_Shader->OnContextChanged(Context());
}

MaterialGraphController::ToolbarItems ShaderGraphPanel::ShaderToolbar()
{
    auto button = [this](const char* id) { return dynamic_cast<Button*>(FindById(id)); };
    MaterialGraphController::ToolbarItems items;
    items.Preview = button("NodeGraphMaterialPreviewToggle");
    items.PreviewIbl = button("NodeGraphMaterialPreviewIblToggle");
    items.AutoCompile = button("NodeGraphAutoCompileToggle");
    items.Compile = button("NodeGraphCompileButton");
    items.CompileWrap = FindById("NodeGraphCompileWrap");
    items.CompileStatusDot = FindById("NodeGraphCompileStatusDot");
    return items;
}

void ShaderGraphPanel::ContributeStatusLabel(UIElement& toolbar)
{
    auto compileStatus = std::make_unique<Label>();
    compileStatus->SetId("NodeGraphCompileStatus");
    compileStatus->AddClass("inspector-text");
    compileStatus->SetText("");
    m_CompileStatusLabel = compileStatus.get();
    toolbar.AddChild(std::move(compileStatus));
}

void ShaderGraphPanel::ContributeDiagnosticsStrip(UIElement& container)
{
    auto diagnostics = std::make_unique<UIElement>();
    diagnostics->SetId("NodeGraphDiagnostics");
    diagnostics->AddClass("node-graph-diagnostics");
    diagnostics->Overrides().Set(Style::Display, DisplayMode::None);

    // Plain elements, not Buttons: Button.css sizes `button, .button` at a 32px
    // min-height that a single class only ties with, and the fix for that tie
    // is a shorter row, not a deeper selector.
    auto summary = std::make_unique<UIElement>();
    summary->AddClass("node-graph-diagnostic-summary");
    auto chevron = std::make_unique<UIElement>();
    chevron->AddClass("node-graph-diagnostic-chevron");
    m_DiagnosticsChevron = chevron.get();
    summary->AddChild(std::move(chevron));
    auto summaryText = std::make_unique<Label>();
    summaryText->AddClass("node-graph-diagnostic-summary-text");
    m_DiagnosticsSummaryText = summaryText.get();
    summary->AddChild(std::move(summaryText));
    EditorUI::ActivateOnRelease(*summary, [this]() { ToggleDiagnosticsExpanded(); });
    m_DiagnosticsSummary = summary.get();
    diagnostics->AddChild(std::move(summary));

    // The expanded/collapsed class lives on this wrapper because the wrapper is
    // also what carries the list's max-height and `flex-shrink: 0` — and
    // ScrollView sets its own flex-shrink as an inline override, which no
    // stylesheet rule can beat.
    auto list = std::make_unique<UIElement>();
    list->AddClass("node-graph-diagnostic-list");
    auto listScroll = std::make_unique<ScrollView>();
    listScroll->AddClass("node-graph-diagnostic-scroll");
    auto listContent = std::make_unique<UIElement>();
    listContent->AddClass("node-graph-diagnostic-list-content");
    m_DiagnosticsListContent = listContent.get();
    listScroll->AddContent(std::move(listContent));
    list->AddChild(std::move(listScroll));
    m_DiagnosticsList = list.get();
    diagnostics->AddChild(std::move(list));

    m_DiagnosticsStrip = diagnostics.get();
    container.AddChild(std::move(diagnostics));
}

std::vector<GraphToolbarEntry> ShaderGraphPanel::ToolbarEntries()
{
    std::vector<GraphToolbarEntry> entries;

    entries.push_back({GraphToolbarEntry::Place::Icon,
                       "NodeGraphMaterialPreviewToggle",
                       {"eye-icon", "node-graph-toolbar-preview-button",
                        "node-graph-toolbar-preview-toggle"},
                       "",
                       "Toggle material preview",
                       [this]() { TogglePreview(); },
                       {}});

    entries.push_back({GraphToolbarEntry::Place::Icon,
                       "NodeGraphMaterialPreviewIblToggle",
                       {"sky-environment-icon", "node-graph-toolbar-preview-button",
                        "node-graph-toolbar-preview-ibl-toggle"},
                       "",
                       "Toggle IBL for material preview",
                       [this]() { ToggleIbl(); },
                       {}});

    entries.push_back({GraphToolbarEntry::Place::Action,
                       "NodeGraphAutoCompileToggle",
                       {"node-graph-auto-compile-toggle"},
                       "Auto",
                       "Automatically compile the shader graph when it changes",
                       [this]() { ToggleAutoCompile(); },
                       {}});

    /* The compile button wears its status dot, so the kind builds this one. */
    entries.push_back(
        {GraphToolbarEntry::Place::Action, "NodeGraphCompileWrap", {}, "", "", {}, [this]()
         {
             auto wrap = std::make_unique<UIElement>();
             wrap->SetId("NodeGraphCompileWrap");
             wrap->AddClass("node-graph-compile-wrap");

             auto compile = std::make_unique<Button>();
             compile->SetId("NodeGraphCompileButton");
             compile->AddClass("small");
             compile->AddClass("secondary");
             compile->AddClass("node-graph-compile-button");
             compile->AddClass("needs-compile");
             compile->SetText("Compile");
             compile->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Compile(); });
             wrap->AddChild(std::move(compile));

             auto dot = std::make_unique<UIElement>();
             dot->SetId("NodeGraphCompileStatusDot");
             dot->AddClass("node-graph-compile-status-dot");
             dot->AddClass("stale");
             wrap->AddChild(std::move(dot));
             return wrap;
         }});

    return entries;
}

void ShaderGraphPanel::ContributeBodyChrome(UIElement& body)
{
    auto preview = std::make_unique<MaterialGraphPreviewHost>();
    preview->SetId("NodeGraphMaterialPreview");
    m_PreviewHost = preview.get();
    preview->SetIblEnabled(true);
    preview->SetVisibleForMaterialGraph(false);
    body.AddChild(std::move(preview));
}

void ShaderGraphPanel::OnColorPickerChanged()
{
    if (m_Shader)
        m_Shader->SetOpenColorPicker(OpenColorPickerWindow());
}

GraphCanvas::NodeFactoryFn ShaderGraphPanel::NodeCatalog()
{
    if (!m_Shader)
        return {};
    m_Shader->EnsureNodeTypesRegistered();
    return m_Shader->MakeNodeFactory();
}

bool ShaderGraphPanel::OnNodeParamsEdited(const Graph::Node& node)
{
    if (!m_Shader || !m_Shader->NodeParamsAffectVariables(node))
        return false;
    SyncGraphVariables();
    return true;
}

bool ShaderGraphPanel::BuildSampleGraph(Graph::Model& model)
{
    return m_Shader && m_Shader->BuildSampleGraph(model);
}

std::filesystem::path ShaderGraphPanel::SampleGraphAssetPath(
    const std::filesystem::path& assetsRoot) const
{
    return m_Shader ? m_Shader->SampleGraphAssetPath(assetsRoot) : std::filesystem::path{};
}

void ShaderGraphPanel::OnContextChanged()
{
    if (m_PreviewHost)
        m_PreviewHost->SetContext(Context());
    if (m_Shader)
        m_Shader->OnContextChanged(Context());
    OnModelChanged();
}

void ShaderGraphPanel::OnBeforeSave(Graph::Model& model)
{
    if (m_Shader)
        m_Shader->OnBeforeSave(model);
}

void ShaderGraphPanel::OnSaved()
{
    if (m_Shader)
        m_Shader->OnAfterSave();
}

std::filesystem::path ShaderGraphPanel::DefaultSaveDirectory(
    const std::filesystem::path& assetsRoot) const
{
    return m_Shader ? m_Shader->DefaultSaveDirectory(assetsRoot) : assetsRoot;
}

void ShaderGraphPanel::OnModelShown()
{
    if (m_Shader)
        m_Shader->OnModelShown(RootModel());
    if (m_Shader)
        m_Shader->SyncToolbar(ShaderToolbar());
}

void ShaderGraphPanel::OnUpdate(float dt)
{
    if (m_PreviewHost)
        m_PreviewHost->Update(dt);
    if (m_Shader)
        m_Shader->SyncMainPreview(m_PreviewHost);
    UIManager* ui = Canvas() ? Canvas()->GetOwnerManager() : nullptr;
    const bool gestureActive = (ui && ui->IsMouseCaptured()) || NodeDragInProgress();
    if (m_Shader)
        m_Shader->PumpDeferredPreviewRebuild(Context(), RootModel(), ExpandedNodesEnabled(), Canvas(),
                                             gestureActive);
}

void ShaderGraphPanel::OnModelChanged()
{
    if (m_Shader)
        m_Shader->SetCompileUpToDate(false);
    if (m_Shader)
        m_Shader->SyncToolbar(ShaderToolbar());
    if (!m_Shader || !m_Shader->WantsCompileRefresh(m_PreviewHost != nullptr))
        return;
    /* A node drag moves rects; it cannot change what the graph compiles to or
       what any preview shows. Dropping the refresh rather than deferring it to
       drag-end is what keeps the previews from regenerating on every move. */
    if (NodeDragInProgress())
        return;
    if (m_CompileRefreshScheduled)
        return;
    m_CompileRefreshScheduled = true;
    Scheduler::IScheduler* sched = GetScheduler();
    if (!sched)
    {
        m_CompileRefreshScheduled = false;
        Compile();
        return;
    }
    sched->ScheduleNext([this]() {
        m_CompileRefreshScheduled = false;
        if (NodeDragInProgress())
            return;
        Compile();
    });
}

void ShaderGraphPanel::Shutdown()
{
    if (m_PreviewHost)
        m_PreviewHost->ClearPreview();
    if (m_Shader)
    {
        m_Shader->OnDeactivated();
        m_Shader->ShutdownPreviews();
    }
}

void ShaderGraphPanel::OnPanelPostLayout()
{
    if (m_PreviewHost)
        m_PreviewHost->OnParentLayoutReady();
}

void ShaderGraphPanel::Compile()
{
    if (!m_Shader)
        return;
    MaterialGraphController::CompileContext ctx;
    ctx.Model = &RootModel();
    ctx.Path = GetCurrentGraphPath();
    ctx.Context = Context();
    ctx.Canvas = Canvas();
    ctx.ExpandedNodes = ExpandedNodesEnabled();
    UIManager* ui = Canvas() ? Canvas()->GetOwnerManager() : nullptr;
    ctx.GestureActive = (ui && ui->IsMouseCaptured()) || NodeDragInProgress();
    ctx.PostAction = [this](std::function<void()> action) { PostAction(std::move(action)); };
    m_Shader->Compile(ctx);
    m_Shader->SyncToolbar(ShaderToolbar());
}

void ShaderGraphPanel::ShowCompileDiagnostics(const std::vector<ShaderGraph::SgDiagnostic>& errors)
{
    const MaterialGraphDiagnosticsSummary summary = SummarizeMaterialGraphDiagnostics(errors);
    if (m_CompileStatusLabel)
    {
        m_CompileStatusLabel->SetText(summary.LabelText);
        m_CompileStatusLabel->SetTooltip(summary.Tooltip);
        if (errors.empty())
            m_CompileStatusLabel->RemoveClass("error-text");
        else
            m_CompileStatusLabel->AddClass("error-text");
    }
    RebuildDiagnosticsStrip(summary, errors);
    if (GraphCanvas* canvas = Canvas())
        canvas->SetErrorNodes(summary.ErrorNodeIds);
}

/* Panel-local. Clearing the view says nothing about the file on disk, so the
   Shader Errors mirror keeps its entry until a clean compile of that file
   resolves it. While it still holds rows for the open file, a blank strip would
   misreport a broken file as fine, so the strip states the fact and points at
   the Shader Errors panel instead. */
void ShaderGraphPanel::ClearCompileDiagnostics(std::size_t onDiskErrorCount)
{
    if (m_CompileStatusLabel)
    {
        m_CompileStatusLabel->SetText("");
        m_CompileStatusLabel->SetTooltip({});
        m_CompileStatusLabel->RemoveClass("error-text");
    }
    RebuildDiagnosticsStrip(SummarizeMaterialGraphDiagnostics({}), {});
    /* The hint is the summary line's message: this panel holds no rows to open,
       so the strip stays one line and points at the panel that has them. */
    if (onDiskErrorCount > 0 && m_DiagnosticsStrip && m_DiagnosticsSummaryText)
    {
        const std::string hint = OnDiskFailureHintText(onDiskErrorCount);
        m_DiagnosticsSummaryText->SetText(hint);
        m_DiagnosticsSummaryText->SetTooltip(hint);
        m_DiagnosticsExpandable = false;
        m_DiagnosticsStrip->Overrides().Set(Style::Display, DisplayMode::Flex);
        ApplyDiagnosticsExpandedState();
    }
    if (GraphCanvas* canvas = Canvas())
        canvas->SetErrorNodes({});
}

/* The strip costs the canvas one text row while folded and lists every failure
   when opened; the list is bounded and scrolls, so a graph that fails in twenty
   places cannot push the canvas off-panel. */
void ShaderGraphPanel::RebuildDiagnosticsStrip(const MaterialGraphDiagnosticsSummary& summary,
                                               const std::vector<ShaderGraph::SgDiagnostic>& errors)
{
    if (!m_DiagnosticsStrip || !m_DiagnosticsSummaryText || !m_DiagnosticsListContent)
        return;

    m_DiagnosticsListContent->RemoveAllChildren();
    m_DiagnosticsStrip->Overrides().Set(Style::Display,
                                        errors.empty() ? DisplayMode::None : DisplayMode::Flex);
    m_DiagnosticsSummaryText->SetText(summary.CollapsedLineText);
    /* The folded line ellipsises at the strip's width, so the tooltip is where
       a long first message stays readable without opening the list. Authored
       here rather than left to the truncated-label fallback: the fallback would
       repeat the whole line, count included, and the count is never the part
       that gets cut. */
    m_DiagnosticsSummaryText->SetTooltip(errors.empty() ? std::string() : errors.front().Message);
    m_DiagnosticsExpandable = !errors.empty();

    for (const ShaderGraph::SgDiagnostic& diagnostic : errors)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("node-graph-diagnostic-row");
        row->SetTooltip(diagnostic.Message);
        auto rowText = std::make_unique<Label>();
        rowText->AddClass("node-graph-diagnostic-row-text");
        rowText->SetText(diagnostic.Message);
        row->AddChild(std::move(rowText));
        if (diagnostic.NodeId.empty())
        {
            // Nothing to select: a graph-scoped failure names no node.
            row->AddClass("graph-scoped");
        }
        else
        {
            const std::string nodeId = diagnostic.NodeId;
            EditorUI::ActivateOnRelease(*row, [this, nodeId]()
            {
                GraphCanvas* canvas = Canvas();
                if (!canvas)
                    return;
                canvas->SetSelectedNodeId(nodeId);
                canvas->MarkDirty(VisualDirty);
            });
        }
        m_DiagnosticsListContent->AddChild(std::move(row));
    }

    ApplyDiagnosticsExpandedState();
}

void ShaderGraphPanel::ToggleDiagnosticsExpanded()
{
    if (!m_DiagnosticsExpandable)
        return;
    m_DiagnosticsExpanded = !m_DiagnosticsExpanded;
    ApplyDiagnosticsExpandedState();
}

/* Every visual difference between the two states is a class the stylesheet
   reads: the list's display, the chevron's icon, and whether the summary line
   reads as clickable at all. */
void ShaderGraphPanel::ApplyDiagnosticsExpandedState()
{
    if (!m_DiagnosticsStrip)
        return;

    const auto setClass = [](UIElement* element, const char* name, bool on)
    {
        if (!element)
            return;
        if (on)
            element->AddClass(name);
        else
            element->RemoveClass(name);
    };

    /* One term for one state, as Foldout does: `expanded` on every part that
       changes with it, so no part of the strip defaults to the opposite word. */
    const bool expanded = m_DiagnosticsExpanded && m_DiagnosticsExpandable;
    setClass(m_DiagnosticsList, "expanded", expanded);
    setClass(m_DiagnosticsChevron, "expanded", expanded);
    /* The on-disk-failure hint has no rows behind it: no chevron, the line does
       not read as clickable, and its text is muted rather than error-red —
       nothing here failed to compile, the file on disk did. */
    setClass(m_DiagnosticsChevron, "no-toggle", !m_DiagnosticsExpandable);
    setClass(m_DiagnosticsSummary, "no-toggle", !m_DiagnosticsExpandable);
    setClass(m_DiagnosticsSummaryText, "no-toggle", !m_DiagnosticsExpandable);

    m_DiagnosticsStrip->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ShaderGraphPanel::SyncGraphVariables()
{
    if (m_Shader)
        m_Shader->SyncGraphVariables(RootModel());
}

void ShaderGraphPanel::OnPublicVariableChanged(bool syncDisk)
{
    if (m_Shader)
        m_Shader->SyncPublicVariablesToMaterials(RootModel(), GetCurrentGraphPath(), syncDisk);
}

void ShaderGraphPanel::RefreshBoundPreviewProperties()
{
    if (m_Shader)
        m_Shader->RefreshBoundPreviewProperties(RootModel());
}

void ShaderGraphPanel::SyncNodePreviews()
{
    if (m_Shader)
        m_Shader->SyncNodePreviews(Context(), RootModel(), ExpandedNodesEnabled(), Canvas());
}

void ShaderGraphPanel::CollectImpliedVariables(std::vector<Graph::Variable>& variables,
                                                   std::unordered_set<std::string>& names) const
{
    for (const Graph::Node& node : RootModel().Nodes)
    {
        if (!MaterialGraphVariables::IsVariableNode(node))
            continue;
        auto it = node.Parameters.find("variableName");
        if (it == node.Parameters.end())
            continue;
        const std::string name = it->second.ToString();
        if (name.empty() || names.count(name))
            continue;
        Graph::Variable variable;
        variable.Name = name;
        variable.Type = MaterialGraphVariables::TypeFromNode(node);
        variable.Value = "0";
        variable.CreatedOrder = std::numeric_limits<std::uint64_t>::max() - variables.size();
        variables.push_back(std::move(variable));
        names.insert(name);
    }
}

void ShaderGraphPanel::TogglePreview()
{
    if (!m_Shader)
        return;
    m_Shader->TogglePreview(ShaderToolbar());
}

void ShaderGraphPanel::ToggleIbl()
{
    if (m_Shader)
        m_Shader->ToggleIbl(ShaderToolbar());
}

void ShaderGraphPanel::ToggleAutoCompile()
{
    if (m_Shader)
        m_Shader->ToggleAutoCompile(ShaderToolbar());
}

} // namespace GameEngine
