#include "EditorToolbar/EditorToolbar.h"
#include "Core/Engine.h"
#include "Editor/Registries/EditorMenuRegistry.h"
#include "EditorApplication.h"
#include "EditorPanelManager.h"
#include "Platform/Toolbar.h"
#include "Platform/Window.h"
#include "UI/EditorIcons.h"
#include "Scripting/EditorScriptMenuRegistry.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace
{
constexpr uint32_t kCmdWindowLayoutReset = 0x2001;
constexpr uint32_t kCmdEditUndo = 0x1001;
constexpr uint32_t kCmdEditRedo = 0x1002;

// Generate this menu from EditorPanelManager's live inventory instead of a
// hard-coded panel table. This includes package panels and keeps titles and icons
// consistent with the top-toolbar burger menu.
// Range 0x3000–0x30FF is reserved for the generated native-menu commands.
constexpr uint32_t kWindowPanelCommandFirst = 0x3001;
constexpr uint32_t kWindowPanelCommandLast = 0x30FF;

static void BuildUndoRedoTitles(EditorApplication* app, std::string& undoTitle, std::string& redoTitle)
{
    undoTitle = "Undo";
    if (app && app->CanUndo())
    {
        if (const char* name = app->GetUndoActionName())
        {
            undoTitle += " ";
            undoTitle += name;
        }
    }
    undoTitle += "\tCtrl+Z";

    redoTitle = "Redo";
    if (app && app->CanRedo())
    {
        if (const char* name = app->GetRedoActionName())
        {
            redoTitle += " ";
            redoTitle += name;
        }
    }
    redoTitle += "\tCtrl+Y";
}
} // namespace

EditorToolbar::~EditorToolbar()
{
    Uninstall();
}

bool EditorToolbar::Install(Platform::Window* window, EditorApplication* app)
{
    if (!window || !app)
        return false;
    m_Toolbar = CreateNativeToolbar();
    if (!m_Toolbar)
        return false;
    if (!m_Toolbar->Install(window))
    {
        m_Toolbar.reset();
        return false;
    }

    m_App = app;
    m_Toolbar->SetCommandHandler([this](uint32_t cmd)
                                 {
        if (!m_App)
            return;
        if (cmd == kCmdWindowLayoutReset) m_App->ResetLayoutToDefault();
        else if (cmd == kCmdEditUndo) m_App->Undo();
        else if (cmd == kCmdEditRedo) m_App->Redo();
        else if (const auto panel = m_WindowPanelCommands.find(cmd);
                 panel != m_WindowPanelCommands.end())
        {
            m_App->OpenPanel(panel->second);
        }
        else if (Editor::EditorMenuRegistry::Get().TryInvoke(cmd))
        {
            // Native package/module menu item — dispatched by the registry.
        }
        else
        {
            // Script-driven toolbar items (Editor-managed snapshot)
            uint64_t dom = 0;
            std::string method;
            if (Editor::ScriptMenuRegistry::Get().TryResolveCommand(cmd, dom, method) && !method.empty())
            {
                try
                {
                    auto& eng = EngineCore::GetInstance();
                    auto& clr = eng.GetScriptManager().GetCLRHost();
                    int32_t out = 0;
                    // Domain 0 is treated as \"current\" by managed routing; prefer snapshot domain when provided.
                    (void)clr.InvokeInDomain(dom, method.c_str(), (uint32_t)method.size(), &out);
                }
                catch (...)
                {
                    // best-effort; keep menu handler exception-free
                }
            }
        } });

    Build(app);
    return true;
}

void EditorToolbar::Uninstall()
{
    if (m_Toolbar)
    {
        m_Toolbar->Uninstall();
        m_Toolbar.reset();
    }
    m_App = nullptr;
    m_PanelManager = nullptr;
}

void EditorToolbar::Clear()
{
    if (m_Toolbar)
        m_Toolbar->Clear();
}

void EditorToolbar::Refresh()
{
    if (!m_Toolbar || !m_App)
        return;
    Build(m_App);
}

void EditorToolbar::UpdateUndoRedoTitles()
{
    if (!m_Toolbar || !m_App)
        return;

    std::string undoTitle;
    std::string redoTitle;
    BuildUndoRedoTitles(m_App, undoTitle, redoTitle);

    m_Toolbar->UpdateItemTitle(kCmdEditUndo, undoTitle);
    m_Toolbar->UpdateItemTitle(kCmdEditRedo, redoTitle);
}

uint32_t EditorToolbar::AddMenu(const std::string& title)
{
    return m_Toolbar ? m_Toolbar->AddMenu(title) : 0;
}

uint32_t EditorToolbar::AddSubMenu(uint32_t parentMenuId, const std::string& title)
{
    return m_Toolbar ? m_Toolbar->AddSubMenu(parentMenuId, title) : 0;
}

void EditorToolbar::AddItem(uint32_t parentMenuId, const std::string& title, uint32_t commandId)
{
    if (m_Toolbar)
        m_Toolbar->AddItem(parentMenuId, title, commandId);
}

void EditorToolbar::SetItemIcon(uint32_t commandId, const std::string& imagePath)
{
    if (m_Toolbar)
        m_Toolbar->SetItemIcon(commandId, imagePath);
}

