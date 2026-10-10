#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

namespace GameEngine::Editor
{

// Editor-installed scene/asset services for package modules — deliberately
// the same installed-service shape as EditorPanelRegistry::SetPanelOpener:
// the editor installs handlers at startup; module calls before install are
// loud no-ops. Unlike the other registries this one is called from module
// WORKER threads (asset registration runs on an importer's worker), so the
// handlers are handed out under a lock and invoked unlocked.
class EditorSceneCommands
{
public:
    static EditorSceneCommands& Get();

    using OpenSceneHandler = std::function<void(const std::filesystem::path&)>;
    // Registers freshly imported files with the project's asset registry so
    // path-form references resolve deterministically instead of racing the
    // FileWatcher debounce. Returns how many files FAILED to register.
    using RegisterAssetsHandler = std::function<size_t(const std::vector<std::filesystem::path>&)>;

    void SetOpenSceneHandler(OpenSceneHandler handler);
    void SetRegisterAssetsHandler(RegisterAssetsHandler handler);

    // Open a scene in the editor's scene document flow. Loud no-op without an
    // installed handler.
    void OpenScene(const std::filesystem::path& scenePath) const;

    // Register imported files now; returns the count that failed (all of them
    // when no handler is installed — callers treat unregistered files as
    // "resolves after the next scan", never as fatal).
    size_t RegisterImportedAssets(const std::vector<std::filesystem::path>& files) const;

private:
    EditorSceneCommands() = default;

    mutable std::mutex m_Mutex;
    OpenSceneHandler m_OpenScene;
    RegisterAssetsHandler m_RegisterAssets;
};

} // namespace GameEngine::Editor
