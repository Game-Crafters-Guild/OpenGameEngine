// Editor-module entry: registers the Git VCS provider when the git-vcs
// editor DLL loads (static init — the same single-TU pattern the other VCS
// package modules use). The DLL links the EditorSDK import lib, so the
// registration lands in the HOST's EditorVcsProviderRegistry; the registry's
// observer handles providers arriving after project open (re-detect) and
// module reloads (replace-forward with a loud disconnect of the previous
// integration). Git is the default provider (DetectionOrder 0) — the editor
// gates its FIRST workspace detection on the package-module load pass, so a
// mixed .git+.svn workspace can never see a transient SVN claim while this
// module is still loading.

#include "Editor/Registries/EditorPanelRegistry.h"

namespace GameEngine::Editor
{
void RegisterGitVcsProvider();
}

namespace
{

struct GitVcsModuleRegistrar
{
    GitVcsModuleRegistrar()
    {
        GameEngine::Editor::RegisterGitVcsProvider();
        // Settings-tree row icon for the Git tab (chrome CSS ships with the
        // package; the icon styles an element outside any panel subtree).
        GameEngine::Editor::EditorPanelRegistry::Get().RegisterEditorStyleSheet(
            {"git-vcs", "Editor/UI/GitVcsChrome.css"});
    }
};

GitVcsModuleRegistrar s_Registrar;

} // namespace
