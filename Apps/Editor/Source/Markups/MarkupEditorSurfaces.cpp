#include "Markups/MarkupEditorSurfaces.h"

#include "Core/Engine.h"
#include "Editor/Registries/DebugRequestGateRegistry.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupInspector.h"
#include "Markups/MarkupKind.h"
#include "Markups/MarkupLabelOverlay.h"
#include "Markups/MarkupRequestGate.h"
#include "Markups/MarkupTool.h"
#include "Markups/MarkupsPanel.h"
#include "SceneView/SceneViewToolStripRegistry.h"
#include "SceneView/SplineOwnerQuery.h"
#include "UI/EditorIcons.h"
#include "UI/UIElement.h"
#include "UI/ViewOverlayHost.h"

#include <chrono>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

int64 SystemUnixSeconds()
{
    return static_cast<int64>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

void RegisterMarkupsPanel(MarkupEditorBridge& bridge)
{
    EditorPanelDescriptor descriptor;
    descriptor.PanelId = MarkupsPanel::kPanelId;
    descriptor.Title = "Mark-ups";
    descriptor.Factory = [&bridge]() -> std::unique_ptr<UIElement> {
        return std::make_unique<MarkupsPanel>(bridge);
    };
    EditorPanelRegistry::Get().RegisterPanel(std::move(descriptor));
    // Attached at the UI root: the inspector section lives outside the panel.
    EditorPanelRegistry::Get().RegisterEditorStyleSheet({"editor", "UI/panels/MarkupsPanel.css"});
}

void RegisterMarkupToolStripEntry(MarkupEditorBridge& bridge)
{
    SceneViewToolStripEntry entry;
    entry.Id = "markups";
    entry.Tooltip = "Mark-up: click the world to mark a place for the agent; right-click for the shape (box, "
                    "sphere, or a region you draw)";
    entry.Icon = EditorIcons::kMarkup;
    entry.ContextMenuItems = &BuildMarkupToolMenuItems;
    entry.CreateTool = [&bridge](SceneViewController& owner) -> std::unique_ptr<SceneTools::ISceneTool> {
        return std::make_unique<MarkupTool>(owner, bridge);
    };
    entry.BadgeCount = [&bridge]() -> std::size_t {
        ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
        return world ? bridge.CountUnseen(*world) : 0;
    };
    entry.BadgeButtonTooltip = "Activity: what changed in the mark-ups";
    entry.BadgeButtonIcon = EditorIcons::kActivity;
    entry.OnBadgeButton = [](UIElement&) {
        EditorPanelRegistry::Get().OpenPanel(MarkupsPanel::kPanelId);
        if (MarkupsPanel* panel = MarkupsPanel::TryGetOpen())
            panel->ShowTab(MarkupsPanel::Tab::Activity);
    };
    SceneViewToolStripRegistry::Get().Register(std::move(entry));
}

} // namespace

std::unique_ptr<MarkupEditorBridge> CreateMarkupEditorBridge(EditorChangeNotifications& notifications,
                                                             MarkupEditorBridge::ScenePath scenePath)
{
    auto bridge = std::make_unique<MarkupEditorBridge>(notifications, &SystemUnixSeconds, std::move(scenePath));
    MarkupEditorBridge::Install(bridge.get());
    RegisterMarkupRequestGate(DebugRequestGateRegistry::Get());
    RegisterMarkupToolStripEntry(*bridge);
    RegisterMarkupsPanel(*bridge);
    RegisterMarkupInspector(*bridge);
    SetSplineOwnerQuery(&QueryMarkupSpline);
    ViewOverlayHost::Get().Register(std::make_unique<MarkupLabelOverlay>());
    return bridge;
}

} // namespace GameEngine::Editor
