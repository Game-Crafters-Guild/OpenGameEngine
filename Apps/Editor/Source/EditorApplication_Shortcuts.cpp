#include "EditorApplication.h"

#include "Editor/Shortcuts/EditorShortcuts.h"
#include "EditorPanelManager.h"
#include "EditorWindowBootstrap.h"
#include "Input/InputSystem.h"
#include "UI/UIManager.h"

namespace GameEngine
{

bool EditorApplication::TryHandleCatalogGlobalKeyPre(
    EditorWindowContext* ctx, int key, int action, int mods, bool includeWorkspaceShortcuts)
{
    if (action != Input::kKeyActionPress)
        return false;

    UIManager* ui = ctx ? ctx->ui.get() : nullptr;

    if (Editor::MatchesCatalogShortcut("Global", "CSS Inspector", key, mods))
    {
        if (!IsFocusInTextField(ui))
        {
            m_CssInspector.ToggleEnabled();
            return true;
        }
    }

    if (includeWorkspaceShortcuts &&
        Editor::MatchesCatalogShortcut("Assets", "Toggle Preview", key, mods))
    {
        if (!IsFocusInTextField(ui))
        {
            ShowOrFocusAssetViewInSceneTabs(ctx);
            return true;
        }
    }

    if (m_UndoRedo)
    {
        if (Editor::MatchesCatalogShortcut("Global", "Undo", key, mods))
        {
            Undo();
            return true;
        }
        if (Editor::MatchesCatalogShortcut("Global", "Redo", key, mods) ||
            Editor::MatchesCatalogShortcut("Global", "Redo (alt)", key, mods))
        {
            Redo();
            return true;
        }
    }

    if (includeWorkspaceShortcuts &&
        Editor::MatchesCatalogShortcut("Global", "Close Tab", key, mods))
    {
        if (m_PanelManager)
            m_PanelManager->CloseActiveTabOrWindow(ctx);
        return true;
    }

    return false;
}

} // namespace GameEngine
