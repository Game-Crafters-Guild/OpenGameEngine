#include "ShaderGraph/MaterialGraphController.h"

#include "Graph/GraphCanvas.h"
#include "Assets/MaterialAsset.h"
#include "Core/Engine.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShaderCompileErrorLog.h"
#include "Graph/GraphNodeRegistry.h"
#include "Graph/MaterialGraphCompiler.h"
#include "Graph/SgGraphModelBridge.h"
#include "Logger/Logger.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "ShaderGraph/MaterialGraphPreviewHost.h"
#include "ShaderGraph/MaterialGraphVariables.h"
#include "ShaderGraph/GraphNodePreviewBinding.h"
#include "ShaderGraph/MaterialGraphPreviewModel.h"
#include "ShaderGraph/MaterialGraphPreviews.h"
#include "UI/Controls/Button.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <utility>

namespace GameEngine {
namespace {

/* What the compile controls say when this build cannot compile shader source. Such a
   runtime serves pre-cooked programs only, so a graph edit has nothing to build. */
constexpr const char* kNoRuntimeShaderCompilerNotice =
    "This build has no runtime shader compiler; graph preview needs a cooked variant";

void ApplyToggleState(Button* button, bool active)
{
    if (!button)
        return;
    button->Overrides().Set(Style::Display, DisplayMode::Flex);
    if (active)
        button->AddClass("icon-active");
    else
        button->RemoveClass("icon-active");
    button->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

/* The compiled/stale pip beside the Compile button. Sized and rounded here
   rather than in the stylesheet because the dot has no other identity: it is a
   bare element whose whole appearance is this state. */
constexpr float kCompileStatusDotSizePx = 8.0f;
constexpr uint32_t kCompileStatusDotUpToDateColor = 0xFF4CAF50u;
constexpr uint32_t kCompileStatusDotStaleColor = 0xFF8A8A8Au;

void ApplyCompileStatusDotStyle(UIElement& dot, bool upToDate)
{
    const float radius = kCompileStatusDotSizePx * 0.5f;
    const CornerRadiiTLTRBRBL cornerRadius{radius, radius, radius, radius};
    dot.Overrides()
        .Set(Style::Width, StyleLength::Px(kCompileStatusDotSizePx))
        .Set(Style::Height, StyleLength::Px(kCompileStatusDotSizePx))
        .Set(Style::MinWidth, StyleLength::Px(kCompileStatusDotSizePx))
        .Set(Style::MinHeight, StyleLength::Px(kCompileStatusDotSizePx))
        .Set(Style::FlexShrink, 0.f)
        .Set(Style::BorderRadius, cornerRadius)
        .Set(Style::BackgroundColor,
             upToDate ? kCompileStatusDotUpToDateColor : kCompileStatusDotStaleColor);
}

} // namespace

MaterialGraphController::MaterialGraphController(Host host) : m_Host(std::move(host)) {}

MaterialGraphController::~MaterialGraphController() = default;

void MaterialGraphController::EnsureNodeTypesRegistered()
{
    // The shader graph catalogue is built from the shared node library, which
    // is loaded on demand rather than at startup.
    GraphNodeRegistry::Get().EnsureMaterialShaderGraphTypesRegistered();
}

bool MaterialGraphController::NodeParamsAffectVariables(const Graph::Node& node) const
{
    return MaterialGraphVariables::IsVariableNode(node);
}

bool MaterialGraphController::BuildSampleGraph(Graph::Model& model)
{
    GraphNodeRegistry& registry = GraphNodeRegistry::Get();
model.KindId.assign(Graph::kKindIdMaterial);
    model.Nodes.clear();
    model.Links.clear();
    model.Viewport = Graph::Viewport{-80.f, -20.f, 1.f};

    auto addNode = [&](const std::string& typeId, float x, float y) -> std::string
    {
        std::string id = model.GenerateNodeId();
        Graph::Node node = registry.CreateNode(model.KindId, typeId, id, x, y);
        model.Nodes.push_back(std::move(node));
        return id;
    };
    auto link = [&](const std::string& srcId, const std::string& srcPort,
                    const std::string& tgtId, const std::string& tgtPort)
    {
        Graph::Edge L;
        L.Id = model.GenerateLinkId();
        L.SourceNodeId = srcId;
        L.SourcePortId = srcPort;
        L.TargetNodeId = tgtId;
        L.TargetPortId = tgtPort;
        model.Links.push_back(std::move(L));
    };

    const std::string baseColorId = addNode("ColorConstant", 80.f, 110.f);
    const std::string metallicId = addNode("FloatConstant", 80.f, 260.f);
    const std::string roughnessId = addNode("FloatConstant", 80.f, 360.f);
    const std::string normalId = addNode("NormalVector", 80.f, 500.f);
    const std::string viewId = addNode("ViewDirection", 80.f, 620.f);
    const std::string fresnelId = addNode("Fresnel", 350.f, 540.f);
    const std::string rimMaskId = addNode("VectorCompose", 600.f, 500.f);
    const std::string rimColorId = addNode("ColorConstant", 600.f, 700.f);
    const std::string emissiveId = addNode("VectorMultiply", 860.f, 580.f);
    const std::string outputId = addNode("SurfaceOutput", 1160.f, 300.f);

    if (Graph::Node* n = model.FindNode(baseColorId))
    {
        n->Parameters["r"] = "0.08";
        n->Parameters["g"] = "0.28";
        n->Parameters["b"] = "1.0";
    }
    if (Graph::Node* n = model.FindNode(metallicId))
        n->Parameters["value"] = "0.05";
    if (Graph::Node* n = model.FindNode(roughnessId))
        n->Parameters["value"] = "0.28";
    if (Graph::Node* n = model.FindNode(fresnelId))
        n->Parameters["power"] = "2.6";
    if (Graph::Node* n = model.FindNode(rimColorId))
    {
        n->Parameters["r"] = "1.45";
        n->Parameters["g"] = "0.48";
        n->Parameters["b"] = "0.04";
    }

    link(baseColorId, "value", outputId, "BaseColor");
    link(metallicId, "value", outputId, "Metallic");
    link(roughnessId, "value", outputId, "Roughness");
    link(normalId, "normal", fresnelId, "normal");
    link(viewId, "view", fresnelId, "view");
    link(fresnelId, "out", rimMaskId, "x");
    link(fresnelId, "out", rimMaskId, "y");
    link(fresnelId, "out", rimMaskId, "z");
    link(rimMaskId, "out", emissiveId, "a");
    link(rimColorId, "value", emissiveId, "b");
    link(emissiveId, "out", outputId, "Emissive");
    link(normalId, "normal", outputId, "Normal");

        return true;
}

std::filesystem::path MaterialGraphController::SampleGraphAssetPath(
    const std::filesystem::path& assetsRoot) const
{
    if (assetsRoot.empty())
        return {};
    return assetsRoot / "Materials" / "Graph" / "PreviewSphereTest.glsl";
}

void MaterialGraphController::OnContextChanged(const EditorContext* context)
{
    // A context swap invalidates everything the atlas holds on the device it
    // was built against, so it goes down before the new one comes up.
    if (m_Previews)
        m_Previews->Shutdown();
    m_HasSurfaceDigest = false;
    if (!context)
        return;
    if (!m_Previews)
        m_Previews = std::make_unique<Editor::MaterialGraphPreviews>();
    m_Previews->Initialize(context, m_IblEnabled);
}

void MaterialGraphController::SyncNodePreviews(const EditorContext* context,
                                               const Graph::Model& model, bool expandedNodes,
                                               GraphCanvas* canvas)
{
    if (m_Previews)
        m_Previews->Sync(context, model, expandedNodes, canvas, /*gestureActive=*/false);
}

void MaterialGraphController::PumpDeferredPreviewRebuild(const EditorContext* context,
                                                         const Graph::Model& model,
                                                         bool expandedNodes, GraphCanvas* canvas,
                                                         bool gestureActive)
{
    if (m_Previews && m_Previews->Atlas().TakeDeferredRebuild(gestureActive))
        m_Previews->Sync(context, model, expandedNodes, canvas, /*gestureActive=*/false);
}

void MaterialGraphController::SyncMainPreview(MaterialGraphPreviewHost* host)
{
    if (!m_Previews || !host)
        return;
    m_Previews->SetMainPreviewYaw(host->OrbitYaw());
    Editor::GraphNodePreviewBinding binding;
    if (!m_Previews->TryGetMainPreviewBinding(binding))
        binding = {};
    host->ApplyPreviewBinding(binding);
}

void MaterialGraphController::ShutdownPreviews()
{
    if (m_Previews)
        m_Previews->Shutdown();
}

GraphCanvas::NodeFactoryFn MaterialGraphController::MakeNodeFactory()
{
    // The lookup closes over THIS controller, which owns the atlas: no generic
    // graph type carries preview vocabulary, and two graphs open at once cannot
    // cross-bind because each panel has its own controller.
    return [this]()
    {
        auto node = std::make_unique<MaterialGraphNode>();
        node->SetOpenColorPicker(m_OpenColorPicker);
        node->SetPreviewLookup(
            [this](const std::string& nodeId)
            {
                Editor::GraphNodePreviewBinding binding;
                m_Previews && m_Previews->Atlas().TryGetBinding(nodeId, binding);
                return binding;
            });
        return node;
    };
}

void MaterialGraphController::SyncToolbar(const ToolbarItems& items)
{
    ApplyToggleState(items.Preview, m_PreviewEnabled);
    ApplyToggleState(items.PreviewIbl, m_IblEnabled);
    if (items.PreviewIbl)
    {
        items.PreviewIbl->SetTooltip(m_IblEnabled ? "IBL for material preview: On"
                                                  : "IBL for material preview: Off");
    }
    /* A runtime that serves pre-cooked programs only cannot build what a graph edit
       produces, so the two controls that would try are off and carry the reason. Auto also
       reads inactive: the preference is kept, but nothing here acts on it. Resolved on
       every sync rather than once, because the toolbar is rebuilt with the panel. */
    const bool canCompile = Rendering::ShaderCompileService::IsCompilerAvailable();
    ApplyToggleState(items.AutoCompile, m_AutoCompileEnabled && canCompile);

    if (items.AutoCompile)
    {
        items.AutoCompile->SetEnabled(canCompile);
        if (!canCompile)
            items.AutoCompile->SetTooltip(kNoRuntimeShaderCompilerNotice);
    }
    if (items.Compile)
    {
        items.Compile->SetEnabled(canCompile);
        if (!canCompile)
            items.Compile->SetTooltip(kNoRuntimeShaderCompilerNotice);
    }

    if (items.CompileWrap && items.Compile && items.CompileStatusDot)
    {
        items.CompileWrap->Overrides().Set(Style::Display, DisplayMode::Flex);
        if (m_CompileUpToDate)
        {
            items.CompileStatusDot->AddClass("up-to-date");
            items.CompileStatusDot->RemoveClass("stale");
            items.Compile->AddClass("compiled");
            items.Compile->RemoveClass("needs-compile");
        }
        else
        {
            items.CompileStatusDot->AddClass("stale");
            items.CompileStatusDot->RemoveClass("up-to-date");
            items.Compile->AddClass("needs-compile");
            items.Compile->RemoveClass("compiled");
        }
        ApplyCompileStatusDotStyle(*items.CompileStatusDot, m_CompileUpToDate);
        items.CompileStatusDot->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        items.Compile->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        items.CompileWrap->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
}

void MaterialGraphController::ToggleAutoCompile(const ToolbarItems& items)
{
    m_AutoCompileEnabled = !m_AutoCompileEnabled;
    if (m_Host.SaveAutoCompilePreference)
        m_Host.SaveAutoCompilePreference(m_AutoCompileEnabled);
    SyncToolbar(items);
    // Turning it on with an edit still uncompiled compiles now, rather than
    // waiting for the next edit to notice.
    if (m_AutoCompileEnabled && !m_CompileUpToDate && m_Host.RequestCompileRefreshIfStale)
        m_Host.RequestCompileRefreshIfStale();
}

void MaterialGraphController::TogglePreview(const ToolbarItems& items)
{
    m_PreviewEnabled = !m_PreviewEnabled;
    SyncToolbar(items);

    MaterialGraphPreviewHost* previewHost = m_Host.PreviewHost ? m_Host.PreviewHost() : nullptr;
    if (!m_PreviewEnabled)
    {
        if (previewHost)
            previewHost->SetVisibleForMaterialGraph(false);
        // Switching the preview off does not excuse the graph from compiling:
        // the last edit may still be uncompiled, and turning the preview back
        // on must not be the thing that finally builds it.
        if (m_Host.RequestCompileRefreshIfStale)
            m_Host.RequestCompileRefreshIfStale();
        return;
    }

    if (previewHost)
    {
        previewHost->SetIblEnabled(m_IblEnabled);
        previewHost->ClearPreview();
    }
    if (m_Host.RefreshPreview)
        m_Host.RefreshPreview();
}

void MaterialGraphController::ToggleIbl(const ToolbarItems& items)
{
    m_IblEnabled = !m_IblEnabled;
    if (MaterialGraphPreviewHost* previewHost = m_Host.PreviewHost ? m_Host.PreviewHost() : nullptr)
        previewHost->SetIblEnabled(m_IblEnabled);
    // The node previews light the same way the big one does, or the two read as
    // different materials.
    if (m_Previews)
        m_Previews->Atlas().SetIblEnabled(m_IblEnabled);
    SyncToolbar(items);
}

void MaterialGraphController::RefreshBoundPreviewProperties(const Graph::Model& model)
{
    if (!m_PreviewEnabled)
        return;
    if (MaterialGraphPreviewHost* previewHost = m_Host.PreviewHost ? m_Host.PreviewHost() : nullptr)
        previewHost->RefreshMaterialProperties(model);
}

} // namespace GameEngine

namespace GameEngine {

MaterialGraphController::CompileOutcome MaterialGraphController::Compile(const CompileContext& ctx)
{
    CompileOutcome outcome;
    if (!ctx.Model)
        return outcome;
    /* Nothing downstream of here can succeed without a shader compiler: the preview
       material's variant is not cooked, so the build fails with an error naming a cache the
       user cannot produce from the editor. Decline the whole attempt — including the
       preview host's, which is only reached from here — so an edit is quiet instead of
       failing once per keystroke. SyncToolbar carries the reason. */
    if (!Rendering::ShaderCompileService::IsCompilerAvailable())
        return outcome;
    if (ctx.Model->Nodes.empty())
    {
        ClearForEmptyModel(ctx.Path);
        return outcome;
    }

    /* Values-only: nothing in the model that can reach a compiled shader has
       moved, so the previous compile still stands. A scrub costs a uniform push
       and a redraw — no graph compile, no materialized surface, no file. */
    const std::uint64_t digest =
        MaterialGraphSurfaceDigest(Editor::MakeMaterialGraphPreviewModel(*ctx.Model).Model);
    /* m_HasSurfaceDigest is only set by a compile that succeeded, so it is the
       whole precondition. Do NOT add m_CompileUpToDate: OnModelChanged clears it
       before scheduling this, so it is false on arrival every time and gating on
       it made this path unreachable. */
    if (m_HasSurfaceDigest && digest == m_SurfaceDigest)
    {
        outcome.Success = true;
        // The compiled surface still stands — OnModelChanged cleared this on the
        // way in, and nothing here invalidates it.
        m_CompileUpToDate = true;
        if (!ctx.Path.empty())
            SyncPublicVariablesToMaterials(*ctx.Model, ctx.Path, false);
        if (m_PreviewEnabled)
        {
            if (MaterialGraphPreviewHost* host = m_Host.PreviewHost ? m_Host.PreviewHost() : nullptr)
                host->RefreshMaterialProperties(*ctx.Model);
        }
        if (m_Previews)
            m_Previews->SyncValues(ctx.Context, *ctx.Model);
        return outcome;
    }

    const auto result = MaterialGraphCompiler::Compile(*ctx.Model);
    m_CompileUpToDate = result.success;
    outcome.Success = result.success;

    ReportCompileResult(result.errors, ctx.Path);

    MaterialGraphPreviewHost* previewHost = m_Host.PreviewHost ? m_Host.PreviewHost() : nullptr;
    const bool showPreview = result.success && m_PreviewEnabled && previewHost;
    if (showPreview)
    {
        previewHost->SetVisibleForMaterialGraph(true);
        previewHost->RefreshFromModel(*ctx.Model, result.glslSource, ctx.Path);
        if (m_Previews)
            m_Previews->SetMainPreviewMaterial(previewHost->PreviewMaterialGuid());
        // Again next frame: the host needs a laid-out parent before it can size
        // its own view, and that has not happened yet on the first pass.
        if (ctx.PostAction)
        {
            const Graph::Model* model = ctx.Model;
            std::filesystem::path path = ctx.Path;
            ctx.PostAction([this, model, path = std::move(path), glsl = result.glslSource]()
            {
                MaterialGraphPreviewHost* host = m_Host.PreviewHost ? m_Host.PreviewHost() : nullptr;
                if (!m_PreviewEnabled || !host)
                    return;
                // Only a first pass that produced nothing needs the assets
                // again; re-materializing an unchanged graph costs a full graph
                // compile for a result the host already holds.
                if (!host->HasPreviewMaterial())
                    host->RefreshFromModel(*model, glsl, path);
                host->OnParentLayoutReady();
            });
        }
    }
    else if (previewHost)
    {
        previewHost->SetVisibleForMaterialGraph(false);
        if (m_Previews)
            m_Previews->SetMainPreviewMaterial(GUID{});
    }

    /* Runtime-only property push: compiling must never write the graph file.
       Disk and dependent material documents sync on explicit save. */
    if (result.success && !ctx.Path.empty())
        SyncPublicVariablesToMaterials(*ctx.Model, ctx.Path, false);

    if (result.success && m_Previews)
        m_Previews->Sync(ctx.Context, *ctx.Model, ctx.ExpandedNodes, ctx.Canvas, ctx.GestureActive);

    // Only a compile that stands can license the values-only path above.
    m_SurfaceDigest = digest;
    m_HasSurfaceDigest = result.success;

    return outcome;
}

void MaterialGraphController::ReportCompileResult(const std::vector<ShaderGraph::SgDiagnostic>& errors,
                                                  const std::filesystem::path& path)
{
    MirrorToShaderErrors(errors, path);

    std::string summary;
    for (const ShaderGraph::SgDiagnostic& diagnostic : errors)
    {
        if (!summary.empty())
            summary += "; ";
        summary += diagnostic.Message;
    }
    // Only when it changes: an auto-compiling graph that will not compile would
    // otherwise log the same failure on every keystroke.
    if (summary != m_LastLoggedErrors)
    {
        if (!summary.empty())
        {
            if (path.empty())
                Logger::Log::Warning("MaterialGraph: compile failed: {}", summary);
            else
                Logger::Log::Warning("MaterialGraph: compile failed ({}): {}", path.string(), summary);
        }
        m_LastLoggedErrors = std::move(summary);
    }

    if (m_Host.ShowDiagnostics)
        m_Host.ShowDiagnostics(errors);
}

void MaterialGraphController::MirrorToShaderErrors(const std::vector<ShaderGraph::SgDiagnostic>& errors,
                                                   const std::filesystem::path& path) const
{
    // Keyed on the graph file, so the Shader Errors panel lists a broken graph
    // next to the surface shaders it already tracks and resolves it on the next
    // clean compile. A graph with no path on disk has no stable key: skip it.
    if (path.empty())
        return;
    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    if (!renderServices)
        return;

    Engine::Renderer::ShaderCompileErrorLog& log = renderServices->Materials().ShaderErrors();
    const std::string graphName = path.stem().string();
    const std::string graphPath = path.string();

    if (errors.empty())
    {
        log.ReportSuccess(graphName, graphPath);
        return;
    }

    Engine::Renderer::ShaderCompileErrorLog::Entry entry;
    entry.MaterialName = graphName;
    entry.SurfaceShaderPath = graphPath;
    entry.MaterialAssetPath = path;
    entry.When = std::chrono::system_clock::now();
    entry.Errors.reserve(errors.size());
    for (const ShaderGraph::SgDiagnostic& diagnostic : errors)
        entry.Errors.push_back(diagnostic.Message);
    log.ReportFailure(std::move(entry));
}

void MaterialGraphController::ClearForEmptyModel(const std::filesystem::path& path)
{
    // Nothing to compile leaves nothing stale: a pip asking for a Compile that
    // could change nothing would send the user after a button that does nothing.
    m_CompileUpToDate = true;
    if (MaterialGraphPreviewHost* previewHost = m_Host.PreviewHost ? m_Host.PreviewHost() : nullptr)
        previewHost->SetVisibleForMaterialGraph(false);
    m_LastLoggedErrors.clear();
    if (m_Host.ClearDiagnostics)
        m_Host.ClearDiagnostics(MirroredErrorCount(path));
}

std::size_t MaterialGraphController::MirroredErrorCount(const std::filesystem::path& path) const
{
    if (path.empty())
        return 0;
    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    if (!renderServices)
        return 0;

    // Same key MirrorToShaderErrors writes under.
    const std::string graphPath = path.string();
    std::size_t errorCount = 0;
    for (const Engine::Renderer::ShaderCompileErrorLog::Entry& entry :
         renderServices->Materials().ShaderErrors().Snapshot())
    {
        if (entry.SurfaceShaderPath == graphPath)
            errorCount += entry.Errors.size();
    }
    return errorCount;
}

namespace {

std::string Trimmed(const std::string& value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

} // namespace

void MaterialGraphController::OnModelShown(const Graph::Model& model)
{
    const std::filesystem::path path =
        m_Host.CurrentPath ? m_Host.CurrentPath() : std::filesystem::path{};
    if (model.Nodes.empty())
    {
        ClearForEmptyModel(path);
        return;
    }
    if (m_Host.RequestCompileRefreshIfStale)
        m_Host.RequestCompileRefreshIfStale();
    /* The refresh, when one runs, replaces the verdict in view with this file's.
       With auto-compile and the preview both off none runs, so the verdict in
       view is still the previous file's: clear it, leaving only what the Shader
       Errors mirror holds for the file now shown. */
    const bool previewHostPresent = m_Host.PreviewHost && m_Host.PreviewHost() != nullptr;
    if (!WantsCompileRefresh(previewHostPresent) && m_Host.ClearDiagnostics)
        m_Host.ClearDiagnostics(MirroredErrorCount(path));
}

void MaterialGraphController::OnDeactivated()
{
    // Preview host is this kind's; take it down on shutdown.
    if (MaterialGraphPreviewHost* previewHost = m_Host.PreviewHost ? m_Host.PreviewHost() : nullptr)
        previewHost->SetVisibleForMaterialGraph(false);
}

void MaterialGraphController::OnBeforeSave(Graph::Model& model)
{
    // Written form has to match what the nodes now say: a parameter node added
    // or renamed since the last save has a variable to declare and a slot to
    // claim, and neither can be left for the reader of the file to work out.
    SyncGraphVariables(model);
    SyncMaterialParameterNodeSlots(model);
}

void MaterialGraphController::OnAfterSave()
{
    if (m_Host.RecompileAndReport)
        m_Host.RecompileAndReport();
    // Now that the graph is on disk, the materials that use it take the new
    // public properties, documents and all.
    const Graph::Model* model = m_Host.Model ? m_Host.Model() : nullptr;
    if (model && m_Host.CurrentPath)
        SyncPublicVariablesToMaterials(*model, m_Host.CurrentPath(), true);
}

std::filesystem::path MaterialGraphController::DefaultSaveDirectory(
    const std::filesystem::path& assetsRoot) const
{
    // Shader graphs live with the materials they build.
    return assetsRoot / "Materials" / "Graph";
}

void MaterialGraphController::SyncGraphVariables(Graph::Model& model)
{
    std::unordered_map<std::string, size_t> indexByName;
    for (size_t i = 0; i < model.Variables.size(); ++i)
    {
        const std::string name = Trimmed(model.Variables[i].Name);
        if (!name.empty())
            indexByName[name] = i;
    }

    std::uint64_t nextCreatedOrder = 0;
    for (const Graph::Variable& variable : model.Variables)
        nextCreatedOrder = std::max(nextCreatedOrder, variable.CreatedOrder + 1);

    for (const Graph::Node& node : model.Nodes)
    {
        if (!MaterialGraphVariables::IsVariableNode(node))
            continue;
        auto it = node.Parameters.find("variableName");
        if (it == node.Parameters.end())
            continue;
        const std::string name = Trimmed(it->second.ToString());
        if (name.empty())
            continue;

        const std::string nodeType = MaterialGraphVariables::TypeFromNode(node);
        if (auto existing = indexByName.find(name); existing != indexByName.end())
        {
            // An existing declaration keeps its type; only an untyped one takes
            // the node's, so renaming a node cannot retype a variable in use.
            Graph::Variable& declared = model.Variables[existing->second];
            if (declared.Type.empty())
                declared.Type = nodeType;
            continue;
        }

        Graph::Variable variable;
        variable.Name = name;
        variable.Type = nodeType;
        variable.Value = MaterialGraphVariables::DefaultValueForType(variable.Type);
        variable.CreatedOrder = nextCreatedOrder++;
        indexByName[name] = model.Variables.size();
        model.Variables.push_back(std::move(variable));
    }
}

void MaterialGraphController::SyncPublicVariablesToMaterials(const Graph::Model& model,
                                                             const std::filesystem::path& path,
                                                             bool syncDisk)
{
    if (path.empty())
        return;
    auto* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
        return;

    std::vector<ShaderGraph::SgGraphProperty> properties;
    properties.reserve(model.Variables.size());
    for (const Graph::Variable& variable : model.Variables)
    {
        if (variable.Name.empty())
            continue;
        properties.push_back(GraphVariableToSgProperty(variable));
    }

    if (syncDisk)
    {
        const std::vector<GUID> updated =
            rs->Materials().Compiler().SyncMaterialDocumentsFromShaderGraph(path, &properties);
        for (const GUID& materialGuid : updated)
        {
            if (auto asset = EngineCore::GetInstance().GetAssetManager().GetAsset(materialGuid))
            {
                if (auto* matAsset = dynamic_cast<MaterialAsset*>(asset.get()))
                    rs->Materials().RegisterMaterialFromDocument(materialGuid, matAsset->GetDocument());
            }
        }
        rs->Materials().Compiler().NotifySurfaceGraphChanged(path);
    }

    rs->Materials().Compiler().PushPublicShaderGraphPropertiesToRuntime(path, properties, *rs);
}

} // namespace GameEngine
