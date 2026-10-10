#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace GameEngine {

class AssetManager;
class EditorApplication;
class FileWatchingService;
class IThumbnailProvider; // to be defined by ThumbnailService later
class EditorVersionControlService;
class MissingAssetTracker;
namespace Platform { class Window; }
class PolyhavenDownloadManager;
namespace Editor { class UndoRedoService; class PlayModeManager; class RenderGraphTickRegistry; }
namespace Engine::Renderer { class RenderServices; }
namespace ECS { class World; }

struct EditorContext {
    std::filesystem::path AssetsRoot;           // absolute path to runtime assets
    AssetManager*         Assets = nullptr;     // optional
    FileWatchingService*  FileWatcher = nullptr;// optional (defaults to global singleton)
    IThumbnailProvider*   Thumbnails = nullptr; // optional
    /// Main editor window id — GPU-backed model thumbnails are keyed per window; panels use this to address that window.
    uint64_t              ThumbnailHostWindowId = 0;
    Platform::Window*     MainWindow = nullptr; // optional (host window for editor UI)
    Editor::UndoRedoService* UndoRedo = nullptr; // optional (for undoable editor actions)
    EditorVersionControlService* VcsService = nullptr; // optional (VCS init + status notifications)
    Engine::Renderer::RenderServices* RenderServices = nullptr; // optional (for model instantiation, GPU resource access)
    Editor::PlayModeManager* PlayMode = nullptr; // optional (play mode state for runtime entity distinction)
    PolyhavenDownloadManager* DownloadManager = nullptr; // optional (background Polyhaven downloads)
    MissingAssetTracker* MissingAssets = nullptr; // optional (broken-asset-reference tracking for the open scene)
    /// Where an editor-side object registers a per-frame render-graph callback so it
    /// can declare its own passes into each window's frame. The application drains
    /// this registry without knowing what registered; participants own their handle
    /// and unregister on teardown.
    Editor::RenderGraphTickRegistry* RenderGraphTicks = nullptr;
    /// Active ECS world for the open scene. Borrowed; ownership lives with the
    /// engine. Code that needs a world from the editor context (e.g. the
    /// MissingAssetTracker rescan path) reads this rather than reaching into
    /// EngineCore — keeps the singleton-grab in one place (EditorApplication)
    /// and lets future multi-world editing change a single field instead of
    /// every consumer.
    ECS::World*           World = nullptr;
    std::function<void()> OnSceneDirty; // optional (mark scene dirty from drop handlers)
    std::function<bool()> IsSceneView2D; // optional (true if the active scene view is in 2D mode)
    // Optional: query deferred scene-build progress for the scene-view load
    // indicator. Returns true (filling processed/total) while a replace-open's
    // entity resolve drains across frames; false when no build is active.
    std::function<bool(uint64_t& processed, uint64_t& total)> SceneBuildProgress;
    bool UIReplayActive = false; // optional (disable noisy background UI updates)
};

} // namespace GameEngine

