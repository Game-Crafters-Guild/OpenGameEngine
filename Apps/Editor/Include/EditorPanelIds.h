#pragma once

/**
 * Centralized panel and layout element IDs used when wiring EditorApplication.
 * Avoids fragile string literals scattered in EditorApplication.cpp.
 * Must match ids in layout.uxml and dock config.
 */
namespace GameEngine {
namespace EditorPanelIds {

// Panel IDs (DockPanel id attribute; used with GetPanel(), ActivateTab(), AddTab()).
inline constexpr const char* Assets = "Assets";
inline constexpr const char* AssetView = "AssetView";
inline constexpr const char* AssetViewSceneCopy = "AssetViewSceneCopy";
inline constexpr const char* Inspector = "Inspector";
inline constexpr const char* Inspector2 = "Inspector 2";
inline constexpr const char* Log = "Log";
inline constexpr const char* UiDemo = "UI Demo";
inline constexpr const char* Build = "Build";
inline constexpr const char* Mixer = "Mixer";
inline constexpr const char* NodeGraph = "NodeGraph";
inline constexpr const char* AnimationGraph = "AnimationGraph";
inline constexpr const char* GameLogicGraph = "GameLogicGraph";
inline constexpr const char* ScriptEditor = "ScriptEditor";
inline constexpr const char* Animation = "Animation";
inline constexpr const char* Timeline = "Timeline";
inline constexpr const char* ClipEditor = "ClipEditor";
inline constexpr const char* SceneView = "SceneView";
inline constexpr const char* GameView = "GameView";
inline constexpr const char* Hierarchy = "Hierarchy";
inline constexpr const char* Settings = "Settings";
inline constexpr const char* Todo = "Todo";
inline constexpr const char* Bookmarks = "Bookmarks";
inline constexpr const char* Web = "Web";
inline constexpr const char* Diff = "Diff";
inline constexpr const char* Monitors = "Monitors";
inline constexpr const char* VisualProfiler = "VisualProfiler";
inline constexpr const char* RenderGraph = "RenderGraph";
inline constexpr const char* Vram = "Vram";
inline constexpr const char* CpuProfiler = "CpuProfiler";
inline constexpr const char* UndoHistory = "UndoHistory";
inline constexpr const char* MissingAssets = "MissingAssets";
inline constexpr const char* ScriptErrors = "ScriptErrors";
inline constexpr const char* ShaderErrors = "ShaderErrors";
inline constexpr const char* PackageManager = "PackageManager";

// Mount IDs / focus IDs used for mounted panel lookup and input gating.
inline constexpr const char* MountSceneView = "mount:SceneView";
inline constexpr const char* MountGameView = "mount:GameView";
inline constexpr const char* GameViewViewport = "GameViewViewport";
inline constexpr const char* GameViewNoCameraLabel = "GameViewNoCameraLabel";

// Top toolbar element IDs (layout.uxml) for wiring button callbacks.
inline constexpr const char* TopToolbarUniversalSearch = "TopToolbarUniversalSearch";
inline constexpr const char* TopToolbarBuild = "TopToolbarBuild";
inline constexpr const char* TopToolbarMixer = "TopToolbarMixer";
inline constexpr const char* TopToolbarMonitors = "TopToolbarMonitors";
inline constexpr const char* TopToolbarVisualProfiler = "TopToolbarVisualProfiler";
inline constexpr const char* TopToolbarRenderGraph = "TopToolbarRenderGraph";
inline constexpr const char* TopToolbarAnimation = "TopToolbarAnimation";
inline constexpr const char* TopToolbarTimeline = "TopToolbarTimeline";
inline constexpr const char* TopToolbarLog = "TopToolbarLog";
inline constexpr const char* TopToolbarVram = "TopToolbarVram";
inline constexpr const char* TopToolbarCpuProfiler = "TopToolbarCpuProfiler";
inline constexpr const char* TopToolbarPanelMenu = "TopToolbarPanelMenu";
inline constexpr const char* TopToolbarNodeGraph = "TopToolbarNodeGraph";
inline constexpr const char* TopToolbarSettings = "TopToolbarSettings";
inline constexpr const char* TopToolbarHelp = "TopToolbarHelp";
inline constexpr const char* TopToolbarBookmarks = "TopToolbarBookmarks";
inline constexpr const char* TopToolbarTodos = "TopToolbarTodos";
inline constexpr const char* TopToolbarUndoHistory = "TopToolbarUndoHistory";
inline constexpr const char* TopToolbarPackages = "TopToolbarPackages";
inline constexpr const char* TopToolbarScriptEditor = "TopToolbarScriptEditor";
inline constexpr const char* TopToolbarRight1 = "TopToolbarRight1";
inline constexpr const char* TopToolbarRecord = "TopToolbarRecord";
inline constexpr const char* TopToolbarCenter2 = "TopToolbarCenter2";
inline constexpr const char* TopToolbarCenter3 = "TopToolbarCenter3";
inline constexpr const char* TopToolbarCenter4 = "TopToolbarCenter4";
inline constexpr const char* TopToolbarLeft1 = "TopToolbarLeft1";
inline constexpr const char* TopToolbarLeft2 = "TopToolbarLeft2";
inline constexpr const char* TopToolbarLeft3 = "TopToolbarLeft3";

} // namespace EditorPanelIds
} // namespace GameEngine