void EditorToolbar::Build(EditorApplication* app)
{
    // Build minimal menu structure (extend as needed)
    Clear();
    m_WindowPanelCommands.clear();

    // Edit — first menu becomes the macOS application menu (shown as "Open Engine Editor").
    const uint32_t editMenu = AddMenu("Edit");
    {
        std::string undoTitle;
        std::string redoTitle;
        BuildUndoRedoTitles(app, undoTitle, redoTitle);
        AddItem(editMenu, undoTitle, kCmdEditUndo);
        SetItemIcon(kCmdEditUndo, EditorIcons::kUndo);
        AddItem(editMenu, redoTitle, kCmdEditRedo);
    }

    // Window — quick access to all editor panels plus the layout submenu.
    const uint32_t windowMenu = AddMenu("Window");
    if (m_PanelManager)
    {
        const auto panelEntries = m_PanelManager->GetPanelMenuEntries();
        const std::size_t panelLimit = static_cast<std::size_t>(
            kWindowPanelCommandLast - kWindowPanelCommandFirst + 1);
        for (std::size_t i = 0; i < std::min(panelEntries.size(), panelLimit); ++i)
        {
            const auto& entry = panelEntries[i];
            const uint32_t commandId = kWindowPanelCommandFirst + static_cast<uint32_t>(i);
            AddItem(windowMenu, entry.title, commandId);
            SetItemIcon(commandId, entry.iconPath);
            m_WindowPanelCommands.emplace(commandId, entry.panelId);
        }
    }
    const uint32_t layoutMenu = AddSubMenu(windowMenu, "Layout");
    AddItem(layoutMenu, "Reset Layout", kCmdWindowLayoutReset);
    SetItemIcon(kCmdWindowLayoutReset, EditorIcons::kReset);

    // Registry-driven menus: native module items (EditorMenuRegistry — engine
    // packages) and script items (Editor-managed snapshot) walk the same
    // path->submenu machinery, seeded with the built-in menus so both extend
    // them without creating duplicates. Native items come first — package
    // affordances sit above script-contributed ones within a menu. Top-level
    // menus with no built-in items (Tools) are created on demand by the walk,
    // so an editor with no contributors shows no dangling empty menu.
    {
        const auto nativeItems = Editor::EditorMenuRegistry::Get().Snapshot(); // pre-sorted
        auto scriptItems = Editor::ScriptMenuRegistry::Get().GetToolbarItems();
        if (!nativeItems.empty() || !scriptItems.empty())
        {
            std::sort(scriptItems.begin(), scriptItems.end(),
                      [](const Editor::ScriptToolbarMenuItem& a, const Editor::ScriptToolbarMenuItem& b)
                      {
                          if (a.priority != b.priority)
                              return a.priority < b.priority;
                          return a.path < b.path;
                      });

            // Cache menu ids by prefix path (e.g. "Tools" or "Tools/Sub").
            std::unordered_map<std::string, uint32_t> prefixToMenuId;
            // Seed with built-in menus so contributors extend them without creating duplicates.
            prefixToMenuId["Edit"] = editMenu;
            prefixToMenuId["Window"] = windowMenu;
            prefixToMenuId["Window/Layout"] = layoutMenu;

            auto splitPath = [](const std::string& path) -> std::vector<std::string>
            {
                std::vector<std::string> parts;
                std::string cur;
                for (char c : path)
                {
                    if (c == '/')
                    {
                        if (!cur.empty())
                        {
                            parts.push_back(cur);
                            cur.clear();
                        }
                    }
                    else
                    {
                        cur.push_back(c);
                    }
                }
                if (!cur.empty())
                    parts.push_back(cur);
                return parts;
            };

            auto addPathItem = [&](const std::string& path, uint32_t commandId)
            {
                auto parts = splitPath(path);
                if (parts.size() < 2)
                {
                    // Toolbar items require at least "Menu/Item".
                    return;
                }

                std::string prefix;
                uint32_t parentMenuId = 0;

                for (size_t i = 0; i < parts.size(); ++i)
                {
                    const std::string& segment = parts[i];
                    if (segment.empty())
                        continue;
                    if (!prefix.empty())
                        prefix += '/';
                    prefix += segment;
                    const bool isLeaf = (i + 1 == parts.size());

                    if (!isLeaf)
                    {
                        auto found = prefixToMenuId.find(prefix);
                        if (found == prefixToMenuId.end())
                        {
                            const uint32_t menuId = (parentMenuId == 0)
                                                        ? AddMenu(segment)
                                                        : AddSubMenu(parentMenuId, segment);
                            prefixToMenuId.emplace(prefix, menuId);
                            parentMenuId = menuId;
                        }
                        else
                        {
                            parentMenuId = found->second;
                        }
                    }
                    else
                    {
                        if (parentMenuId != 0)
                        {
                            AddItem(parentMenuId, segment, commandId);
                        }
                    }
                }
            };

            for (const auto& it : nativeItems)
                addPathItem(it.Path, it.CommandId);
            for (const auto& it : scriptItems)
            {
                if (it.path.empty() || it.commandId == 0)
                    continue;
                addPathItem(it.path, it.commandId);
            }
        }
    }
}

} // namespace GameEngine
