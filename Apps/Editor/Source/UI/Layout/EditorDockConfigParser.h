#pragma once

#include "UI/Layout/Docking.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{

class UIElement;

namespace EditorUI
{

struct EditorDockPanelDef
{
    std::string id;     // Docking panel id (e.g. "SceneView")
    std::string type;   // C++ panel type key (e.g. "SceneViewPanel")
    std::string title;  // Optional display title

    // Optional per-panel UI bindings (relative to Editor UI directory).
    // Example: "panels/SceneViewPanel.uxml" / "panels/SceneViewPanel.css"
    std::string layout;
    std::string style;

    // Per-instance CSS icon class for the panel tab, as authored by icon=.
    // Disengaged = no icon= attribute, so the type's DeclaredTabIconClass()
    // applies. Engaged and empty = icon="", meaning this panel shows no icon
    // even though its type declares one.
    std::optional<std::string> icon;

    // menu="false": keep the panel registered and openable by id, but omit it
    // from user-facing panel listings (Window menu, hamburger, search).
    bool showInMenu = true;
};

struct EditorDockLayoutDef
{
    std::unique_ptr<DockNode> root;
};

struct EditorDockConfig
{
    std::vector<EditorDockPanelDef> panels;
    EditorDockLayoutDef defaultLayout;
};

// Parse dock configuration metadata embedded under a DockspaceElement in the loaded UI tree.
// Returns true on success. On failure, out is left empty and the caller should fall back.
bool TryParseEditorDockConfigFromDockspace(UIElement* dockspace, EditorDockConfig& out);

} // namespace EditorUI

} // namespace GameEngine


