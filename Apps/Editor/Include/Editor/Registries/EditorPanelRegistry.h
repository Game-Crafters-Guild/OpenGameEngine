#pragma once

#include "ECS/ModuleRegistration.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::Editor
{

// What a package module registers to add a dockable panel to the editor —
// the module-facing mirror of the layout.uxml <DockablePanel> inventory row,
// with the editor-internal type key replaced by a factory. Consumed panels
// enter the same DockingManager id-space as built-in panels, so activation,
// tab drag/float, close, and saved layouts need no package-specific handling.
struct EditorPanelDescriptor
{
    std::string PanelId; // dock id, unique across the editor (e.g. "EZTreeStats")
    std::string Title;   // tab title; empty falls back to PanelId
    // CSS class for the tab icon; empty leaves the panel type's own
    // DeclaredTabIconClass() in charge rather than forcing no icon.
    std::string TabIconClass;
    // Panel content assets, resolved against AssetSourceAlias (typically the
    // registering package's mount). All empty = the factory builds its UI in
    // code. AssetBoundDockPanel is the matching panel-side consumer.
    std::string AssetSourceAlias;
    std::string LayoutAssetPath; // e.g. "Editor/UI/panels/EZTreeStatsPanel.uxml"
    std::string StyleAssetPath;  // e.g. "Editor/UI/panels/EZTreeStatsPanel.css"
    // Existing panel whose dock leaf receives this panel the first time it is
    // opened. Empty keeps the editor's generic package-panel placement.
    std::string DefaultDockAnchorPanelId;
    // Creates the panel root — a DockPanel subclass, so the dock tab picks up
    // title/icon and close/drag behave like built-in panels.
    std::function<std::unique_ptr<UIElement>()> Factory;
};

// A panel TYPE a dock-config inventory row names (layout.uxml
// <DockablePanel type="...">): the editor builds every row of that type with
// this factory. The editor's own panel types register at startup
// (Panels/BuiltInPanelTypes); a package may register more, so its panels can
// appear in an authored layout like built-in ones.
struct EditorPanelTypeDescriptor
{
    std::string TypeKey; // e.g. "HierarchyPanel"
    std::function<std::unique_ptr<UIElement>()> Factory;
};

// A stylesheet a package contributes to the editor chrome, attached at the UI
// root rather than a panel subtree: tab icons, hierarchy-row classes, and
// inspector icon classes style elements that live outside any panel. Global
// stylesheets append after the editor theme, so package rules extend it
// (the CSS cascade here is file order).
struct EditorStyleSheetContribution
{
    std::string AssetSourceAlias;
    std::string StyleAssetPath; // e.g. "Editor/UI/EZTreeEditorChrome.css"
};

// A full-screen element a package contributes to the editor's main UI ROOT
// rather than the dock (import modals, wizard overlays). The editor attaches
// the factory's element to the root once per id; the element owns its own
// show/hide, and the module keeps whatever pointer its factory captured to
// drive it (the UI tree owns the element's lifetime).
struct EditorOverlayDescriptor
{
    std::string OverlayId; // unique across the editor (e.g. "unityImport.modal")
    std::function<std::unique_ptr<UIElement>()> Factory;
};

// Registration is main-thread (module loads + editor startup), matching the
// other editor registries. Module lifetime is loud no-unload: a module
// rebuild re-registers under the same id and the NEW descriptor wins
// (replace-forward); nothing is ever unregistered.
class EditorPanelRegistry
{
public:
    static EditorPanelRegistry& Get();

    void RegisterPanel(EditorPanelDescriptor descriptor);
    void RegisterEditorStyleSheet(EditorStyleSheetContribution contribution);
    void RegisterOverlay(EditorOverlayDescriptor descriptor);
    void RegisterPanelType(EditorPanelTypeDescriptor descriptor);

    bool TryGetPanel(std::string_view panelId, EditorPanelDescriptor& outDescriptor) const;
    std::vector<EditorPanelDescriptor> PanelSnapshot() const;
    std::vector<EditorStyleSheetContribution> StyleSheetSnapshot() const;
    std::vector<EditorOverlayDescriptor> OverlaySnapshot() const;

    // Builds a panel of a registered type; null when no type is registered
    // under typeKey.
    std::unique_ptr<UIElement> CreatePanelOfType(std::string_view typeKey) const;

    // The editor attaches consumers once its docking model + UI exist.
    // Entries registered before that (test hosts) are replayed on attach;
    // later ones fire immediately (the editor order: packages load at
    // project open, long after startup).
    struct Consumers
    {
        std::function<void(const EditorPanelDescriptor&)> Panel;
        std::function<void(const EditorStyleSheetContribution&)> StyleSheet;
        std::function<void(const EditorOverlayDescriptor&)> Overlay;
    };
    void SetConsumers(Consumers consumers);

    // Editor-installed service that shows/activates a registered panel in the
    // dock. Modules call OpenPanel from their own affordances (context menus,
    // inspector buttons); without an installed opener it is a loud no-op.
    using PanelOpener = std::function<void(const std::string& panelId)>;
    void SetPanelOpener(PanelOpener opener);
    void OpenPanel(std::string_view panelId) const;

    // C12 editor-kind unload refusal diagnostics: append a description of
    // every entry attributed to `moduleId` (registrations are stamped from the
    // loader's active-module bracket, ECS/ModuleRegistration.h). These entries
    // hold module code (panel factories) and pin the module's images mapped.
    void AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const;

private:
    EditorPanelRegistry() = default;

    // Registration + the module stamp active when it was made (attribution is
    // registry bookkeeping — not part of the module-facing descriptor).
    struct PanelEntry
    {
        EditorPanelDescriptor Descriptor;
        ECS::ModuleRegistrationStamp Module;
    };
    struct StyleSheetEntry
    {
        EditorStyleSheetContribution Contribution;
        ECS::ModuleRegistrationStamp Module;
    };
    struct OverlayEntry
    {
        EditorOverlayDescriptor Descriptor;
        ECS::ModuleRegistrationStamp Module;
    };
    struct PanelTypeEntry
    {
        EditorPanelTypeDescriptor Descriptor;
        ECS::ModuleRegistrationStamp Module;
    };

    std::vector<PanelEntry> m_Panels;
    std::vector<StyleSheetEntry> m_StyleSheets;
    std::vector<OverlayEntry> m_Overlays;
    std::vector<PanelTypeEntry> m_PanelTypes;
    Consumers m_Consumers;
    PanelOpener m_PanelOpener;
};

} // namespace GameEngine::Editor
