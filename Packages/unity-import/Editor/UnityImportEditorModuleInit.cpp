// Editor-module entry: registers the "Import Unity Package" modal as a UI
// overlay and its Tools-menu item when the unity-import editor DLL loads
// (static init — the same single-TU pattern EZTreeEditorModuleInit uses).
// The DLL links the EditorSDK import lib, so registrations land in the
// HOST's editor registries; both replay for modules that load after editor
// startup (packages load at project open).

#include "UnityImportModal.h"

#include "Editor/EditorPaths.h"
#include "Editor/Registries/EditorMenuRegistry.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "Editor/Registries/EditorSceneCommands.h"
#include "Logger/Logger.h"
#include "Platform/Shell.h"

#include <filesystem>
#include <memory>
#include <system_error>

namespace
{

// Owned by the editor UI tree once the overlay consumer attaches it; the
// module drives Show() through this pointer. Cleared by the tracking
// subclass below when the owner destroys the modal (headless factory runs
// in tests, editor teardown) so the menu path never touches a dead element.
GameEngine::UnityImportModal* g_Modal = nullptr;

// Factory-produced modal that untracks itself on destruction. The identity
// check keeps a stale instance's teardown from nulling the pointer after a
// newer factory run replaced it.
struct TrackedUnityImportModal final : GameEngine::UnityImportModal
{
    ~TrackedUnityImportModal() override
    {
        if (g_Modal == this)
            g_Modal = nullptr;
    }
};

// The converter assembly ships INSIDE this package: Tools/UnityConverter.dll
// is built by the OpenEngine-Unity-Scene-Converter repository and vendored at
// the commit Tools/UnityConverter.dll.source names. ge_stage_packages
// copies the package to <exe dir>/Packages/unity-import/ — the same
// exe-anchored tree the PackageResolver's implicit engine-package scan mounts.
// Anchoring to the exe keeps the converter version locked to the engine
// build; a project shadowing this package for package-dev builds its own
// module and can rewire the path.
std::filesystem::path StagedConverterDllPath()
{
    return GameEngine::Platform::GetExecutablePath().parent_path() / "Packages" /
           "unity-import" / "Tools" / "UnityConverter.dll";
}

void ShowImportModal()
{
    using namespace GameEngine;
    if (!g_Modal)
    {
        Logger::Log::Error("UnityImport: menu item invoked before the modal overlay attached; "
                           "check the editor log for the overlay consumer");
        return;
    }

    // Resolve the project context at Show() time — the open project can
    // change between shows, and packages outlive project switches.
    const Editor::EditorProjectPaths pp = Editor::GetCurrentEditorProjectPaths();
    std::filesystem::path assetsDir = pp.projectRoot / "assets";
    std::error_code ec;
    if (std::filesystem::exists(pp.projectRoot / "Assets", ec))
        assetsDir = pp.projectRoot / "Assets";
    g_Modal->SetProjectContext(assetsDir, pp.projectCacheRoot,
                               pp.projectRoot / "AssetDatabase.assetdb");
    g_Modal->Show();
}

struct UnityImportEditorModuleRegistrar
{
    UnityImportEditorModuleRegistrar()
    {
        using namespace GameEngine;

        Editor::EditorPanelRegistry::Get().RegisterOverlay(
            {"unityImport.modal", []() -> std::unique_ptr<UIElement> {
                 auto modal = std::make_unique<TrackedUnityImportModal>();
                 g_Modal = modal.get();
                 modal->SetConverterDll(StagedConverterDllPath());
                 modal->SetOnOpenScene([](const std::filesystem::path& scenePath) {
                     Editor::EditorSceneCommands::Get().OpenScene(scenePath);
                 });
                 // Import-quiescence nudge: register each moved output with
                 // the editor so path-form scene references (seeded
                 // Models_Unity FBX, staged source textures) resolve at open
                 // instead of racing the FileWatcher debounce. Runs on the
                 // modal's worker thread.
                 modal->SetOnAssetsChanged([](const std::vector<std::filesystem::path>& movedFiles) {
                     const size_t failed =
                         Editor::EditorSceneCommands::Get().RegisterImportedAssets(movedFiles);
                     if (failed > 0)
                     {
                         Logger::Log::Warning(
                             "UnityImport: {} of {} imported file(s) failed to register; "
                             "their scene references resolve after the next scan",
                             failed, movedFiles.size());
                     }
                 });
                 return modal;
             }});

        Editor::EditorMenuRegistry::Get().RegisterMenuItem(
            {"Tools/Import Unity Package...", 0, [] { ShowImportModal(); }});
    }
};

UnityImportEditorModuleRegistrar s_Registrar;

} // namespace
