// Editor-module entry: registers the SVN VCS provider when the svn-vcs
// editor DLL loads (static init — the same single-TU pattern the lore-vcs
// editor module uses). The DLL links the EditorSDK import lib, so the
// registration lands in the HOST's EditorVcsProviderRegistry; the registry's
// observer handles providers arriving after project open (re-detect) and
// module reloads (replace-forward with a loud disconnect of the previous
// integration).

#include "Editor/Registries/EditorPanelRegistry.h"

namespace GameEngine::Editor
{
void RegisterSvnVcsProvider();
}

namespace
{

struct SvnVcsModuleRegistrar
{
    SvnVcsModuleRegistrar()
    {
        GameEngine::Editor::RegisterSvnVcsProvider();
        // Settings-tree row icon for the SVN tab (chrome CSS ships with the
        // package; the icon styles an element outside any panel subtree).
        GameEngine::Editor::EditorPanelRegistry::Get().RegisterEditorStyleSheet(
            {"svn-vcs", "Editor/UI/SvnVcsChrome.css"});
    }
};

SvnVcsModuleRegistrar s_Registrar;

} // namespace
