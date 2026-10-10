#include "Panels/BuiltInPanelTypes.h"

#include "Editor/Registries/EditorPanelRegistry.h"
#include "EditorPanelIds.h"
#include "Graph/GraphModel.h"
#include "Panels/AnimationWindowPanel.h"
#include "Panels/AssetViewPanel.h"
#include "Panels/AssetsPanel.h"
#include "Panels/BookmarksPanel.h"
#include "Panels/BuildPanel.h"
#include "Panels/CpuProfilerPanel.h"
#include "Panels/DiffPanel.h"
#include "Panels/GameViewPanel.h"
#include "Panels/GraphPanel.h"
#include "Panels/HierarchyPanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/LogPanel.h"
#include "Panels/MissingAssetsPanel.h"
#include "Panels/MixerPanel.h"
#include "Panels/MonitorsPanel.h"
#include "Panels/PackageManagerPanel.h"
#include "Panels/RenderGraphPanel.h"
#include "Panels/SceneViewPanel.h"
#include "Panels/ScriptEditorPanel.h"
#include "Panels/ScriptErrorsPanel.h"
#include "Panels/SettingsPanel.h"
#include "Panels/ShaderErrorsPanel.h"
#include "Panels/TodoPanel.h"
#include "Panels/UIDemoPanel.h"
#include "Panels/UndoHistoryPanel.h"
#include "Panels/VisualProfilerPanel.h"
#include "Panels/VramPanel.h"
#include "Panels/WebPanel.h"

#include <memory>

namespace GameEngine::Editor
{
namespace
{

using PanelFactory = std::unique_ptr<UIElement> (*)();

struct BuiltInPanelType
{
    const char* TypeKey;
    PanelFactory Factory;
};

template <typename T>
std::unique_ptr<UIElement> MakePanel()
{
    return std::make_unique<T>();
}

std::unique_ptr<UIElement> MakeMaterialGraphPanel()
{
    return GraphPanel::CreateForKind(GameEngine::Graph::kKindIdMaterial);
}

std::unique_ptr<UIElement> MakeAnimationGraphPanel()
{
    return GraphPanel::CreateForKind(GameEngine::Graph::kKindIdAnimation);
}

std::unique_ptr<UIElement> MakeGameLogicGraphPanel()
{
    return GraphPanel::CreateForKind(GameEngine::Graph::kKindIdGameLogic);
}

// Every panel type a dock-config row may name. Tests/Editor/PanelDefaultTabIconTests
// reads this table's `{"Key", &MakePanel<Class>}` rows to check each type's tab icon.
constexpr BuiltInPanelType kBuiltInPanelTypes[] = {
    {"AssetsPanel", &MakePanel<AssetsPanel>},
    {"AssetViewPanel", &MakePanel<AssetViewPanel>},
    {"TodoPanel", &MakePanel<TodoPanel>},
    {"BookmarksPanel", &MakePanel<BookmarksPanel>},
    {"HierarchyPanel", &MakePanel<HierarchyPanel>},
    {"InspectorPanel", &MakePanel<InspectorPanel>},
    {"InspectorPanel2", &MakePanel<InspectorPanel>},
    {"GameViewPanel", &MakePanel<GameViewPanel>},
    {"SceneViewPanel", &MakePanel<SceneViewPanel>},
    {"UIDemoPanel", &MakePanel<UIDemoPanel>},
    {"MissingAssetsPanel", &MakePanel<MissingAssetsPanel>},
    {"PackageManagerPanel", &MakePanel<PackageManagerPanel>},
    {"LogPanel", &MakePanel<LogPanel>},
    {"ScriptErrorsPanel", &MakePanel<ScriptErrorsPanel>},
    {"ShaderErrorsPanel", &MakePanel<ShaderErrorsPanel>},
    {"MonitorsPanel", &MakePanel<MonitorsPanel>},
    {"RenderGraphPanel", &MakePanel<RenderGraphPanel>},
    {"CpuProfilerPanel", &MakePanel<CpuProfilerPanel>},
    {"VisualProfilerPanel", &MakePanel<VisualProfilerPanel>},
    {"VramPanel", &MakePanel<VramPanel>},
    {"ScriptEditorPanel", &MakePanel<ScriptEditorPanel>},
    {"SettingsPanel", &MakePanel<SettingsPanel>},
    {"DiffPanel", &MakePanel<DiffPanel>},
    {"WebPanel", &MakePanel<WebPanel>},
    {"BuildPanel", &MakePanel<BuildPanel>},
    {"AnimationWindowPanel", &MakePanel<AnimationWindowPanel>},
    {"AnimationTimelinePanel", &MakePanel<AnimationTimelinePanel>},
    {"AnimationClipEditorPanel", &MakePanel<AnimationClipEditorPanel>},
    {"MixerPanel", &MakePanel<MixerPanel>},
    {"UndoHistoryPanel", &MakePanel<UndoHistoryPanel>},
    {"NodeGraphPanel", &MakeMaterialGraphPanel},
    {"ShaderGraphPanel", &MakeMaterialGraphPanel},
    {"AnimationGraphPanel", &MakeAnimationGraphPanel},
    {"GameLogicGraphPanel", &MakeGameLogicGraphPanel},
};

constexpr FallbackDockPanel kFallbackDockPanels[] = {
    {EditorPanelIds::Assets, "AssetsPanel"},
    {EditorPanelIds::Hierarchy, "HierarchyPanel"},
    {EditorPanelIds::Inspector, "InspectorPanel"},
    {EditorPanelIds::Inspector2, "InspectorPanel"},
    {EditorPanelIds::GameView, "GameViewPanel"},
    {EditorPanelIds::SceneView, "SceneViewPanel"},
    {EditorPanelIds::UiDemo, "UIDemoPanel"},
    {EditorPanelIds::Log, "LogPanel"},
    {EditorPanelIds::ScriptErrors, "ScriptErrorsPanel"},
    {EditorPanelIds::Web, "WebPanel"},
    {EditorPanelIds::NodeGraph, "NodeGraphPanel"},
    {EditorPanelIds::Build, "BuildPanel"},
    {EditorPanelIds::Animation, "AnimationWindowPanel"},
    {EditorPanelIds::Timeline, "AnimationTimelinePanel"},
    {EditorPanelIds::ClipEditor, "AnimationClipEditorPanel"},
    {EditorPanelIds::ShaderErrors, "ShaderErrorsPanel"},
    {EditorPanelIds::AnimationGraph, "AnimationGraphPanel"},
    {EditorPanelIds::GameLogicGraph, "GameLogicGraphPanel"},
};

} // namespace

void RegisterBuiltInPanelTypes()
{
    auto& registry = EditorPanelRegistry::Get();
    for (const BuiltInPanelType& type : kBuiltInPanelTypes)
        registry.RegisterPanelType({type.TypeKey, type.Factory});
}

std::span<const FallbackDockPanel> FallbackDockPanels()
{
    return kFallbackDockPanels;
}

} // namespace GameEngine::Editor
