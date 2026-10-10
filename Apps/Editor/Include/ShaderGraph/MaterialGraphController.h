#pragma once

#include "InspectorRegistry.h"
#include "Graph/GraphCanvas.h"
#include "Rendering/ShaderGraph/SgTypes.h"
#include "ShaderGraph/MaterialGraphNode.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine {

class Button;
class MaterialGraphPreviewHost;
class UIElement;
struct EditorContext;
namespace Editor { class MaterialGraphPreviews; }

/**
 * Shader-graph compile, preview atlas, and toolbar state. Owned by
 * ShaderGraphPanel; not a GraphPanel strategy swap.
 */
class MaterialGraphController
{
  public:
    struct ToolbarItems
    {
        Button* Preview = nullptr;
        Button* PreviewIbl = nullptr;
        Button* AutoCompile = nullptr;
        Button* Compile = nullptr;
        UIElement* CompileWrap = nullptr;
        UIElement* CompileStatusDot = nullptr;
    };

    struct CompileContext
    {
        const Graph::Model* Model = nullptr;
        std::filesystem::path Path;
        const EditorContext* Context = nullptr;
        GraphCanvas* Canvas = nullptr;
        bool ExpandedNodes = false;
        bool GestureActive = false;
        std::function<void(std::function<void()>)> PostAction;
    };

    struct CompileOutcome
    {
        bool Success = false;
    };

    /** What the controller needs from the panel that outlives any one graph.
        The preview host is a UI child of the panel, so it is fetched rather
        than stored. The per-node atlas lives on this controller
        (`m_Previews`) and is rebuilt in OnContextChanged. */
    struct Host
    {
        std::function<MaterialGraphPreviewHost*()> PreviewHost;
        std::function<void()> RefreshPreview;
        std::function<void()> RequestCompileRefreshIfStale;
        std::function<std::filesystem::path()> CurrentPath;
        std::function<Graph::Model*()> Model;
        std::function<void()> RecompileAndReport;
        std::function<void(bool)> SaveAutoCompilePreference;
        /** A compile finished: render its verdict. Empty means clean. The
            controller has already mirrored it to the Shader Errors log and
            logged it, so this is pixels only. */
        std::function<void(const std::vector<ShaderGraph::SgDiagnostic>&)> ShowDiagnostics;
        /** The model in view is empty, so nothing compiled and no verdict
            stands. `onDiskErrorCount` is what the Shader Errors mirror still
            holds for the open file: non-zero means the file on disk is broken
            even though the view has nothing to blame. */
        std::function<void(std::size_t onDiskErrorCount)> ClearDiagnostics;
    };

    explicit MaterialGraphController(Host host);
    ~MaterialGraphController();

    /** The window ColorConstant's swatch opens. Material-side only: no other
        graph kind has a colour to pick, so no generic type carries it. */
    void SetOpenColorPicker(OpenColorPickerWindowFn fn) { m_OpenColorPicker = std::move(fn); }
    GraphCanvas::NodeFactoryFn MakeNodeFactory();
    void SyncToolbar(const ToolbarItems& items);
    void EnsureNodeTypesRegistered();
    bool NodeParamsAffectVariables(const Graph::Node& node) const;
    bool BuildSampleGraph(Graph::Model& model);
    std::filesystem::path SampleGraphAssetPath(
        const std::filesystem::path& assetsRoot) const;
    void OnContextChanged(const EditorContext* context);
    void OnDeactivated();
    void OnBeforeSave(Graph::Model& model);
    void OnAfterSave();
    std::filesystem::path DefaultSaveDirectory(
        const std::filesystem::path& assetsRoot) const;

    void OnModelShown(const Graph::Model& model);

    void TogglePreview(const ToolbarItems& items);
    void ToggleIbl(const ToolbarItems& items);
    void ToggleAutoCompile(const ToolbarItems& items);
    bool AutoCompileEnabled() const { return m_AutoCompileEnabled; }
    void SetAutoCompileEnabled(bool enabled) { m_AutoCompileEnabled = enabled; }
    bool CompileUpToDate() const { return m_CompileUpToDate; }
    void SetCompileUpToDate(bool upToDate) { m_CompileUpToDate = upToDate; }
    bool PreviewEnabled() const { return m_PreviewEnabled; }
    bool IblEnabled() const { return m_IblEnabled; }
    bool WantsCompileRefresh(bool previewHostPresent) const
    {
        return m_AutoCompileEnabled || (m_PreviewEnabled && previewHostPresent);
    }
    void SetIblEnabled(bool enabled) { m_IblEnabled = enabled; }

    void RefreshBoundPreviewProperties(const Graph::Model& model);
    CompileOutcome Compile(const CompileContext& ctx);
    void SyncNodePreviews(const EditorContext* context, const Graph::Model& model,
                          bool expandedNodes, GraphCanvas* canvas);
    void PumpDeferredPreviewRebuild(const EditorContext* context, const Graph::Model& model,
                                    bool expandedNodes, GraphCanvas* canvas, bool gestureActive);
    void ShutdownPreviews();
    /** Hands the graph's preview panel its atlas cell and pushes the angle it has
        been spun to. Called every frame the panel updates. */
    void SyncMainPreview(MaterialGraphPreviewHost* host);
    void SyncGraphVariables(Graph::Model& model);
    void SyncPublicVariablesToMaterials(const Graph::Model& model,
                                        const std::filesystem::path& path, bool syncDisk);

  private:
    OpenColorPickerWindowFn m_OpenColorPicker;
    /** Mirror to the Shader Errors log (keyed on the graph file), log once on
        change, then hand the structured list to the panel to render. */
    void ReportCompileResult(const std::vector<ShaderGraph::SgDiagnostic>& errors,
                             const std::filesystem::path& path);
    void MirrorToShaderErrors(const std::vector<ShaderGraph::SgDiagnostic>& errors,
                              const std::filesystem::path& path) const;
    /** Rows the Shader Errors log still holds under this graph file's key. */
    std::size_t MirroredErrorCount(const std::filesystem::path& path) const;
    /** An empty model has nothing to compile, so no verdict stands: the
        compile pip reads up to date (nothing is stale), the preview hides, the
        panel clears its view. The mirror keeps what the file's last compile
        left — the file on disk did not change — and the count lets the panel
        say so. */
    void ClearForEmptyModel(const std::filesystem::path& path);

    Host m_Host;
    std::unique_ptr<Editor::MaterialGraphPreviews> m_Previews;
    /** MaterialGraphSurfaceDigest of the model the last full compile ran on. An
        edit that leaves it still cannot change any compiled shader, so it takes
        the values-only path instead. */
    std::uint64_t m_SurfaceDigest = 0;
    bool m_HasSurfaceDigest = false;
    bool m_PreviewEnabled = true;
    /** Off by default: the environment lighting washes a preview sphere out, and
        a plate is there to show what the material IS. The toolbar toggle turns it
        on for a lookdev pass. Drives both the atlas cells and the panel sphere,
        so the two never disagree. */
    bool m_IblEnabled = false;
    bool m_AutoCompileEnabled = true;
    bool m_CompileUpToDate = false;
    std::string m_LastLoggedErrors;
};

} // namespace GameEngine
