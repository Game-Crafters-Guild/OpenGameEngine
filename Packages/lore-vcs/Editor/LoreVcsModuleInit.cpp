// Editor-module entry: registers the Lore VCS provider when the lore-vcs
// editor DLL loads (static init — the same single-TU pattern the eztree
// editor module uses). The DLL links the EditorSDK import lib, so the
// registration lands in the HOST's EditorVcsProviderRegistry; the registry's
// observer handles providers arriving after project open (re-detect) and
// module reloads (replace-forward with a loud disconnect of the previous
// integration).

#include "Editor/Registries/EditorPanelRegistry.h"

namespace GameEngine::Editor
{
void RegisterLoreVcsProvider();
}

namespace
{

struct LoreVcsModuleRegistrar
{
    LoreVcsModuleRegistrar()
    {
        GameEngine::Editor::RegisterLoreVcsProvider();
        // Settings-tree row icon for the Lore tab (chrome CSS ships with the
        // package; the icon styles an element outside any panel subtree).
        GameEngine::Editor::EditorPanelRegistry::Get().RegisterEditorStyleSheet(
            {"lore-vcs", "Editor/UI/LoreVcsChrome.css"});
    }
};

LoreVcsModuleRegistrar s_Registrar;

} // namespace
