#pragma once

#include "Panels/GraphPanel.h"
#include "Rendering/ShaderGraph/SgTypes.h"
#include "ShaderGraph/MaterialGraphController.h"

#include <cstddef>
#include <vector>

namespace GameEngine {

class Button;
class Label;
class MaterialGraphPreviewHost;
class UIElement;
struct MaterialGraphDiagnosticsSummary;

class ShaderGraphPanel final : public GraphPanel
{
public:
    ShaderGraphPanel();
    ~ShaderGraphPanel() override;

private:
    /** States what a shader graph contributes, and what it reacts to. Called
        once, from the constructor. */
    void Init();

    void ContributeChrome(GraphPanelRegion region, UIElement& host);
    std::vector<GraphToolbarEntry> ToolbarEntries();
    void OnColorPickerChanged();
    void ContributeStatusLabel(UIElement& toolbar);
    void ContributeDiagnosticsStrip(UIElement& container);
    void ContributeBodyChrome(UIElement& body);
    GraphCanvas::NodeFactoryFn NodeCatalog();
    bool OnNodeParamsEdited(const Graph::Node& node);
    bool BuildSampleGraph(Graph::Model& model);
    std::filesystem::path SampleGraphAssetPath(const std::filesystem::path& assetsRoot) const;
    void OnBeforeSave(Graph::Model& model);
    std::filesystem::path DefaultSaveDirectory(const std::filesystem::path& assetsRoot) const;
    void OnUpdate(float dt);
    void Shutdown();
    void OnPanelPostLayout();
    void SyncGraphVariables();
    void OnPublicVariableChanged(bool syncDisk);
    void RefreshBoundPreviewProperties();
    void SyncNodePreviews();
    void CollectImpliedVariables(std::vector<Graph::Variable>& variables,
                                 std::unordered_set<std::string>& names) const;

    void BindShaderController();
    void OnModelChanged();
    void OnModelShown();
    void OnSaved();
    void OnContextChanged();
    MaterialGraphController::ToolbarItems ShaderToolbar();
    void Compile();
    void TogglePreview();
    void ToggleIbl();
    void ToggleAutoCompile();

    /** Compile diagnostics, in the three places an author looks: the toolbar
        status label, a strip under the toolbar summarising the failures, and
        the error class on the nodes those failures name. The controller
        compiles and reports; these only render. The strip is folded to one
        line by default and lists every failure when opened, so a broken graph
        costs the canvas one text row, not four. */
    void ShowCompileDiagnostics(const std::vector<ShaderGraph::SgDiagnostic>& errors);
    void ClearCompileDiagnostics(std::size_t onDiskErrorCount);
    void RebuildDiagnosticsStrip(const MaterialGraphDiagnosticsSummary& summary,
                                 const std::vector<ShaderGraph::SgDiagnostic>& errors);
    /** Folds the strip to its summary line, or opens the full list under it. */
    void ToggleDiagnosticsExpanded();
    void ApplyDiagnosticsExpandedState();

    std::unique_ptr<MaterialGraphController> m_Shader;
    MaterialGraphPreviewHost* m_PreviewHost = nullptr;
    Label* m_CompileStatusLabel = nullptr;
    // Strip chrome, built once with the panel and never replaced: only the
    // summary text and the list's rows change per compile.
    UIElement* m_DiagnosticsStrip = nullptr;
    UIElement* m_DiagnosticsSummary = nullptr;
    UIElement* m_DiagnosticsChevron = nullptr;
    Label* m_DiagnosticsSummaryText = nullptr;
    // The wrapper, not the ScrollView inside it: ScrollView's constructor sets
    // Display as an inline override, which the cascade cannot beat, so the
    // expanded/collapsed class has to sit on a plain element.
    UIElement* m_DiagnosticsList = nullptr;
    UIElement* m_DiagnosticsListContent = nullptr;
    // Per panel instance for the session: an author who opens the list keeps it
    // open across recompiles, and a fresh panel starts folded.
    bool m_DiagnosticsExpanded = false;
    // False for the on-disk-failure hint, which has no rows to open.
    bool m_DiagnosticsExpandable = false;
    bool m_CompileRefreshScheduled = false;
};

} // namespace GameEngine
