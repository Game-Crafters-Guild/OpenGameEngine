#include "Panels/SceneViewPanel.h"
#include "Panels/SceneCameraSettingsPopup.h"
#include "SceneViewController.h"
#include "SceneView/SceneViewEvents.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/Label.h"
#include "UI/Interaction/ContextMenuManipulator.h"
#include "UI/EditorIcons.h"
#include "UI/StyleProperties.h"
#include "Core/Engine.h"
#include "Editor/Assets/AssetRelativePath.h"
#include "Core/Application.h"
#include "Core/CpuProfiler.h"
#include "Assets/AssetCreation.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/AssetTypes.h"

#include "EditorContext.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "Editor/Entities/EntityDuplicate.h"
#include "Editor/Hierarchy/HierarchyOrdering.h"
#include "Editor/Settings/FbxImportSettings.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Platform/ContextMenu.h"
#include "Platform/Window.h"
#include "Scripting/EditorScriptMenuRegistry.h"
#include "UI/Controls/SceneViewToolbar.h"
#include "SceneView/TransformTool.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Widgets/CameraBookmarksWidget.h"
#include "UI/Controls/Widgets/SceneViewMeasureOverlay.h"
#include "UI/Controls/Widgets/SceneViewRulerOverlay.h"
#include "UI/Controls/Widgets/ViewportRotationGizmo.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "Thumbnails/IThumbnailProvider.h"

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"

#include "UndoRedo/DeleteEntitiesCommand.h"
#include "UndoRedo/DeleteEntitiesSelectionCommand.h"
#include "UndoRedo/DuplicateEntitiesCommand.h"
#include "UndoRedo/MaterialDropOnMeshCommand.h"
#include "UndoRedo/TextureDropOnMeshCommand.h"
#include "UndoRedo/UndoRedoService.h"
#include "EditorChangeNotifications.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/LensFlareSource.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Editor/DragDropPayloads.h"
#include "Editor/Assets/AsyncAssetHelpers.h"
#include "Editor/Entities/EntityMaterialTextureAssign.h"
#include "Editor/Entities/SpriteEntityFactory.h"
#include "SceneView/SceneViewDropPlacement.h"
#include "SceneView/SceneViewNoticeOverlay.h"
#include "SceneView/SceneViewToolStrip.h"
#include "SceneView/SceneViewToolStripRegistry.h"
#include "Assets/DownloadPillOverlay.h"
#include "UI/ViewOverlayHost.h"
#include "Editor/Entities/SkyboxEntityFactory.h"
#include "Assets/ModelAsset.h"
#include "Assets/PolyhavenDownloadManager.h"
#include "Assets/PolyhavenPlaceholderFactory.h"
#include "Assets/PolyhavenService.h"
#include "AssetCore/AssetTypes.h"
#include "Components/Hierarchy.h"
#include "Components/Measure/MeasureComponent.h"
#include "Components/Name.h"
#include "Components/SceneBlueprintInstance.h"
#include "Components/Transform.h"
#include "ECS/Components.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/PostProcessEffectRegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Mathematics/Types.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Interaction/Payload.h"
#include "UI/Interaction/Types.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include "Logger/Logger.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
void ClampPointerToViewport(float& localX, float& localY, float viewW, float viewH)
{
    localX = std::clamp(localX, 0.0f, std::max(0.0f, viewW));
    localY = std::clamp(localY, 0.0f, std::max(0.0f, viewH));
}

// The built-in buttons' icons; the registered entries' icons join them in the reveal gate.
constexpr std::array<std::string_view, 5> kSceneToolOverlayIconPaths = {
    "Icons/arrowmouse.png",
    "Icons/GiszmoTranslate.png",
    "Icons/GizmoRotate.png",
    "Icons/Giszmo_scale.png",
    "Icons/Ruler.png",
};

// Window-space anchor for a scene tool button's context menu. A vertical row
// (the floating overlay in its default orientation) opens the menu to the
// button's right; a horizontal row — the docked toolbar mirror, or the overlay
// after it has been dragged into its horizontal orientation — drops it below.
Mathematics::Vector2 ToolButtonMenuAnchor(const UIElement& button)
{
    const UIElement* row = button.GetParent();
    const bool horizontal = row && (row->HasClass("inline-tool-buttons") || row->HasClass("horizontal"));
    if (horizontal)
        return Mathematics::Vector2(button.GetLayoutX(),
                                    button.GetLayoutY() + button.GetLayoutHeight());
    return Mathematics::Vector2(button.GetLayoutX() + button.GetLayoutWidth(),
                                button.GetLayoutY());
}

void SetEditorBackgroundPath(UIElement& element, std::string_view path);

// The editor asset path of an icon as EditorIcons names one ("editor:Icons/x.png").
std::string_view EditorIconAssetPath(std::string_view icon)
{
    constexpr std::string_view kEditorAliasPrefix = "editor:";
    if (icon.starts_with(kEditorAliasPrefix))
        icon.remove_prefix(kEditorAliasPrefix.size());
    return icon;
}

// An editor icon as EditorIcons names one as the button's background.
void SetEditorIcon(UIElement& element, std::string_view icon)
{
    SetEditorBackgroundPath(element, EditorIconAssetPath(icon));
}

void SetEditorBackgroundPath(UIElement& element, std::string_view path)
{
    BackgroundImageSource source{};
    source.Kind = BackgroundImageSource::SourceKind::Path;
    source.Value = std::string(path);
    source.SourceAlias = std::string(GameEngine::kAssetSourceAliasEditor);

    element.Overrides()
        .Set(Style::BackgroundImage, std::move(source))
        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
        .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Explicit, 16.0f, false, 16.0f, false})
        .Set(Style::BackgroundPosition, BackgroundPositionValue{50.0f, true, 50.0f, true})
        .Reset(Style::BackgroundTint);
    element.MarkDirty(UIElement::VisualDirty);
}

std::array<float, 3> MeasurePointFromPointer(SceneViewController* controller,
                                             const Editor::SceneTools::ScenePointerEvent& ev)
{
    std::array<float, 3> out{ev.ray.origin.x, ev.ray.origin.y, ev.ray.origin.z};
    if (!controller || controller->Is2DMode() || controller->IsOrthographic())
        return out;

    const auto pose = controller->GetCameraPose();
    const Mathematics::Vector3 planePoint = ev.cameraPos + ev.cameraForward * pose.Distance;

    float t = 0.0f;
    Mathematics::Vector3 hit;
    if (Mathematics::IntersectRayPlane(ev.ray, planePoint, ev.cameraForward, t, hit))
        out = {hit.x, hit.y, hit.z};
    return out;
}

static std::string FormatHoverHighlightPillText(ECS::World& world,
                                                 ECS::EntityHandle root,
                                                 const std::vector<ECS::EntityHandle>& descendants)
{
    if (!root.IsValid() || !world.IsValid(root))
        return {};
    std::string primary = Editor::EntityDisplayName(world, root);
    if (descendants.empty())
        return primary;
    return primary + " · +" + std::to_string(descendants.size());
}

// Matches the default `SceneViewFpsLabel` bottom anchor (`RegisterViewportEvents`).
constexpr float kSceneViewHudBottomInsetPx = 14.0f;

// Scene view toolbar icon visibility context-menu toggles.
struct SceneToolbarIconToggle
{
    const char* id;      // element id inside the toolbar
    const char* label;   // user-visible menu label
    const char* prefKey; // editor preferences key
};

static const SceneToolbarIconToggle kSceneToolbarIconToggles[] = {
    {"CameraBookmarks",     "Camera Bookmarks",      "sceneView.toolbar.show.cameraBookmarks"},
    {"GridToggle",          "Grid",                  "sceneView.toolbar.show.grid"},
    {"SnapToggle",          "Snap",                  "sceneView.toolbar.show.snap"},
    {"GizmoToggle",         "Gizmos",                "sceneView.toolbar.show.gizmo"},
    {"PickModeToggle",      "Pick Mode",             "sceneView.toolbar.show.pickMode"},
    {"ToolSpaceToggle",     "Transform Space",       "sceneView.toolbar.show.toolSpace"},
    {"View2DToggle",        "2D / 3D",               "sceneView.toolbar.show.view2d"},
    {"QuadViewToggle",      "Four View Layout",      "sceneView.toolbar.show.quadView"},
    {"RotationGizmoToggle", "Rotation Gizmo",        "sceneView.toolbar.show.rotationGizmo"},
    {"ToolOverlayToggle",   "Tool Overlay Toggle",   "sceneView.toolbar.show.toolOverlay"},
};

constexpr uint32_t kCmdSceneToolbarToggleBase = 0x5301;
constexpr uint32_t kCmdSceneToolbarFpsToggle  = 0x5401;
constexpr uint32_t kCmdSceneFpsToggleVsync    = 0x5402;

bool IsModelFileExtension(const std::string& ext)
{
    return ext == ".glb" || ext == ".gltf" || ext == ".obj" || ext == ".fbx" || ext == ".GLB" || ext == ".GLTF" || ext == ".OBJ"
           || ext == ".FBX";
}

void ApplyRootWorldPosition(ECS::World& world, ECS::EntityHandle root, const Mathematics::Vector3& worldPos)
{
    using namespace Components;
    if (auto* xf = world.GetComponentForWrite<Transform>(root))
    {
        xf->matrix[12] = worldPos.x;
        xf->matrix[13] = worldPos.y;
        xf->matrix[14] = worldPos.z;
    }
}

class CreateMeasureEntitiesCommand final : public Editor::IEditorCommand
{
public:
    CreateMeasureEntitiesCommand(ECS::World* world,
                                 Editor::EditorChangeNotifications* notifications,
                                 std::vector<ECS::EntityHandle> entities)
        : m_World(world)
        , m_Notifications(notifications)
        , m_Entities(std::move(entities))
    {
        CaptureSnapshots();
    }

    const char* GetName() const override { return "Create Measure"; }
    void Do() override { Redo(); }

    void Undo() override
    {
        if (!m_World)
            return;
        for (std::size_t i = m_Snapshots.size(); i-- > 0;)
        {
            if (m_Snapshots[i].Entity.IsValid())
                m_World->DestroyEntityImmediatePreserveHandle(m_Snapshots[i].Entity);
        }
        NotifyWorldStructure(m_Notifications, m_World);
    }

    void Redo() override
    {
        if (!m_World)
            return;
        for (const EntitySnapshot& snap : m_Snapshots)
        {
            if (!snap.Entity.IsValid())
                continue;
            if (!m_World->IsValid(snap.Entity))
                (void)m_World->ReviveEntityImmediatePreserveHandle(snap.Entity);
            if (!m_World->IsValid(snap.Entity))
                continue;
            for (const auto& [typeId, bytes] : snap.Components)
                (void)m_World->ApplyComponentBytesImmediate(snap.Entity, typeId, bytes);
        }
        NotifyWorldStructure(m_Notifications, m_World);
    }

private:
    struct EntitySnapshot
    {
        ECS::EntityHandle Entity{};
        std::vector<std::pair<ECS::ComponentTypeId, std::vector<std::uint8_t>>> Components;
    };

    void CaptureSnapshots()
    {
        m_Snapshots.clear();
        if (!m_World)
            return;

        for (ECS::EntityHandle entity : m_Entities)
        {
            if (!entity.IsValid() || !m_World->IsValid(entity))
                continue;

            ECS::Archetype* archetype = m_World->GetEntityArchetype(entity);
            if (!archetype)
                continue;

            EntitySnapshot snap{};
            snap.Entity = entity;
            const auto typeIds = archetype->GetSignature().GetComponents();
            snap.Components.reserve(typeIds.size());
            for (ECS::ComponentTypeId typeId : typeIds)
            {
                std::vector<std::uint8_t> bytes;
                if (m_World->CaptureComponentBytes(entity, typeId, bytes))
                    snap.Components.emplace_back(typeId, std::move(bytes));
            }
            m_Snapshots.push_back(std::move(snap));
        }
    }

    ECS::World* m_World = nullptr;
    Editor::EditorChangeNotifications* m_Notifications = nullptr;
    std::vector<ECS::EntityHandle> m_Entities;
    std::vector<EntitySnapshot> m_Snapshots;
};

// Collects every entity belonging to the active drag-drop preview (root,
// submeshes, billboard) so the picking ray can skip them and avoid the
// preview self-intersecting and walking toward the camera each frame.
std::vector<ECS::EntityHandle> GatherDropPreviewEntities(
    const std::optional<Engine::Renderer::ModelEntityResult>& previewModel,
    ECS::EntityHandle previewBillboard)
{
    std::vector<ECS::EntityHandle> ignore;
    if (previewModel.has_value())
    {
        ignore.reserve(previewModel->submeshEntities.size() + 1);
        if (previewModel->rootEntity.IsValid())
            ignore.push_back(previewModel->rootEntity);
        for (ECS::EntityHandle h : previewModel->submeshEntities)
            ignore.push_back(h);
    }
    if (previewBillboard.IsValid())
        ignore.push_back(previewBillboard);
    return ignore;
}

void DestroyModelHierarchy(ECS::World& world, const Engine::Renderer::ModelEntityResult& r)
{
    if (!r.IsValid())
        return;
    // Submeshes first, then the root. A single-submesh model's root is one of
    // its submeshes; the set destroy ignores the duplicate.
    std::vector<ECS::EntityHandle> doomed = r.submeshEntities;
    doomed.push_back(r.rootEntity);
    world.DestroyEntitiesImmediate(doomed);
}

} // namespace

void SceneViewPanel::OnDockTabActivationArmed(float /*contentWidth*/, float /*contentHeight*/)
{
    for (SceneViewController* controller : m_SceneControllers)
    {
        if (controller)
            controller->PrepareForActivation();
    }
}

SceneViewPanel::SceneViewPanel()
    : DockPanel("Scene View")
{
    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    prefs.TryGetBool("sceneView.measure.createEntities", m_MeasureCreateEntitiesMode);
    std::string units;
    if (prefs.TryGetString("sceneView.measure.units", units) &&
        (units == "imperial" || units == "inches"))
    {
        m_MeasureUnitSystem = SceneViewMeasureOverlay::UnitSystem::Imperial;
    }
    std::string twoDMode;
    if (prefs.TryGetString("sceneView.measure.2dMode", twoDMode) &&
        (twoDMode == "points" || twoDMode == "twoPoints" || twoDMode == "2points"))
    {
        m_MeasureTwoDMode = SceneViewMeasureOverlay::TwoDMode::Points;
    }
}

SceneViewPanel::~SceneViewPanel()
{
    // The pending asset callbacks capture this panel; withdraw them before it goes
    // away (destroying an AssetLoadHandle does not).
    if (m_LayoutLoadHandle)
        m_LayoutLoadHandle->Cancel();
    if (m_ThemeStyleLoadHandle)
        m_ThemeStyleLoadHandle->Cancel();
    if (m_PanelStyleLoadHandle)
        m_PanelStyleLoadHandle->Cancel();

    ClearDropPreviewModel();
}

void SceneViewPanel::SetChangeNotifications(Editor::EditorChangeNotifications* notifications)
{
    m_ChangeNotifications = notifications;
}

void SceneViewPanel::SetSceneController(SceneViewController* controller)
{
    SetSceneControllerForSlot(ViewportSlot::Perspective, controller);
}

bool SceneViewPanel::HandleSceneViewCommandShortcut(SceneViewController* controller, UIEvent& ev)
{
    if (!controller)
        return false;

    // T: toggle pick mode (exact entity vs model-instance root). Pick mode is
    // a global scene-view setting, so it lives in the shared shortcut path and
    // works from perspective and quad viewports alike.
    if (ev.Mods == 0 && ev.Key == Input::kKeyCode_T)
    {
        auto& settings = Editor::SceneViewSettings::Get();
        settings.SetExactPickMode(!settings.GetExactPickMode());
        if (m_Toolbar)
            m_Toolbar->UpdatePickModeButtonState();
        ev.Stop();
        return true;
    }

    // Delete/Backspace while the active tool holds a selection of its own (spline
    // knots): the keys belong to that selection, not the entity that owns it. The tool
    // reports whether it took the key, so a tool with nothing selected still falls
    // through to entity deletion below.
    if (ev.Mods == 0 && (ev.Key == Input::kKeyCode_Delete || ev.Key == Input::kKeyCode_Backspace))
    {
        Editor::SceneTools::ISceneTool* activeTool = controller->GetToolContext().GetActiveTool();
        if (activeTool && activeTool->DeleteSelection())
        {
            ev.Stop();
            return true;
        }
    }

    // Delete key: delete the selected entity as a single undo step.
    if (Editor::MatchesCatalogShortcut("Scene View", "Delete Entity", ev.Key, ev.Mods))
    {
        auto* xfTool = controller->GetTransformTool();
        const ECS::EntityHandle selected = xfTool ? xfTool->GetTargetEntity() : ECS::EntityHandle{};
        ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
        if (world && selected.IsValid() && world->IsValid(selected))
        {
            auto ents = Editor::DeleteEntitiesCommand::CollectSubtree(*world, selected);
            if (!ents.empty())
            {
                auto* undo = (m_Context && m_Context->UndoRedo) ? m_Context->UndoRedo : nullptr;
                const std::string undoName = "Delete Entity";
                if (undo)
                {
                    // Delete + deselect + (on undo) re-select the entity as one undo step, so a
                    // single Ctrl+Z brings it back AND restores the scene-view selection (redo
                    // re-deletes + deselects). OnEntityPicked syncs to the Hierarchy via
                    // ApplySelectionProgrammatic, which suppresses its own selection-undo.
                    const ECS::EntityHandle reselect = selected;
                    auto onDeleted = [controller]() { controller->OnEntityPicked({}); };
                    auto onRevived = [controller, reselect]() { controller->OnEntityPicked(reselect); };
                    undo->Execute(std::make_unique<Editor::DeleteEntitiesSelectionCommand>(
                        undoName, world, m_ChangeNotifications, std::move(ents),
                        std::move(onDeleted), std::move(onRevived)));
                }
                else
                {
                    Editor::DeleteEntitiesCommand cmdObj(
                        undoName, world, m_ChangeNotifications, std::move(ents));
                    cmdObj.Redo();
                    controller->OnEntityPicked({});
                }
                ev.Stop();
                return true;
            }
        }
        return false;
    }

    // Cmd/Ctrl+D: duplicate selected entities.
    if (Editor::MatchesCatalogShortcut("Scene View", "Duplicate", ev.Key, ev.Mods))
    {
        SceneViewController* selectionController = controller;
        if (!selectionController || selectionController->GetSelectedEntities().empty())
        {
            if (m_Controller && !m_Controller->GetSelectedEntities().empty())
                selectionController = m_Controller;
        }
        if (!selectionController || selectionController->GetSelectedEntities().empty())
        {
            for (SceneViewController* slotController : m_SceneControllers)
            {
                if (slotController && !slotController->GetSelectedEntities().empty())
                {
                    selectionController = slotController;
                    break;
                }
            }
        }

        const auto& selected = selectionController ? selectionController->GetSelectedEntities()
                                                   : controller->GetSelectedEntities();
        ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
        if (world && !selected.empty())
        {
            // Copy: `selected` aliases the controller's live selection, which the
            // post-duplicate re-select below replaces.
            const std::vector<ECS::EntityHandle> selectionBefore = selected;
            const Editor::EntityDuplicateResult duplicate = Editor::DuplicateEntitySubtreeRoots(*world, selectionBefore);
            const std::vector<ECS::EntityHandle>& newRoots = duplicate.NewRoots;

            SceneViewController* notifyController = controller ? controller : selectionController;

            auto* undo = (m_Context && m_Context->UndoRedo) ? m_Context->UndoRedo : nullptr;
            if (undo && !newRoots.empty() && notifyController)
            {
                // NewRoots only lists the duplicated top roots; the command must own
                // the clones' full subtrees so undo removes every cloned entity.
                std::vector<ECS::EntityHandle> clones;
                clones.reserve(newRoots.size() * 4);
                for (const auto& nr : newRoots)
                {
                    auto subtree = Editor::DeleteEntitiesCommand::CollectSubtree(*world, nr);
                    clones.insert(clones.end(), subtree.begin(), subtree.end());
                }

                // Redo re-selects the clones; Undo restores the pre-duplicate
                // selection — both as part of THIS undo entry. Selection routes
                // through the controller like the delete path: it fires the normal
                // callbacks that sync hierarchy and inspector, and the hierarchy
                // side suppresses its own selection-undo.
                auto selectViaController = [notifyController](const std::vector<ECS::EntityHandle>& ents)
                {
                    if (ents.empty())
                        notifyController->OnEntityPicked({});
                    else if (ents.size() == 1)
                        notifyController->OnEntityPicked(ents.front());
                    else
                        notifyController->OnEntitiesMarqueeSelected(ents, false);
                };
                std::vector<ECS::EntityHandle> cloneRoots = newRoots;
                auto onClonesRestored = [selectViaController, cloneRoots]()
                { selectViaController(cloneRoots); };
                auto onClonesRemoved = [selectViaController, selectionBefore]()
                { selectViaController(selectionBefore); };

                const std::string undoName = (newRoots.size() > 1) ? "Duplicate Entities" : "Duplicate Entity";
                undo->CommitAlreadyApplied(std::make_unique<Editor::DuplicateEntitiesCommand>(
                    undoName, world, m_ChangeNotifications, std::move(clones),
                    std::move(onClonesRestored), std::move(onClonesRemoved)));
            }

            NotifyWorldStructure(m_ChangeNotifications, world);
            if (m_Context && m_Context->OnSceneDirty)
                m_Context->OnSceneDirty();

            // Match HierarchyPanel::DuplicateSelectedEntities: select the new
            // roots through the scene controller that received the shortcut.
            // That controller fires the normal callbacks, which sync hierarchy,
            // inspector, and every split viewport from one authoritative source.
            if (!newRoots.empty() && notifyController)
            {
                if (newRoots.size() == 1)
                    notifyController->OnEntityPicked(newRoots.front());
                else
                    notifyController->OnEntitiesMarqueeSelected(newRoots, false);
            }
        }
        ev.Stop();
        return true;
    }

    return false;
}

void SceneViewPanel::SetContext(const EditorContext* ctx)
{
    m_Context = ctx;
    m_Window = ctx ? ctx->MainWindow : nullptr;
}

UIElement* SceneViewPanel::GetFpsLabelElement() const
{
    return m_FpsLabel;
}

Engine::Renderer::RenderServices* SceneViewPanel::ResolveRenderServices() const
{
    if (m_RenderServices)
        return m_RenderServices;
    if (m_Controller)
    {
        if (Engine::Renderer::RenderServices* rs = m_Controller->GetRenderServices())
            return rs;
    }
    if (m_Context && m_Context->RenderServices)
        return m_Context->RenderServices;
    return EngineCore::GetInstance().GetRenderServices();
}

void SceneViewPanel::ApplyLookDelta(float deltaYawDeg, float deltaPitchDeg)
{
    // lock anyone from moving camera while teleporting to a CameraBookmark.
    if (m_Controller->IsTweenActive())
        return;

    // 2D and fixed-axis views use pan/zoom controls rather than tumble/look.
    if (m_Controller->UsesFixedViewPanControls())
        return;

    m_Yaw   += deltaYawDeg;
	    // Invert pitch delta so that moving the mouse up pitches the camera up,
	    // matching typical editor expectations.
	    m_Pitch -= deltaPitchDeg;

    if (m_Pitch > 89.9f) m_Pitch = 89.9f;
    if (m_Pitch < -89.9f) m_Pitch = -89.9f;
}

void SceneViewPanel::ClearPanShift2DConstraint()
{
    m_PanShift2DWasHeld      = false;
    m_Pan2DShiftAxisLock     = Pan2DShiftAxisLock::None;
    m_Pan2DShiftCumDx        = 0.0f;
    m_Pan2DShiftCumDy        = 0.0f;
}

void SceneViewPanel::BeginCameraDragTracking(float x, float y)
{
    m_CameraDragLastMouseValid = true;
    m_CameraDragLastMouseX = x;
    m_CameraDragLastMouseY = y;
}

void SceneViewPanel::EndCameraDragTracking()
{
    m_CameraDragLastMouseValid = false;
    ClearPanShift2DConstraint();
}

bool SceneViewPanel::ApplyImmediatePan(SceneViewController* controller, UIElement* viewport, UIEvent& ev)
{
    if (!m_Dragging)
    {
        EndCameraDragTracking();
        return false;
    }

    if (!m_CameraDragLastMouseValid)
    {
        BeginCameraDragTracking(ev.X, ev.Y);
        if (m_IsPanning)
            ev.Stop();
        return m_IsPanning;
    }

    const float rawDeltaX = ev.X - m_CameraDragLastMouseX;
    const float rawDeltaY = ev.Y - m_CameraDragLastMouseY;
    m_CameraDragLastMouseX = ev.X;
    m_CameraDragLastMouseY = ev.Y;

    if (!m_IsPanning || !controller || !viewport)
    {
        if (!m_IsPanning)
            ClearPanShift2DConstraint();
        return false;
    }

    float panDeltaX = rawDeltaX;
    float panDeltaY = -rawDeltaY;
    const bool shiftAxisLock = (ev.Mods & Input::kModShift) != 0;
    ConstrainPanDelta2DShiftAxis(shiftAxisLock, panDeltaX, panDeltaY);

    if (panDeltaX != 0.0f || panDeltaY != 0.0f)
        controller->UpdatePan(panDeltaX, panDeltaY, viewport->GetLayoutHeight());

    ev.Stop();
    return true;
}

void SceneViewPanel::ConstrainPanDelta2DShiftAxis(bool shiftHeld, float& deltaX, float& deltaY)
{
    if (!shiftHeld || !m_Controller ||
        !(m_Controller->Is2DMode() || m_Controller->IsOrthographic()) || !IsPanning())
    {
        ClearPanShift2DConstraint();
        return;
    }

    const bool shiftBecameHeld = shiftHeld && !m_PanShift2DWasHeld;
    m_PanShift2DWasHeld        = shiftHeld;
    if (shiftBecameHeld)
    {
        m_Pan2DShiftAxisLock = Pan2DShiftAxisLock::None;
        m_Pan2DShiftCumDx    = 0.0f;
        m_Pan2DShiftCumDy    = 0.0f;
    }

    // Once horizontal/vertical locked, constrain each incremental frame delta.
    if (m_Pan2DShiftAxisLock == Pan2DShiftAxisLock::Horizontal)
    {
        deltaY = 0.0f;
        return;
    }
    if (m_Pan2DShiftAxisLock == Pan2DShiftAxisLock::Vertical)
    {
        deltaX = 0.0f;
        return;
    }

    // Pre-lock: radial dead zone on accumulated displacement; first exit picks axis
    // and applies the breakout motion along it (so small drift inside the radius is ignored).
    m_Pan2DShiftCumDx += deltaX;
    m_Pan2DShiftCumDy += deltaY;

    constexpr float kDeadZonePx = 4.0f;
    const float r2 =
        m_Pan2DShiftCumDx * m_Pan2DShiftCumDx + m_Pan2DShiftCumDy * m_Pan2DShiftCumDy;
    if (r2 < kDeadZonePx * kDeadZonePx)
    {
        deltaX = 0.0f;
        deltaY = 0.0f;
        return;
    }

    const float ax = std::fabs(m_Pan2DShiftCumDx);
    const float ay = std::fabs(m_Pan2DShiftCumDy);
    if (ax >= ay)
    {
        m_Pan2DShiftAxisLock = Pan2DShiftAxisLock::Horizontal;
        deltaX               = m_Pan2DShiftCumDx;
        deltaY               = 0.0f;
    }
    else
    {
        m_Pan2DShiftAxisLock = Pan2DShiftAxisLock::Vertical;
        deltaX               = 0.0f;
        deltaY               = m_Pan2DShiftCumDy;
    }
    m_Pan2DShiftCumDx = 0.0f;
    m_Pan2DShiftCumDy = 0.0f;
}

void SceneViewPanel::OnPostLayout()
{
    // Bind panel-local UXML/CSS once the panel is attached to a UIManager and has
    // entered the layout pipeline. Use a deferred action so we never mutate the
    // UI tree during layout/geometry traversal.
    if (!m_BindApplied && !m_BindPending && !m_BindFailed && GetOwnerManager())
    {
        m_BindPending = true;
        // A dropped action never runs, so it would never release the latch: release it
        // here instead and let the next laid-out frame try again.
        if (!this->PostAction([this]()
                              { this->BindFromAssetsDeferred(); }))
            m_BindPending = false;
    }

    // Dock splitters can relayout the Scene View continuously while the popup
    // remains open. Re-read the toolbar button geometry after layout completes;
    // doing this through PostAction avoids mutating style during the layout walk.
    if (m_CameraSettingsPopup && m_CameraSettingsPopup->IsVisible() &&
        !m_CameraSettingsPopupLayoutScheduled && GetOwnerManager())
    {
        m_CameraSettingsPopupLayoutScheduled = true;
        this->PostAction([this]()
                         {
                             m_CameraSettingsPopupLayoutScheduled = false;
                             RefreshCameraSettingsPopupLayout();
                         });
    }
}

void SceneViewPanel::BindFromAssetsDeferred()
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
    {
        // Detached between scheduling and draining: transient, so release the
        // latch and let the next laid-out frame schedule a fresh attempt.
        m_BindPending = false;
        return;
    }

    // m_BindPending stays set: LoadBindAttachLayoutAndStyle hands it to the async
    // load, whose completion is what resolves it.
    LoadBindAttachLayoutAndStyle(ui);

    RefreshElementPointers();

    RegisterViewportEvents();

    // INVARIANT: this call must wire NOTHING. The layout subtree is created by
    // BindLayoutToSubtreeChildrenFromAsset, which runs only inside the async load
    // callback below, so every FindById here resolves to null and every wiring guard
    // falls through. The second call, from that callback, is the one that wires.
    //
    // Clicks are additive handler-table subscriptions, so if a synchronous bind fast
    // path is ever added here (GameViewPanel has that shape: a cached GetAsset that
    // binds outright), BOTH calls would wire the same button instances and one click
    // would fire twice. Enforcement is that the bind is reachable only through
    // PostAction/async completion -- keep it that way, or make the second call
    // conditional on the first having wired nothing.
    BindToolbarWidgets();
    WireToolOverlay();
}

void SceneViewPanel::MarkBindFailed(std::string_view reason)
{
    m_BindPending = false;
    m_BindFailed = true;
    Logger::Log::Error(
        "SceneViewPanel: layout bind failed ({}). The panel stays unbound; "
        "check that UI/panels/SceneViewPanel.uxml is staged under the editor asset mount.",
        reason);
}

void SceneViewPanel::LoadBindAttachLayoutAndStyle(UIManager* ui)
{
    (void)ui;
    auto& am = EngineCore::GetInstance().GetAssetManager();

    const std::filesystem::path layoutAssetPath = std::filesystem::path("UI") / "panels" / "SceneViewPanel.uxml";
    const std::filesystem::path styleAssetPath = std::filesystem::path("UI") / "panels" / "SceneViewPanel.css";
    const std::filesystem::path sceneViewThemeAssetPath = std::filesystem::path("UI") / "theme" / "scene-view.css";

    const GUID layoutGuid = am.ResolveAssetGuid(layoutAssetPath, GameEngine::kAssetSourceAliasEditor);
    const GUID styleGuid = am.ResolveAssetGuid(styleAssetPath, GameEngine::kAssetSourceAliasEditor);
    const GUID sceneViewThemeGuid = am.ResolveAssetGuid(sceneViewThemeAssetPath, GameEngine::kAssetSourceAliasEditor);

    // IMPORTANT: avoid blocking waits (`future.get()`) on the UI thread.
    // Load and apply assets asynchronously; cancel callbacks automatically if the panel is destroyed.

    // Bind layout children. Reached once per panel: OnPostLayout holds m_BindPending for
    // the whole attempt, so there is no second entry to guard against and every exit
    // below resolves the latch.
    if (layoutGuid.IsNull())
    {
        MarkBindFailed("the layout guid did not resolve from the editor asset source");
    }
    else
    {
        m_LayoutLoadHandle = std::make_unique<AssetLoadHandle>(
            am.LoadAsset(layoutGuid,
                         [this, post = GetPostHandle(), layoutGuid](Result<SharedPtr<Asset>, AssetError> r)
                         {
                             // The load completes on a worker; every latch mutation below
                             // runs on the UI thread through PostAction.
                             const bool loaded = r.IsOk() && r.Value() &&
                                                 r.Value()->GetType() == AssetType::UILayout;
                             post.Post([this, layoutGuid, loaded]()
                                              {
                                                  if (!loaded)
                                                      return MarkBindFailed("the layout asset did not load");
                                                  UIManager* ui2 = GetOwnerManager();
                                                  if (!ui2 || m_BindApplied)
                                                  {
                                                      m_BindPending = false;
                                                      return;
                                                  }
                                                  auto& am2 = EngineCore::GetInstance().GetAssetManager();
                                                  auto a2 = am2.GetAsset(layoutGuid);
                                                  if (!a2 || a2->GetType() != AssetType::UILayout)
                                                      return MarkBindFailed("the loaded layout asset is not a UILayout");
                                                  const bool ok = ui2->BindLayoutToSubtreeChildrenFromAsset(this, *static_cast<UILayoutAsset*>(a2.get()));
                                                  if (!ok)
                                                      return MarkBindFailed("binding the layout into the panel subtree was rejected");

                                                  m_BindApplied = true;
                                                  m_BindPending = false;
                                                  // Now that the UXML children exist, resolve pointers and bind event handlers.
                                                  // Without this, optimized builds can race: RegisterViewportEvents() runs before
                                                  // SceneViewViewport exists and never gets retried, leaving SceneView input dead.
                                                  RefreshElementPointers();
                                                  RegisterViewportEvents();
                                                  BindToolbarWidgets();
                                                  WireToolOverlay();
                                              });
                         },
                         AssetLoadPriority::High));
    }

    // Attach scene-view theme stylesheet first (base styles), then panel-specific stylesheet.
    if (!sceneViewThemeGuid.IsNull() && !m_ThemeStyleLoadHandle)
    {
        m_ThemeStyleLoadHandle = std::make_unique<AssetLoadHandle>(
            am.LoadAsset(sceneViewThemeGuid,
                         [this, post = GetPostHandle(), sceneViewThemeGuid, styleGuid](Result<SharedPtr<Asset>, AssetError> r)
                         {
                             if (!r.IsOk() || !r.Value() || r.Value()->GetType() != AssetType::UIStyle)
                                 return;
                             post.Post([this, sceneViewThemeGuid, styleGuid]()
                                              {
                                                  UIManager* ui2 = GetOwnerManager();
                                                  if (!ui2)
                                                      return;
                                                  auto& am2 = EngineCore::GetInstance().GetAssetManager();
                                                  auto a2 = am2.GetAsset(sceneViewThemeGuid);
                                                  if (a2 && a2->GetType() == AssetType::UIStyle)
                                                  {
                                                      (void)ui2->AttachStyleToSubtreeFromAsset(this, *static_cast<UIStyleAsset*>(a2.get()));
                                                  }

                                                  // Now load/apply the panel-specific style (higher precedence).
                                                  if (!styleGuid.IsNull() && !m_PanelStyleLoadHandle)
                                                  {
                                                      m_PanelStyleLoadHandle = std::make_unique<AssetLoadHandle>(
                                                          am2.LoadAsset(styleGuid,
                                                                       [this, styleGuid, post = GetPostHandle()](Result<SharedPtr<Asset>, AssetError> r2)
                                                                       {
                                                                           if (!r2.IsOk() || !r2.Value() || r2.Value()->GetType() != AssetType::UIStyle)
                                                                               return;
                                                                           post.Post([this, styleGuid]()
                                                                                            {
                                                                                                UIManager* ui3 = GetOwnerManager();
                                                                                                if (!ui3)
                                                                                                    return;
                                                                                                auto& am3 = EngineCore::GetInstance().GetAssetManager();
                                                                                                auto a3 = am3.GetAsset(styleGuid);
                                                                                                if (a3 && a3->GetType() == AssetType::UIStyle)
                                                                                                {
                                                                                                    (void)ui3->AttachStyleToSubtreeFromAsset(this, *static_cast<UIStyleAsset*>(a3.get()));
                                                                                                }
                                                                                            });
                                                                       },
                                                                       AssetLoadPriority::High));
                                                  }
                                              });
                         },
                         AssetLoadPriority::High));
    }
}

void SceneViewPanel::RefreshElementPointers()
{
    // Refresh element pointers by id in case hot-reload reconciliation swapped nodes.
    m_ViewportSlots.fill(nullptr);
    m_ViewportLabels.fill(nullptr);
    if (UIElement* vp = FindById("SceneViewViewport"))
    {
        m_Viewport = vp;
        m_ViewportSlots[static_cast<size_t>(ViewportSlot::Perspective)] = vp;
    }
    m_ViewportSlots[static_cast<size_t>(ViewportSlot::Top)] = FindById("SceneViewViewportTop");
    m_ViewportSlots[static_cast<size_t>(ViewportSlot::Front)] = FindById("SceneViewViewportFront");
    m_ViewportSlots[static_cast<size_t>(ViewportSlot::Side)] = FindById("SceneViewViewportSide");
    // Every pane hosts a world render target sized from its own rect, so the
    // rect has to land on whole device pixels or the `cover` fit resolves to a
    // non-unit scale and the pane is resampled (see SetSnapRectToDevicePixels).
    for (UIElement* slot : m_ViewportSlots)
    {
        if (slot)
            slot->SetSnapRectToDevicePixels(true);
    }
    // Which quadrant each slot occupies in the four-view layout is fixed
    // (QuadrantName below). Naming it lets the theme round only the four corners
    // on the outside of the block, leaving the seams where panes meet square.
    if (UIElement* e = m_ViewportSlots[static_cast<size_t>(ViewportSlot::Top)])
        e->AddClass("quad-top-left");
    if (UIElement* e = m_ViewportSlots[static_cast<size_t>(ViewportSlot::Front)])
        e->AddClass("quad-top-right");
    if (UIElement* e = m_ViewportSlots[static_cast<size_t>(ViewportSlot::Side)])
        e->AddClass("quad-bottom-left");
    if (UIElement* e = m_ViewportSlots[static_cast<size_t>(ViewportSlot::Perspective)])
        e->AddClass("quad-bottom-right");
    m_ViewportLabels[static_cast<size_t>(ViewportSlot::Perspective)] =
        dynamic_cast<Label*>(FindById("SceneViewViewportLabelPerspective"));
    m_ViewportLabels[static_cast<size_t>(ViewportSlot::Top)] =
        dynamic_cast<Label*>(FindById("SceneViewViewportLabelTop"));
    m_ViewportLabels[static_cast<size_t>(ViewportSlot::Front)] =
        dynamic_cast<Label*>(FindById("SceneViewViewportLabelFront"));
    m_ViewportLabels[static_cast<size_t>(ViewportSlot::Side)] =
        dynamic_cast<Label*>(FindById("SceneViewViewportLabelSide"));
    m_QuadSplitVerticalTop = FindById("SceneViewQuadSplitVerticalTop");
    m_QuadSplitVerticalBottom = FindById("SceneViewQuadSplitVerticalBottom");
    m_QuadSplitHorizontal = FindById("SceneViewQuadSplitHorizontal");
    m_Toolbar = dynamic_cast<SceneViewToolbar*>(FindById("SceneViewToolbar"));
    if (m_Toolbar)
    {
        m_Toolbar->SetSceneController(m_Controller);
        if (m_OnToolbarReady)
            m_OnToolbarReady(m_Toolbar);
    }
    m_FpsLabel = nullptr;
    if (m_Viewport)
    {
        m_FpsLabel = dynamic_cast<Label*>(m_Viewport->FindById("SceneViewFpsLabel"));
        m_HoverHighlightLabel =
            dynamic_cast<Label*>(FindById("SceneViewHoverHighlightLabel"));
    }
    if (m_FpsLabel)
    {
        auto& style = m_FpsLabel->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PositionTop, StyleLength::Px(14.0f))
            .Set(Style::PositionRight, StyleLength::Px(14.0f));
        if (m_FpsVisible)
        {
            style.Reset(Style::Opacity);
            style.Reset(Style::PointerEvents);
        }
        else
        {
            style.Set(Style::Opacity, 0.0f);
            style.Set(Style::PointerEvents, false);
        }
    }
    for (size_t i = 0; i < m_ViewportLabels.size(); ++i)
        UpdateQuadViewLabel(static_cast<ViewportSlot>(i));
    for (UIElement* slot : m_ViewportSlots)
    {
        if (slot)
            slot->SetFocusable(true);
    }
    ApplyQuadViewLayout();
    RefreshViewportRotationGizmos();
    RefreshViewportRulerOverlays();
    RefreshViewportMeasureOverlays();
}

void SceneViewPanel::FocusViewport(UIElement* viewport)
{
    if (!viewport)
        return;
    // Scene View camera movement is driven by the editor input action system.
    // Giving the viewport UI focus makes UIManager consume keys like W/A/S/D
    // before those actions can see them. Use SetFocusById("") instead of
    // ClearFocus() so UIManager treats this as an explicit focus choice during
    // mouse-down dispatch and does not auto-focus the clicked viewport again.
    if (UIManager* ui = GetOwnerManager())
        ui->SetFocusById("");
}

void SceneViewPanel::FocusSceneViewViewport()
{
    FocusViewport(GetActiveViewportElement());
}


void SceneViewPanel::RegisterViewportEvents()
{
    // should only be called after m_Viewport is bound to UI
    RegisterQuadSplitHandleEvents();

    if (!m_AuxViewportEventsBound)
    {
        bool boundAnyAux = false;
        for (size_t i = 1; i < m_ViewportSlots.size(); ++i)
        {
            if (m_ViewportSlots[i])
            {
                RegisterAuxViewportEvents(m_ViewportSlots[i], static_cast<ViewportSlot>(i));
                boundAnyAux = true;
            }
        }
        m_AuxViewportEventsBound = boundAnyAux;
    }

    if (m_ViewportEventsBound)
    {
        return;
    }
    if (!m_Viewport)
    {
        return;
    }

    // Prevent duplicate handler registration.
    m_ViewportEventsBound = true;

    UIElement* overlayHost = GetViewportForOverlay();
    if (!m_FpsLabel && overlayHost)
    {
        auto fpsLabel = std::make_unique<Label>();
        fpsLabel->SetId("SceneViewFpsLabel");
        fpsLabel->AddClass("fps-counter");
        fpsLabel->SetText("FPS: --");

        // Try to restore persisted FPS label position and visibility from
        // editor preferences. If present, anchor with left/top instead of
        // the default bottom-left.
        double savedFpsX = 0.0;
        double savedFpsY = 0.0;
        bool hasFpsPos = false;
        {
            Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
            std::string err;
            (void)prefs.Load(&err);
            const bool hasX = prefs.TryGetDouble("sceneView.fpsLabel.x", savedFpsX);
            const bool hasY = prefs.TryGetDouble("sceneView.fpsLabel.y", savedFpsY);
            hasFpsPos = hasX && hasY;
            (void)prefs.TryGetBool("sceneView.fpsLabel.visible", m_FpsVisible);
        }

        {
            auto& style = fpsLabel->Overrides()
                .Set(Style::Position, PositionType::Absolute);
            if (hasFpsPos)
            {
                style.Set(Style::PositionLeft, StyleLength::Px(static_cast<float>(savedFpsX)))
                     .Set(Style::PositionTop,  StyleLength::Px(static_cast<float>(savedFpsY)));
                m_FpsUsingDefaultPosition = false;
            }
            else
            {
                // Default: bottom-left corner of the viewport. The .fps-counter
                // CSS class sets an explicit width and height so Yoga can
                // resolve bottom-anchoring without stretching the node.
                style.Set(Style::PositionLeft,   StyleLength::Px(14.0f))
                     .Set(Style::PositionBottom, StyleLength::Px(14.0f));
                m_FpsUsingDefaultPosition = true;
            }
            if (m_FpsVisible)
            {
                style.Reset(Style::Opacity);
                style.Reset(Style::PointerEvents);
            }
            else
            {
                style.Set(Style::Opacity, 0.0f);
                style.Set(Style::PointerEvents, false);
            }
        }
        m_FpsLabel = fpsLabel.get();
        // Add FPS label to viewport so absolute positioning works correctly.
        overlayHost->AddChild(std::move(fpsLabel));

        // The view overlays (the "Loading scene" and "Compiling shaders" banners, notices)
        // stack on the layer the overlay host adds to this viewport.
        PublishOverlayLayer(*overlayHost);

        // Drag-to-reposition with LMB. Label lives inside the active viewport, so the
        // final Left/Top values are viewport-local.
        m_FpsLabel->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
            if (e.Button == 1)
            {
                m_FpsContextMenuArmed = true;
                if (m_FpsLabel)
                    e.Capture(m_FpsLabel);
                e.Stop();
                return;
            }
            if (e.Button != 0) return;
            if (!m_FpsLabel) return;
            m_FpsLabelDrag.pressed     = true;
            m_FpsLabelDrag.active      = false;
            m_FpsLabelDrag.startMouseX = e.X;
            m_FpsLabelDrag.startMouseY = e.Y;
            m_FpsLabelDrag.startLabelX = m_FpsLabel->GetLayoutX();
            m_FpsLabelDrag.startLabelY = m_FpsLabel->GetLayoutY();
            e.Capture(m_FpsLabel);
            e.Stop();
        });

        m_FpsLabel->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) {
            if (m_FpsContextMenuArmed)
            {
                e.Stop();
                return;
            }
            if (!m_FpsLabelDrag.pressed) return;
            if (!m_FpsLabel) return;
            UIElement* fpsViewport = m_FpsLabel->GetParent();
            if (!fpsViewport) return;

            const float dx = e.X - m_FpsLabelDrag.startMouseX;
            const float dy = e.Y - m_FpsLabelDrag.startMouseY;

            if (!m_FpsLabelDrag.active)
            {
                constexpr float kDragThresholdPx = 5.0f;
                if ((dx * dx + dy * dy) < (kDragThresholdPx * kDragThresholdPx))
                    return;
                m_FpsLabelDrag.active = true;
            }

            // Convert absolute drag delta into viewport-local absolute
            // position. The label anchors via Left/Top once dragging starts,
            // so clear the default Right anchor.
            float newX = (m_FpsLabelDrag.startLabelX + dx) - fpsViewport->GetLayoutX();
            float newY = (m_FpsLabelDrag.startLabelY + dy) - fpsViewport->GetLayoutY();

            const float vw  = fpsViewport->GetLayoutWidth();
            const float vh  = fpsViewport->GetLayoutHeight();
            const float lw  = m_FpsLabel->GetLayoutWidth();
            const float lh  = m_FpsLabel->GetLayoutHeight();
            const float maxX = std::max(0.0f, vw - lw);
            const float maxY = std::max(0.0f, vh - lh);
            newX = std::clamp(newX, 0.0f, maxX);
            newY = std::clamp(newY, 0.0f, maxY);

            {
                auto& ov = m_FpsLabel->Overrides();
                ov.Reset(Style::PositionRight);
                ov.Reset(Style::PositionBottom);
                ov.Reset(Style::MarginLeft);
                ov.Set(Style::Position,     PositionType::Absolute)
                  .Set(Style::PositionLeft, StyleLength::Px(newX))
                  .Set(Style::PositionTop,  StyleLength::Px(newY));
            }
            /* Same as SceneToolOverlay drag: captured-move fast path can skip Yoga without this. */
            m_FpsLabel->RequestRelayout();
            e.Stop();
        });

        m_FpsLabel->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
            if (e.Button == 1)
            {
                const bool shouldOpen = m_FpsContextMenuArmed &&
                                        m_FpsLabel &&
                                        m_FpsLabel->ContainsPoint(e.X, e.Y);
                m_FpsContextMenuArmed = false;
                if (shouldOpen)
                    ShowFpsContextMenu(e.X, e.Y);
                e.Stop();
                return;
            }
            if (e.Button != 0) return;
            const bool wasDragActive = m_FpsLabelDrag.active;
            m_FpsLabelDrag.pressed = false;
            m_FpsLabelDrag.active  = false;

            if (!wasDragActive || !m_FpsLabel)
                return;
            UIElement* fpsViewport = m_FpsLabel->GetParent();
            if (!fpsViewport)
                return;

            e.Stop();

            const float localX = m_FpsLabel->GetLayoutX() - fpsViewport->GetLayoutX();
            const float localY = m_FpsLabel->GetLayoutY() - fpsViewport->GetLayoutY();

            // User has now manually positioned the label — stop auto-shifting
            // it to avoid the rotation gizmo on subsequent frames.
            m_FpsUsingDefaultPosition = false;

            Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
            std::string err;
            (void)prefs.Load(&err);
            prefs.SetDouble("sceneView.fpsLabel.x", static_cast<double>(localX));
            prefs.SetDouble("sceneView.fpsLabel.y", static_cast<double>(localY));
            (void)prefs.Save(&err);
        });
    }

    if (!m_HoverHighlightLabel && overlayHost)
    {
        auto hoverLabel = std::make_unique<Label>();
        hoverLabel->SetId("SceneViewHoverHighlightLabel");
        hoverLabel->AddClass("scene-view-hover-pill");
        hoverLabel->SetText("");
        hoverLabel->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PositionBottom, StyleLength::Px(kSceneViewHudBottomInsetPx))
            .Set(Style::PointerEvents, false)
            .Set(Style::Opacity, 0.0f);
        m_HoverHighlightLabel = hoverLabel.get();
        overlayHost->AddChild(std::move(hoverLabel));
    }

    SyncViewportOverlays();

    // Mouse down: start dragging and capture pointer
    // Editor camera controls:
    // - RMB = FPS look
    // - MMB = orbit
    // - Alt + LMB = orbit
    // - Alt + Ctrl/Cmd + LMB = pan
    // - Alt + RMB = dolly (zoom)
    m_Viewport->RegisterEventHandler(kEventMouseDown, [this](UIEvent& ev){
        SetHoveredViewportSlot(ViewportSlot::Perspective);
        SetActiveViewportSlot(ViewportSlot::Perspective);
        FocusViewport(m_Viewport);
        m_Dragging = true;
        m_DragButton = ev.Button; // 0=LMB, 1=RMB, 2=MMB
        BeginCameraDragTracking(ev.X, ev.Y);
        ev.Capture(m_Viewport);

        const bool alt = (ev.Mods & Input::kModAlt) != 0;
        const bool primaryMod = Input::IsPrimaryShortcutModifier(ev.Mods);

        // Detect camera modes (Ctrl on Windows/Linux, Cmd on Mac).
        // 2D and fixed-axis views use pan/zoom controls instead of tumble/orbit.
        const bool fixedControls = m_Controller && m_Controller->UsesFixedViewPanControls();
        const bool spacePan = (ev.Button == 0 && m_SpaceHeld);
        if (spacePan)
            m_SpacePanDragStarted = true;
        const bool rmbPanFixed = (ev.Button == 1 && fixedControls && !alt);
        const bool mmbPanFixed = (ev.Button == 2 && fixedControls && !alt);
        m_IsAltOrbiting = (ev.Button == 0 && alt && !primaryMod && !fixedControls); // Alt + LMB = orbit (free 3D views)
        m_IsPanning = (ev.Button == 0 && alt && primaryMod)                // Alt + Ctrl/Cmd + LMB = pan
                    || (ev.Button == 0 && alt && !primaryMod && fixedControls)
                    || spacePan                                             // Space + LMB = pan
                    || rmbPanFixed
                    || mmbPanFixed;
        m_IsDollying = (ev.Button == 1 && alt);                            // Alt + RMB = dolly

        // RMB context menu (click-without-drag) - only if not in a camera mode
        if (ev.Button == 1 && !m_IsDollying && !m_IsPanning)
        {
            m_RmbCandidateMenu = true;
            m_RmbDownX = ev.X;
            m_RmbDownY = ev.Y;
        }
        else
        {
            m_RmbCandidateMenu = false;
        }

        // Forward LMB presses into the SceneToolContext as pointer-down events
        // (but not if we're in a camera control mode)
        m_LmbDragIsToolMode = (ev.Button == 0 && !m_IsAltOrbiting && !m_IsPanning);
        if (m_LmbDragIsToolMode && HandleMeasurePointerDown(m_Controller, m_Viewport, ViewportSlot::Perspective, ev))
            return;
        if (m_LmbDragIsToolMode && m_MeasureToolEnabled)
        {
            ev.Stop();
            return;
        }
        if (m_LmbDragIsToolMode && m_Controller)
        {
            using namespace Editor::SceneTools;
            ScenePointerEvent toolEv{};
            if (m_Viewport)
            {
                const float localX = ev.X - m_Viewport->GetLayoutX();
                const float localY = ev.Y - m_Viewport->GetLayoutY();
                toolEv.viewX = localX;
                toolEv.viewY = localY;

                const float viewW = m_Viewport->GetLayoutWidth();
                const float viewH = m_Viewport->GetLayoutHeight();
                toolEv.viewW = viewW;
                toolEv.viewH = viewH;
                toolEv.ray = m_Controller->MakeGizmoRay(localX, localY, viewW, viewH);
                m_Controller->PopulatePointerCameraState(toolEv);
            }

            toolEv.button = PointerButton::Left;
            toolEv.phase  = PointerPhase::Down;
            Editor::SceneTools::PopulateScenePointerMods(toolEv, ev.Mods);

            m_Controller->GetToolContext().HandlePointerEvent(toolEv);
        }
    });

    // Mouse up: stop dragging when releasing the active button
    m_Viewport->RegisterEventHandler(kEventMouseUp, [this](UIEvent& ev){
        const bool showSceneContextMenu = (ev.Button == 1 && m_RmbCandidateMenu && m_Dragging && m_DragButton == 1);

        if (m_Dragging && ev.Button == m_DragButton) {
            m_Dragging = false;
            m_DragButton = -1;
            m_IsAltOrbiting = false;
            m_IsPanning = false;
            m_IsDollying = false;
            EndCameraDragTracking();
        }

        if (showSceneContextMenu)
        {
            if (m_Window)
            {
                if (!m_ContextMenu)
                    m_ContextMenu = CreateContextMenu();

                if (m_ContextMenu)
                {
                    const uint32_t mask = static_cast<uint32_t>(Editor::EditorContextMenuTarget::SceneView);
                    auto scriptItems = Editor::ScriptMenuRegistry::Get().GetContextItems(mask);

                    int scriptEntryCount = 0;
                    for (const auto& si : scriptItems)
                    {
                        if (si.commandId == 0 || si.path.empty())
                            continue;
                        ++scriptEntryCount;
                    }
                    // Post-process toggles live on the toolbar sun control only; viewport RMB is script hooks only.
                    if (scriptEntryCount > 0)
                    {
                        // Always (re)install the handler: m_ContextMenu is shared with the
                        // toolbar icon-toggle menu, which replaces the handler when opened.
                        m_ContextMenu->SetCommandHandler([](uint32_t cmd) {
                            uint64_t dom = 0;
                            std::string method;
                            if (Editor::ScriptMenuRegistry::Get().TryResolveCommand(cmd, dom, method) && !method.empty())
                            {
                                try
                                {
                                    auto& clr = EngineCore::GetInstance().GetScriptManager().GetCLRHost();
                                    int32_t out = 0;
                                    (void)clr.InvokeInDomain(dom, method.c_str(), (uint32_t)method.size(), &out);
                                }
                                catch (...)
                                {
                                }
                            }
                        });

                        m_ContextMenu->Clear();
                        ContextMenuBuilder builder;
                        for (const auto& si : scriptItems)
                        {
                            if (si.commandId == 0 || si.path.empty())
                                continue;
                            builder.AddItem(si.path, si.commandId, MenuItemFlag_None, si.priority,
                                            EditorIcons::kScript);
                        }
                        builder.Build(m_ContextMenu.get());
                        m_ContextMenu->Show(m_Window, (int)ev.X, (int)ev.Y);
                        ev.Stop();
                    }
                }
            }
        }

        m_RmbCandidateMenu = false;

        if (HandleMeasurePointerUp(m_Controller, m_Viewport, ViewportSlot::Perspective, ev))
            return;
        if (m_MeasureToolEnabled && ev.Button == 0)
        {
            ev.Stop();
            return;
        }

        // Forward LMB releases into the SceneToolContext as pointer-up events.
        if (ev.Button == 0 && m_Controller)
        {
            using namespace Editor::SceneTools;
            ScenePointerEvent toolEv{};
            if (m_Viewport)
            {
                const float localX = ev.X - m_Viewport->GetLayoutX();
                const float localY = ev.Y - m_Viewport->GetLayoutY();
                toolEv.viewX = localX;
                toolEv.viewY = localY;

                const float viewW = m_Viewport->GetLayoutWidth();
                const float viewH = m_Viewport->GetLayoutHeight();
                toolEv.viewW = viewW;
                toolEv.viewH = viewH;
                toolEv.ray = m_Controller->MakeGizmoRay(localX, localY, viewW, viewH);
                m_Controller->PopulatePointerCameraState(toolEv);
            }

            toolEv.button = PointerButton::Left;
            toolEv.phase  = PointerPhase::Up;
            Editor::SceneTools::PopulateScenePointerMods(toolEv, ev.Mods);

            m_Controller->GetToolContext().HandlePointerEvent(toolEv);
        }
    });

    // Mouse move: while dragging, update camera mode flags from current modifiers and
    // while dragging with LMB, forward motion as pointer-move events.
    m_Viewport->RegisterEventHandler(kEventMouseMove, [this](UIEvent& ev){
        SetHoveredViewportSlot(ViewportSlot::Perspective);
        if (m_Dragging)
            SetActiveViewportSlot(ViewportSlot::Perspective);
        if (m_Viewport)
        {
            m_SceneViewportPointerInside = true;
            m_SceneViewportPointerLocalX = ev.X - m_Viewport->GetLayoutX();
            m_SceneViewportPointerLocalY = ev.Y - m_Viewport->GetLayoutY();
        }

        if (!m_Controller)
            return;

        // Push cursor position to the ruler overlay so its position indicator
        // tracks the mouse. The overlay fills the viewport rect, so localX/Y
        // are pixels from the viewport top-left.
        SceneViewRulerOverlay* perspectiveRuler =
            m_RulerOverlays[static_cast<size_t>(ViewportSlot::Perspective)];
        if (perspectiveRuler && m_Viewport)
        {
            const float lx = ev.X - m_Viewport->GetLayoutX();
            const float ly = ev.Y - m_Viewport->GetLayoutY();
            perspectiveRuler->SetCursor(lx, ly, true);
        }

        // RMB click vs drag detection (context menu only on click-without-drag)
        if (m_RmbCandidateMenu && m_Dragging && m_DragButton == 1)
        {
            const float dx = ev.X - m_RmbDownX;
            const float dy = ev.Y - m_RmbDownY;
            const float dist2 = dx * dx + dy * dy;
            constexpr float kThresholdPx = 4.0f;
            if (dist2 > (kThresholdPx * kThresholdPx))
            {
                m_RmbCandidateMenu = false;
            }
        }

        // While dragging, keep Alt-based camera modes (orbit/pan/dolly) in sync with the
        // *current* modifier state so Alt can be pressed or released after mouse-down.
        // Exception: if the drag started as a tool interaction (gizmo/marquee), don't let
        // Alt mid-drag switch to camera mode — the marquee should continue uninterrupted.
        if (m_Dragging && !m_LmbDragIsToolMode)
        {
            const bool alt  = (ev.Mods & Input::kModAlt) != 0;
            const bool primaryMod = Input::IsPrimaryShortcutModifier(ev.Mods);

            if (m_DragButton == 0)
            {
                // LMB drag: Alt toggles orbit/pan depending on primary modifier.
                const bool fixedControls = m_Controller->UsesFixedViewPanControls();
                m_IsAltOrbiting = alt && !primaryMod && !fixedControls;        // Alt + LMB (free 3D views)
                m_IsPanning     = (alt && primaryMod)                         // Alt + Ctrl/Cmd + LMB
                               || (alt && !primaryMod && fixedControls)
                               || m_SpaceHeld;                                // Space + LMB
            }
            else if (m_DragButton == 1)
            {
                // RMB drag: Alt toggles dolly (zoom). In fixed-axis views, plain RMB pans.
                const bool fixedControls = m_Controller->UsesFixedViewPanControls();
                m_IsDollying = alt;                     // Alt + RMB
                m_IsPanning  = !alt && fixedControls;
            }
            else if (m_DragButton == 2)
            {
                m_IsAltOrbiting = false;
                m_IsPanning = m_Controller->UsesFixedViewPanControls() && !alt;
            }
        }

        if (HandleMeasurePointerMove(m_Controller, m_Viewport, ViewportSlot::Perspective, ev))
            return;

        if (ApplyImmediatePan(m_Controller, m_Viewport, ev))
            return;
        if (m_MeasureToolEnabled)
            return;

        // Hover preview: when not dragging, let the controller do lightweight hover picking.
        if (!m_Dragging)
        {
            using namespace Editor::SceneTools;
            ScenePointerEvent hoverEv{};
            if (m_Viewport)
            {
                const float localX = ev.X - m_Viewport->GetLayoutX();
                const float localY = ev.Y - m_Viewport->GetLayoutY();
                hoverEv.viewX = localX;
                hoverEv.viewY = localY;

                const float viewW = m_Viewport->GetLayoutWidth();
                const float viewH = m_Viewport->GetLayoutHeight();
                hoverEv.viewW = viewW;
                hoverEv.viewH = viewH;
                hoverEv.ray = m_Controller->MakeGizmoRay(localX, localY, viewW, viewH);
                m_Controller->PopulatePointerCameraState(hoverEv);
            }
            hoverEv.button = PointerButton::None;
            hoverEv.phase  = PointerPhase::Move;
            Editor::SceneTools::PopulateScenePointerMods(hoverEv, ev.Mods);

            m_Controller->HandleHoverDetection(hoverEv);
            m_Controller->GetToolContext().HandlePointerEvent(hoverEv);
        }

        if (!m_Dragging || m_DragButton != 0)
            return;

        using namespace Editor::SceneTools;
        ScenePointerEvent toolEv{};
        if (m_Viewport)
        {
            const float localX = ev.X - m_Viewport->GetLayoutX();
            const float localY = ev.Y - m_Viewport->GetLayoutY();
            toolEv.viewX = localX;
            toolEv.viewY = localY;

            const float viewW = m_Viewport->GetLayoutWidth();
            const float viewH = m_Viewport->GetLayoutHeight();
            toolEv.viewW = viewW;
            toolEv.viewH = viewH;
            toolEv.ray = m_Controller->MakeGizmoRay(localX, localY, viewW, viewH);
            m_Controller->PopulatePointerCameraState(toolEv);
        }

        toolEv.button = PointerButton::Left;
        toolEv.phase  = PointerPhase::Move;
        Editor::SceneTools::PopulateScenePointerMods(toolEv, ev.Mods);

        GE_CPU_PROFILE_SCOPE("SceneViewPanel.DragMouseMove");
        m_Controller->GetToolContext().HandlePointerEvent(toolEv);
    });

    m_Viewport->RegisterEventHandler(kEventMouseEnter, [this](UIEvent&) {
        SetHoveredViewportSlot(ViewportSlot::Perspective);
    });

    // Clear the ruler overlay's cursor indicator when the mouse leaves the
    // viewport so the lines don't get stuck at the last hover position.
    m_Viewport->RegisterEventHandler(kEventMouseLeave, [this](UIEvent& /*ev*/){
        if (m_HasHoveredViewportSlot && m_HoveredViewportSlot == ViewportSlot::Perspective)
            m_HasHoveredViewportSlot = false;
        m_SceneViewportPointerInside = false;
        if (SceneViewRulerOverlay* perspectiveRuler =
                m_RulerOverlays[static_cast<size_t>(ViewportSlot::Perspective)])
            perspectiveRuler->SetCursor(0.0f, 0.0f, false);
        // Keep active measure drags alive across the viewport edge. The mouse
        // down path captures the pointer, so the real mouse-up should be the
        // thing that commits/cancels the drag instead of mouse-leave.
    });

    // Scroll wheel: dolly Scene View camera in/out when hovering the viewport (when enabled in settings).
    // Ctrl/Cmd+scroll cycles through pick candidates under the cursor instead of zooming.
    m_Viewport->RegisterEventHandler(kEventScroll, [this](UIEvent& ev){
        SetActiveViewportSlot(ViewportSlot::Perspective);
        if (!m_Controller)
            return;

        const float dy = ev.ScrollY;
        if (dy == 0.0f)
            return;

        // Ctrl/Cmd+scroll: cycle through pick candidates under the cursor.
        const bool ctrlOrCmd = (ev.Mods & Input::kModControl) != 0 || (ev.Mods & Input::kModSuper) != 0;
        if (ctrlOrCmd)
        {
            const float localX = ev.X - m_Viewport->GetLayoutX();
            const float localY = ev.Y - m_Viewport->GetLayoutY();
            const float viewW = m_Viewport->GetLayoutWidth();
            const float viewH = m_Viewport->GetLayoutHeight();
            int direction = (dy > 0.0f) ? 1 : -1;
            m_Controller->CycleEntityUnderCursor(localX, localY, viewW, viewH, direction);
            ev.Stop();
            return;
        }

        if (!Editor::SceneViewSettings::Get().GetScrollWheelDollyEnabled())
            return;

        // Pixel-perfect 2D: every wheel event advances one discrete scale step
        // while keeping the world point under the cursor anchored on screen.
        if (m_Controller->Is2DMode() && Editor::SceneViewSettings::Get().GetPixelPerfect2D())
        {
            // 2D: invert versus 3D scroll-to-zoom — scroll toward content zooms out.
            int step = (dy > 0.0f) ? 1 : -1;
            if (Editor::SceneViewSettings::Get().GetScrollWheelDollyReversed())
                step = -step;
            const float localX = ev.X - m_Viewport->GetLayoutX();
            const float localY = ev.Y - m_Viewport->GetLayoutY();
            const float viewW = m_Viewport->GetLayoutWidth();
            const float viewH = m_Viewport->GetLayoutHeight();
            m_Controller->ZoomPixelPerfectAtCursor(step, localX, localY, viewW, viewH);
            ev.Stop();
            return;
        }

        if (m_Controller->IsOrthographic() && !m_Controller->Is2DMode())
        {
            int step = dy > 0.0f ? 1 : -1;
            if (Editor::SceneViewSettings::Get().GetScrollWheelDollyReversed()) step = -step;
            m_Controller->ZoomOrthographicStep(step);
            ev.Stop();
            return;
        }

        // Positive scrollY typically means scrolling down; invert so wheel up = zoom in.
        // User can reverse this in settings.
        const float kScrollToDollyScale = 0.25f;
        float deltaY = -dy * kScrollToDollyScale;
        if (Editor::SceneViewSettings::Get().GetScrollWheelDollyReversed())
            deltaY = -deltaY;
        // 2D (non-pixel-perfect): reverse scroll-vs-zoom versus the 3D viewport.
        if (m_Controller->Is2DMode())
            deltaY = -deltaY;
        m_Controller->UpdateDolly(deltaY);

        // Consume the event so parent ScrollViews do not also scroll.
        ev.Stop();
    });

    // Key down/up on the Scene View viewport: route to the active Scene tool as SceneKeyEvent.
    m_Viewport->RegisterEventHandler(kEventKeyDown, [this](UIEvent& ev){
        // Track spacebar for space+LMB panning in 2D mode.
        if (Editor::MatchesCatalogShortcut("Scene View", "Pan Hold (2D)", ev.Key, ev.Mods))
        {
            BeginViewportSpaceToggle(ViewportSlot::Perspective, ev);
            return;
        }

        SetActiveViewportSlot(ViewportSlot::Perspective);
        if (!m_Controller)
            return;

        if (HandleSceneViewCommandShortcut(m_Controller, ev))
            return;

        // Suppress hotkeys while any camera mode is active (RMB look, Alt orbit,
        // pan, dolly) so WASD + Shift speed-modifier don't conflict with shortcuts.
        const bool cameraActive = IsLooking() || IsOrbiting() || IsPanning() || IsDollying();

        // [ / ]: navigate camera position history (auto-captured when the camera
        // settles). No modifier — camera-only, doesn't conflict with transform tools.
        if (!cameraActive && ev.Mods == 0)
        {
            if (ev.Key == Input::kKeyCode_LeftBracket)
            {
                if (m_Controller->StepCameraHistoryBack())
                    ev.Stop();
                return;
            }
            if (ev.Key == Input::kKeyCode_RightBracket)
            {
                if (m_Controller->StepCameraHistoryForward())
                    ev.Stop();
                return;
            }
        }

        // F: frame selected entity. Shift+F: frame all.
        if (!cameraActive && ev.Key == Input::kKeyCode_F)
        {
            if ((ev.Mods & Input::kModShift) && !(ev.Mods & (Input::kModControl | Input::kModAlt | Input::kModSuper)))
            {
                m_Controller->FrameAll();
                ev.Stop();
                return;
            }
            if (ev.Mods == 0)
            {
                m_Controller->FrameOrigin();
                ev.Stop();
                return;
            }
        }

        // Transform tool hotkeys: Q for Selection, W for Move, E for Rotate, R for Scale.
        // Pressing any of these activates the corresponding tool and mode.
        if (!cameraActive)
        {
            if (Editor::MatchesCatalogShortcut("Transform Tool", "Select Mode", ev.Key, ev.Mods))
            {
                if (m_MeasureToolEnabled)
                    SetMeasureToolEnabled(false);
                m_Controller->SetTransformMode(Editor::SceneTools::TransformMode::Select);
                UpdateToolOverlayModeButtons();
                return;
            }
            if (Editor::MatchesCatalogShortcut("Transform Tool", "Translate Mode", ev.Key, ev.Mods))
            {
                if (m_MeasureToolEnabled)
                    SetMeasureToolEnabled(false);
                m_Controller->SetTransformMode(Editor::SceneTools::TransformMode::Translate);
                UpdateToolOverlayModeButtons();
                return;
            }
            if (Editor::MatchesCatalogShortcut("Transform Tool", "Rotate Mode", ev.Key, ev.Mods))
            {
                if (m_MeasureToolEnabled)
                    SetMeasureToolEnabled(false);
                m_Controller->SetTransformMode(Editor::SceneTools::TransformMode::Rotate);
                UpdateToolOverlayModeButtons();
                return;
            }
            if (Editor::MatchesCatalogShortcut("Transform Tool", "Scale Mode", ev.Key, ev.Mods))
            {
                if (m_MeasureToolEnabled)
                    SetMeasureToolEnabled(false);
                m_Controller->SetTransformMode(Editor::SceneTools::TransformMode::Scale);
                UpdateToolOverlayModeButtons();
                return;
            }
            if (ev.Mods == 0 && ev.Key == Input::kKeyCode_M)
            {
                ToggleMeasureTool();
                ev.Stop();
                return;
            }
            if (ev.Mods == 0 && ev.Key == Input::kKeyCode_P)
            {
                // Toggle FPS visibility in the combined perf label and overlay.
                m_FpsVisible = !m_FpsVisible;
                if (m_FpsLabel)
                {
                    auto& style = m_FpsLabel->Overrides();
                    if (m_FpsVisible)
                    {
                        style.Reset(Style::Opacity);
                        style.Reset(Style::PointerEvents);
                    }
                    else
                    {
                        style.Set(Style::Opacity, 0.0f);
                        style.Set(Style::PointerEvents, false);
                    }
                }
                {
                    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
                    std::string err;
                    (void)prefs.Load(&err);
                    prefs.SetBool("sceneView.fpsLabel.visible", m_FpsVisible);
                    (void)prefs.Save(&err);
                }
                return;
            }
        }

        using namespace Editor::SceneTools;
        SceneKeyEvent keyEv{};
        keyEv.keyCode = static_cast<std::uint32_t>(ev.Key);
        keyEv.pressed = true;
        keyEv.alt   = (ev.Mods & Input::kModAlt) != 0;
        keyEv.ctrl  = (ev.Mods & Input::kModControl) != 0;
        keyEv.shift = (ev.Mods & Input::kModShift) != 0;

        m_Controller->GetToolContext().HandleKeyEvent(keyEv);
    });

    m_Viewport->RegisterEventHandler(kEventKeyUp, [this](UIEvent& ev){
        if (Editor::MatchesCatalogShortcutKeyOnly("Scene View", "Pan Hold (2D)", ev.Key))
        {
            EndViewportSpaceToggle(ViewportSlot::Perspective, ev);
            return;
        }

        if (!m_Controller)
            return;

        using namespace Editor::SceneTools;
        SceneKeyEvent keyEv{};
        keyEv.keyCode = static_cast<std::uint32_t>(ev.Key);
        keyEv.pressed = false;
        keyEv.alt   = (ev.Mods & Input::kModAlt) != 0;
        keyEv.ctrl  = (ev.Mods & Input::kModControl) != 0;
        keyEv.shift = (ev.Mods & Input::kModShift) != 0;

        m_Controller->GetToolContext().HandleKeyEvent(keyEv);
    });
}


void SceneViewPanel::BindToolbarWidgets()
{
    if (m_Toolbar)
    {
        m_Toolbar->SetOnToggleView2D([this]() {
            return ToggleActiveViewport2DMode();
        });
    }

    auto* bookmarks = dynamic_cast<CameraBookmarksWidget*>(FindById("CameraBookmarks"));
    if (bookmarks)
    {
        bookmarks->SetSceneController(m_Controller);
        bookmarks->SetPanel(this);
        m_Controller->SetBookmarksWidget(bookmarks);
        m_CameraBookmarksBoundInstanceId = bookmarks->GetInstanceId();
    }

    RefreshViewportRotationGizmos();
    RefreshViewportRulerOverlays();
    RefreshViewportMeasureOverlays();

    // Add preview popup
    {
        // popup wrapper
        auto preview = std::make_unique<UIElement>();
        preview->AddClass("camera-bookmarks-preview");

        // image child
        auto img = std::make_unique<UIElement>();
        img->AddClass("camera-bookmarks-preview-image");

        preview->AddChild(std::move(img));

        m_BookmarkPreviewPopup = preview.get();
        preview->SetOverlayLayer(OverlayLayer::Tooltip);
        AddChild(std::move(preview));
    }

    // Asset preview overlay (full viewport, hidden by default)
    if (UIElement* previewViewport = GetViewportForOverlay())
    {
        auto overlay = std::make_unique<UIElement>();
        overlay->AddClass("scene-asset-preview");
        overlay->SetId("SceneAssetPreview");
        {
            overlay->Overrides().Set(Style::Display, DisplayMode::None);
        }

        auto image = std::make_unique<UIElement>();
        image->AddClass("scene-asset-preview-image");
        image->SetId("SceneAssetPreviewImage");

        m_AssetPreviewOverlay = overlay.get();
        m_AssetPreviewImage = image.get();

        overlay->AddChild(std::move(image));
        previewViewport->AddChild(std::move(overlay));
        SyncViewportOverlays();
    }
}

void SceneViewPanel::SetMeasureToolEnabled(bool enabled)
{
    if (m_MeasureToolEnabled == enabled)
        return;

    m_MeasureToolEnabled = enabled;
    m_MeasureDragActive = false;
    ApplyMeasureSoloMode(enabled);
    for (SceneViewMeasureOverlay* overlay : m_MeasureOverlays)
    {
        if (!overlay)
            continue;
        overlay->SetMeasureToolEnabled(enabled);
        overlay->SetTwoDMode(m_MeasureTwoDMode);
        if (!enabled)
            overlay->CancelMeasure();
    }
    UpdateMeasureToolButtons();
    UpdateToolOverlayModeButtons();
}

void SceneViewPanel::ToggleMeasureTool()
{
    SetMeasureToolEnabled(!m_MeasureToolEnabled);
}

void SceneViewPanel::UpdateMeasureToolButtons()
{
    static const char* kIds[] = {
        "MeasureToolBtn",
        "InlineMeasureBtn"
    };

    for (const char* id : kIds)
    {
        if (auto* btn = dynamic_cast<Button*>(FindById(id)))
        {
            if (m_MeasureToolEnabled)
                btn->AddClass("icon-active");
            else
                btn->RemoveClass("icon-active");
        }
    }
}

void SceneViewPanel::ApplyMeasureSoloMode(bool enabled)
{
    for (size_t i = 0; i < m_MeasureSavedControllerState.size(); ++i)
    {
        SceneViewController* controller = GetSceneControllerForSlot(static_cast<ViewportSlot>(i));
        if (!controller)
            continue;

        if (enabled)
        {
            if (!m_MeasureSavedControllerState[i])
            {
                m_MeasureSavedToolKinds[i] = static_cast<std::uint8_t>(controller->GetActiveToolKind());
                m_MeasureSavedTransformGizmosVisible[i] = controller->AreTransformGizmosVisible();
                m_MeasureSavedControllerState[i] = true;
            }
            controller->SetActiveTool(SceneViewController::ToolKind::Selection);
            controller->SetTransformGizmosVisible(false);
        }
        else if (m_MeasureSavedControllerState[i])
        {
            controller->SetTransformGizmosVisible(m_MeasureSavedTransformGizmosVisible[i]);
            controller->SetActiveTool(static_cast<SceneViewController::ToolKind>(m_MeasureSavedToolKinds[i]));
            m_MeasureSavedControllerState[i] = false;
        }
    }
}

void SceneViewPanel::SetMeasureUnitSystem(SceneViewMeasureOverlay::UnitSystem unitSystem)
{
    if (m_MeasureUnitSystem == unitSystem)
        return;

    m_MeasureUnitSystem = unitSystem;
    for (SceneViewMeasureOverlay* overlay : m_MeasureOverlays)
    {
        if (overlay)
            overlay->SetUnitSystem(unitSystem);
    }

    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    prefs.SetString("sceneView.measure.units",
                    unitSystem == SceneViewMeasureOverlay::UnitSystem::Imperial ? "imperial" : "metric");
    (void)prefs.Save(&err);
}

void SceneViewPanel::SetMeasureTwoDMode(SceneViewMeasureOverlay::TwoDMode mode)
{
    if (m_MeasureTwoDMode == mode)
        return;

    m_MeasureTwoDMode = mode;
    for (SceneViewMeasureOverlay* overlay : m_MeasureOverlays)
    {
        if (overlay)
            overlay->SetTwoDMode(mode);
    }

    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    prefs.SetString("sceneView.measure.2dMode",
                    mode == SceneViewMeasureOverlay::TwoDMode::Points ? "points" : "triangle");
    (void)prefs.Save(&err);
}

ECS::EntityHandle SceneViewPanel::CreateMeasureEntity(const std::array<float, 3>& start,
                                                      const std::array<float, 3>& end,
                                                      bool is2D)
{
    const float dx = end[0] - start[0];
    const float dy = end[1] - start[1];
    const float dz = end[2] - start[2];
    if (dx * dx + dy * dy + dz * dz < 1.0e-8f)
        return {};

    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    if (!world)
        return {};

    ECS::EntityHandle entity = world->CreateEntity();
    if (!entity.IsValid())
        return {};

    auto makeName = [](const char* text) {
        Components::Name name{};
        std::memset(name.value, 0, sizeof(name.value));
        std::strncpy(name.value, text, sizeof(name.value) - 1);
        return name;
    };

    auto makeTransform = [](const std::array<float, 3>& point) {
        Components::Transform transform{};
        transform.SetIdentity();
        transform.matrix[12] = point[0];
        transform.matrix[13] = point[1];
        transform.matrix[14] = point[2];
        return transform;
    };

    const std::array<float, 3> center{
        (start[0] + end[0]) * 0.5f,
        (start[1] + end[1]) * 0.5f,
        (start[2] + end[2]) * 0.5f
    };
    const std::array<float, 3> localStart{start[0] - center[0], start[1] - center[1], start[2] - center[2]};
    const std::array<float, 3> localEnd{end[0] - center[0], end[1] - center[1], end[2] - center[2]};

    Components::Name name{};
    std::memset(name.value, 0, sizeof(name.value));
    std::strncpy(name.value, "Measure", sizeof(name.value) - 1);
    world->AddComponentImmediate(entity, name);

    world->AddComponentImmediate(entity, makeTransform(center));

    ECS::EntityHandle startEntity = world->CreateEntity();
    ECS::EntityHandle endEntity = world->CreateEntity();
    if (!startEntity.IsValid() || !endEntity.IsValid())
        return entity;

    Components::Parent startParent{};
    startParent.parent = entity;
    Components::Parent endParent{};
    endParent.parent = entity;

    Components::HierarchyOrder startOrder{};
    startOrder.order = 0;
    Components::HierarchyOrder endOrder{};
    endOrder.order = 1;

    world->AddComponentImmediate(startEntity, makeName("Measure Start"));
    world->AddComponentImmediate(startEntity, makeTransform(localStart));
    world->AddComponentImmediate(startEntity, startParent);
    world->AddComponentImmediate(startEntity, startOrder);

    world->AddComponentImmediate(endEntity, makeName("Measure End"));
    world->AddComponentImmediate(endEntity, makeTransform(localEnd));
    world->AddComponentImmediate(endEntity, endParent);
    world->AddComponentImmediate(endEntity, endOrder);

    Components::MeasureComponent measure{};
    const uint32_t measureColor = Editor::SceneViewSettings::Get().GetMeasureColor();
    measure.Color[0] = ((measureColor >> 16) & 0xFF) / 255.0f;
    measure.Color[1] = ((measureColor >> 8) & 0xFF) / 255.0f;
    measure.Color[2] = (measureColor & 0xFF) / 255.0f;
    measure.Color[3] = ((measureColor >> 24) & 0xFF) / 255.0f;
    measure.Start[0] = start[0];
    measure.Start[1] = start[1];
    measure.Start[2] = start[2];
    measure.End[0] = end[0];
    measure.End[1] = end[1];
    measure.End[2] = end[2];
    measure.StartEntity = startEntity;
    measure.EndEntity = endEntity;
    measure.Is2D = is2D;
    world->AddComponentImmediate(entity, measure);

    if (auto* undo = (m_Context ? m_Context->UndoRedo : nullptr))
    {
        undo->CommitAlreadyApplied(std::make_unique<CreateMeasureEntitiesCommand>(
            world,
            m_ChangeNotifications,
            std::vector<ECS::EntityHandle>{entity, startEntity, endEntity}));
    }

    NotifyWorldStructure(m_ChangeNotifications, world);
    if (m_Context && m_Context->OnSceneDirty)
        m_Context->OnSceneDirty();
    if (m_Controller)
        m_Controller->OnEntityPicked(entity);

    return entity;
}

void SceneViewPanel::FinishMeasureDrag(bool allowCreateEntity)
{
    if (!m_MeasureDragActive)
        return;

    const size_t slotIndex = static_cast<size_t>(m_MeasureDragSlot);
    if (slotIndex < m_MeasureOverlays.size() && m_MeasureOverlays[slotIndex])
        m_MeasureOverlays[slotIndex]->EndMeasure();
    if (allowCreateEntity && m_MeasureCreateEntitiesMode)
        CreateMeasureEntity(m_MeasureDragStartWorld, m_MeasureDragEndWorld, m_MeasureDragIs2D);
    m_MeasureDragActive = false;
}

void SceneViewPanel::FinishMeasureDragIfReleased()
{
    if (!m_MeasureDragActive)
        return;

    if (Platform::Window::IsLeftMouseButtonDown())
        return;

    FinishMeasureDrag(true);
}

bool SceneViewPanel::HandleMeasurePointerDown(SceneViewController* controller,
                                              UIElement* viewport,
                                              ViewportSlot slot,
                                              UIEvent& ev)
{
    if (!m_MeasureToolEnabled || !controller || !viewport || ev.Button != 0)
        return false;

    const size_t slotIndex = static_cast<size_t>(slot);
    if (slotIndex >= m_MeasureOverlays.size() || !m_MeasureOverlays[slotIndex])
        return false;

    using namespace Editor::SceneTools;
    float localX = ev.X - viewport->GetLayoutX();
    float localY = ev.Y - viewport->GetLayoutY();
    const float viewW = viewport->GetLayoutWidth();
    const float viewH = viewport->GetLayoutHeight();
    ClampPointerToViewport(localX, localY, viewW, viewH);

    ScenePointerEvent toolEv{};
    toolEv.viewX = localX;
    toolEv.viewY = localY;
    toolEv.viewW = viewW;
    toolEv.viewH = viewH;
    toolEv.ray = controller->MakeGizmoRay(localX, localY, viewW, viewH);
    controller->PopulatePointerCameraState(toolEv);
    toolEv.button = PointerButton::Left;
    toolEv.phase = PointerPhase::Down;
    PopulateScenePointerMods(toolEv, ev.Mods);

    if (controller->HasMeasureEndpointAtPointer(toolEv))
    {
        m_MeasureToolEnabled = false;
        m_MeasureDragActive = false;
        for (SceneViewMeasureOverlay* overlay : m_MeasureOverlays)
        {
            if (!overlay)
                continue;
            overlay->SetMeasureToolEnabled(false);
            overlay->CancelMeasure();
        }

        for (size_t i = 0; i < m_MeasureSavedControllerState.size(); ++i)
        {
            SceneViewController* savedController = GetSceneControllerForSlot(static_cast<ViewportSlot>(i));
            if (!savedController || !m_MeasureSavedControllerState[i])
                continue;

            if (savedController == controller)
            {
                savedController->SetTransformGizmosVisible(true);
                savedController->SetTransformMode(Editor::SceneTools::TransformMode::Translate);
            }
            else
            {
                savedController->SetTransformGizmosVisible(m_MeasureSavedTransformGizmosVisible[i]);
                savedController->SetActiveTool(static_cast<SceneViewController::ToolKind>(m_MeasureSavedToolKinds[i]));
            }
            m_MeasureSavedControllerState[i] = false;
        }

        controller->SetTransformGizmosVisible(true);
        controller->SetTransformMode(Editor::SceneTools::TransformMode::Translate);
        UpdateMeasureToolButtons();
        UpdateToolOverlayModeButtons();
        controller->GetToolContext().HandlePointerEvent(toolEv);
        ev.Stop();
        return true;
    }

    m_MeasureDragActive = true;
    m_MeasureDragSlot = slot;
    m_MeasureDragStartWorld = MeasurePointFromPointer(controller, toolEv);
    m_MeasureDragEndWorld = m_MeasureDragStartWorld;
    m_MeasureDragIs2D = controller->Is2DMode();
    m_MeasureOverlays[slotIndex]->BeginMeasure(localX,
                                               localY,
                                               m_MeasureDragStartWorld,
                                               m_MeasureDragIs2D,
                                               toolEv.shift);
    ev.Stop();
    return true;
}

bool SceneViewPanel::HandleMeasurePointerMove(SceneViewController* controller,
                                              UIElement* viewport,
                                              ViewportSlot slot,
                                              UIEvent& ev)
{
    if (!m_MeasureToolEnabled || !m_MeasureDragActive || m_MeasureDragSlot != slot ||
        !controller || !viewport)
        return false;

    const size_t slotIndex = static_cast<size_t>(slot);
    if (slotIndex >= m_MeasureOverlays.size() || !m_MeasureOverlays[slotIndex])
        return false;

    using namespace Editor::SceneTools;
    float localX = ev.X - viewport->GetLayoutX();
    float localY = ev.Y - viewport->GetLayoutY();
    const float viewW = viewport->GetLayoutWidth();
    const float viewH = viewport->GetLayoutHeight();
    ClampPointerToViewport(localX, localY, viewW, viewH);

    ScenePointerEvent toolEv{};
    toolEv.viewX = localX;
    toolEv.viewY = localY;
    toolEv.viewW = viewW;
    toolEv.viewH = viewH;
    toolEv.ray = controller->MakeGizmoRay(localX, localY, viewW, viewH);
    controller->PopulatePointerCameraState(toolEv);
    toolEv.button = PointerButton::Left;
    toolEv.phase = PointerPhase::Move;
    PopulateScenePointerMods(toolEv, ev.Mods);

    m_MeasureDragEndWorld = MeasurePointFromPointer(controller, toolEv);
    m_MeasureOverlays[slotIndex]->UpdateMeasure(localX,
                                                localY,
                                                m_MeasureDragEndWorld,
                                                toolEv.shift);
    ev.Stop();
    return true;
}

bool SceneViewPanel::HandleMeasurePointerUp(SceneViewController* controller,
                                            UIElement* viewport,
                                            ViewportSlot slot,
                                            UIEvent& ev)
{
    if (!m_MeasureToolEnabled || !m_MeasureDragActive || ev.Button != 0 || m_MeasureDragSlot != slot)
        return false;

    const size_t slotIndex = static_cast<size_t>(slot);
    bool haveReleasePoint = false;
    if (controller && viewport)
    {
        using namespace Editor::SceneTools;
        float localX = ev.X - viewport->GetLayoutX();
        float localY = ev.Y - viewport->GetLayoutY();
        const float viewW = viewport->GetLayoutWidth();
        const float viewH = viewport->GetLayoutHeight();
        ClampPointerToViewport(localX, localY, viewW, viewH);
        haveReleasePoint = true;
        ScenePointerEvent toolEv{};
        toolEv.viewX = localX;
        toolEv.viewY = localY;
        toolEv.viewW = viewW;
        toolEv.viewH = viewH;
        toolEv.ray = controller->MakeGizmoRay(localX, localY, viewW, viewH);
        controller->PopulatePointerCameraState(toolEv);
        toolEv.button = PointerButton::Left;
        toolEv.phase = PointerPhase::Up;
        PopulateScenePointerMods(toolEv, ev.Mods);
        m_MeasureDragEndWorld = MeasurePointFromPointer(controller, toolEv);
        if (slotIndex < m_MeasureOverlays.size() && m_MeasureOverlays[slotIndex])
            m_MeasureOverlays[slotIndex]->UpdateMeasure(localX, localY, m_MeasureDragEndWorld, toolEv.shift);
    }

    FinishMeasureDrag(haveReleasePoint);
    ev.Stop();
    return true;
}

// ─── Scene Tool Overlay (floating over viewport) ────────────────────────────

void SceneViewPanel::WireToolOverlay()
{
    if (!GetActiveViewportElement() || !m_Controller)
        return;

    ApplySceneToolIconBackgrounds();
    PopulateRegisteredToolStripEntries();
    PrepareToolOverlayReveal();

    m_Controller->SetOnActiveToolChangedCallback([this](SceneViewController::ToolKind) {
        UpdateToolOverlayModeButtons();
    });

    // Wire selection tool button (Q)
    if (auto* btn = dynamic_cast<Button*>(FindById("SelectionToolBtn")))
    {
        btn->SetTooltip("Select (Q)");
        btn->SetTooltipPlacement(UIElement::TooltipPlacement::Right);
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
            if (m_Controller)
            {
                if (m_MeasureToolEnabled)
                    SetMeasureToolEnabled(false);
                m_Controller->SetActiveTool(SceneViewController::ToolKind::Selection);
                UpdateToolOverlayModeButtons();
            }
        });
    }

    using Mode = Editor::SceneTools::TransformMode;

    // Wire mode buttons (translate / rotate / scale)
    static const struct { const char* id; Mode mode; const char* tooltip; } kModeButtons[] = {
        {"SelectModeBtn",    Mode::Select,    "Select (Q) — drag to move toolbar. Right-click for options."},
        {"TranslateModeBtn", Mode::Translate, "Translate (W)"},
        {"RotateModeBtn",    Mode::Rotate,    "Rotate (E)"},
        {"ScaleModeBtn",     Mode::Scale,     "Scale (R)"},
    };

    for (const auto& entry : kModeButtons)
    {
        if (auto* btn = dynamic_cast<Button*>(FindById(entry.id)))
        {
            btn->SetTooltip(entry.tooltip);
            btn->SetTooltipPlacement(UIElement::TooltipPlacement::Right);
            btn->RegisterEventHandler(kEventButtonClick, [this, mode = entry.mode](UIEvent&) {
                // Swallow the click that would otherwise fire at the end of a
                // drag-to-reposition gesture on the Select button.
                if (m_ToolOverlayDrag.active)
                    return;
                if (m_Controller)
                {
                    if (m_MeasureToolEnabled)
                        SetMeasureToolEnabled(false);
                    m_Controller->SetTransformMode(mode);
                    UpdateToolOverlayModeButtons();
                }
            });
        }
    }

    // Right-click on the Select (arrow) button opens a marquee options menu
    // (shape, outline color, outline thickness) — overlay and docked mirror alike.
    static const char* kSelectButtons[] = {
        "SelectModeBtn",
        "InlineSelectBtn"
    };
    for (const char* id : kSelectButtons)
    {
        if (auto* btn = dynamic_cast<Button*>(FindById(id)))
        {
            auto menu = ContextMenuManipulator::Create(BuildSelectToolMenuItems());
            menu->SetAnchor(ToolButtonMenuAnchor);
            btn->AddManipulator(std::move(menu));
        }
    }

    // Drag-to-reposition the floating tool overlay by pressing and dragging the
    // Select (arrow) button past a small pixel threshold.
    if (auto* selectBtn = dynamic_cast<Button*>(FindById("SelectModeBtn")))
    {
        UIElement* overlay = FindById("SceneToolOverlay");
        if (overlay)
        {
            // Apply user preference for borderless overlay (Editor preferences).
            {
                const bool borderless = Editor::SceneViewSettings::Get().GetToolOverlayBorderless();
                if (borderless && !overlay->HasClass("borderless"))
                    overlay->AddClass("borderless");
                else if (!borderless && overlay->HasClass("borderless"))
                    overlay->RemoveClass("borderless");
            }

            // Restore persisted overlay position/orientation from editor
            // preferences (global across projects).
            {
                Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
                std::string err;
                (void)prefs.Load(&err);

                double savedX = 0.0;
                double savedY = 0.0;
                bool   savedHorizontal = false;
                const bool hasX = prefs.TryGetDouble("sceneView.toolOverlay.x", savedX);
                const bool hasY = prefs.TryGetDouble("sceneView.toolOverlay.y", savedY);
                (void)prefs.TryGetBool("sceneView.toolOverlay.horizontal", savedHorizontal);

                if (savedHorizontal && !overlay->HasClass("horizontal"))
                    overlay->AddClass("horizontal");
                else if (!savedHorizontal && overlay->HasClass("horizontal"))
                    overlay->RemoveClass("horizontal");

                if (hasX && hasY)
                {
                    overlay->Overrides()
                        .Set(Style::Position,     PositionType::Absolute)
                        .Set(Style::PositionLeft, StyleLength::Px(static_cast<float>(savedX)))
                        .Set(Style::PositionTop,  StyleLength::Px(static_cast<float>(savedY)));
                }
            }

            selectBtn->RegisterEventHandler(kEventMouseDown, [this, overlay](UIEvent& e) {
                if (e.Button != 0) return;
                m_ToolOverlayDrag.pressed       = true;
                m_ToolOverlayDrag.active        = false;
                m_ToolOverlayDrag.startMouseX   = e.X;
                m_ToolOverlayDrag.startMouseY   = e.Y;
                m_ToolOverlayDrag.startOverlayX = overlay->GetLayoutX();
                m_ToolOverlayDrag.startOverlayY = overlay->GetLayoutY();
                // Button::OnEvent already Capture(this)+Stop() for LMB — no extra handling needed.
            });

            selectBtn->RegisterEventHandler(kEventMouseMove, [this, overlay](UIEvent& e) {
                if (!m_ToolOverlayDrag.pressed) return;

                const float dx = e.X - m_ToolOverlayDrag.startMouseX;
                const float dy = e.Y - m_ToolOverlayDrag.startMouseY;

                if (!m_ToolOverlayDrag.active)
                {
                    constexpr float kDragThresholdPx = 5.0f;
                    if ((dx * dx + dy * dy) < (kDragThresholdPx * kDragThresholdPx))
                        return;
                    m_ToolOverlayDrag.active = true;
                }

                // Convert absolute drag delta into viewport-local absolute
                // position (the overlay lives inside the current overlay viewport).
                float newX = m_ToolOverlayDrag.startOverlayX + dx;
                float newY = m_ToolOverlayDrag.startOverlayY + dy;
                if (UIElement* overlayViewport = overlay ? overlay->GetParent() : nullptr)
                {
                    newX -= overlayViewport->GetLayoutX();
                    newY -= overlayViewport->GetLayoutY();

                    // Toggle horizontal orientation based on the cursor's Y in
                    // viewport-local space. Use a hysteresis band so the flip
                    // doesn't oscillate when the orientation change resizes the
                    // overlay across the threshold:
                    //   • enter horizontal once the cursor is below 80% of vh
                    //   • leave horizontal only after it rises above 75% of vh
                    // Driven by the cursor (not the overlay center) so it is
                    // independent of the post-flip overlay dimensions.
                    const float vh = overlayViewport->GetLayoutHeight();
                    if (vh > 0.0f)
                    {
                        const float cursorYLocal = e.Y - overlayViewport->GetLayoutY();
                        const bool isHorizontal  = overlay->HasClass("horizontal");
                        const float enterY = vh * 0.80f;
                        const float exitY  = vh * 0.75f;
                        bool wantHorizontal = isHorizontal;
                        if (!isHorizontal && cursorYLocal > enterY)
                            wantHorizontal = true;
                        else if (isHorizontal && cursorYLocal < exitY)
                            wantHorizontal = false;

                        if (wantHorizontal != isHorizontal)
                        {
                            if (wantHorizontal)
                                overlay->AddClass("horizontal");
                            else
                                overlay->RemoveClass("horizontal");
                        }
                    }

                    // Clamp so the overlay stays inside the viewport. Re-read
                    // dimensions after the orientation toggle.
                    const float vw   = overlayViewport->GetLayoutWidth();
                    const float ovw2 = overlay->GetLayoutWidth();
                    const float ovh2 = overlay->GetLayoutHeight();
                    const float maxX = std::max(0.0f, vw - ovw2);
                    const float maxY = std::max(0.0f, vh - ovh2);
                    newX = std::clamp(newX, 0.0f, maxX);
                    newY = std::clamp(newY, 0.0f, maxY);
                }

                overlay->Overrides()
                    .Set(Style::Position,     PositionType::Absolute)
                    .Set(Style::PositionLeft, StyleLength::Px(newX))
                    .Set(Style::PositionTop,  StyleLength::Px(newY));
                /* RequestRelayout forces the heavy pass in the same frame so absolute Left/Top
                   overrides apply visually while dragging, even if UIManager would otherwise
                   defer the solve for a pointer-only frame. */
                overlay->RequestRelayout();
                e.Stop();
            });

            selectBtn->RegisterEventHandler(kEventMouseUp, [this, overlay](UIEvent& e) {
                if (e.Button != 0) return;
                const bool wasDragActive = m_ToolOverlayDrag.active;

                // Reset after Button::OnEvent has already decided whether to
                // fire its click callback; the click lambda gates on .active.
                m_ToolOverlayDrag.pressed = false;
                m_ToolOverlayDrag.active  = false;

                if (!wasDragActive || !overlay)
                    return;
                UIElement* overlayViewport = overlay->GetParent();
                if (!overlayViewport)
                    return;

                // Persist final position and orientation to editor preferences
                // (global, not per project).
                const float localX = overlay->GetLayoutX() - overlayViewport->GetLayoutX();
                const float localY = overlay->GetLayoutY() - overlayViewport->GetLayoutY();

                Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
                std::string err;
                (void)prefs.Load(&err);
                prefs.SetDouble("sceneView.toolOverlay.x", static_cast<double>(localX));
                prefs.SetDouble("sceneView.toolOverlay.y", static_cast<double>(localY));
                prefs.SetBool("sceneView.toolOverlay.horizontal", overlay->HasClass("horizontal"));
                (void)prefs.Save(&err);
            });
        }
    }

    // Wire inline toolbar mode buttons (mirrors of overlay buttons)
    static const struct { const char* id; Mode mode; const char* tooltip; } kInlineButtons[] = {
        {"InlineSelectBtn",    Mode::Select,    "Select (Q) — right-click for options."},
        {"InlineTranslateBtn", Mode::Translate, "Translate (W)"},
        {"InlineRotateBtn",    Mode::Rotate,    "Rotate (E)"},
        {"InlineScaleBtn",     Mode::Scale,     "Scale (R)"},
    };

    for (const auto& entry : kInlineButtons)
    {
        if (auto* btn = dynamic_cast<Button*>(FindById(entry.id)))
        {
            btn->SetTooltip(entry.tooltip);
            btn->RegisterEventHandler(kEventButtonClick, [this, mode = entry.mode](UIEvent&) {
                if (m_Controller)
                {
                    if (m_MeasureToolEnabled)
                        SetMeasureToolEnabled(false);
                    m_Controller->SetTransformMode(mode);
                    UpdateToolOverlayModeButtons();
                }
            });
        }
    }

    static const char* kMeasureButtons[] = {
        "MeasureToolBtn",
        "InlineMeasureBtn"
    };
    for (const char* id : kMeasureButtons)
    {
        if (auto* btn = dynamic_cast<Button*>(FindById(id)))
        {
            btn->SetTooltip("Ruler Mode (M): drag to measure. Right-click for options.");
            if (std::string_view(id) == "MeasureToolBtn")
                btn->SetTooltipPlacement(UIElement::TooltipPlacement::Right);
            btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
                ToggleMeasureTool();
            });
            auto menu = ContextMenuManipulator::Create(BuildMeasureMenuItems());
            menu->SetAnchor(ToolButtonMenuAnchor);
            btn->AddManipulator(std::move(menu));
        }
    }

    // Ensure the scene view always starts with a real transform mode active so
    // the overlay reflects an actually-active tool/mode from frame 1. Default
    // is the Translate mode of the Transform tool.
    if (m_Controller->GetActiveToolKind() != SceneViewController::ToolKind::Transform)
        m_Controller->SetActiveTool(SceneViewController::ToolKind::Transform);
    m_Controller->SetTransformMode(Mode::Translate);

    UpdateToolOverlayModeButtons();
    UpdateMeasureToolButtons();

    // Wire space toggle button (in horizontal toolbar)
    if (auto* btn = dynamic_cast<Button*>(FindById("ToolSpaceToggle")))
    {
        btn->SetTooltip("Toggle Transform Space (X)");
        UpdateToolOverlaySpaceButton();
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
            if (m_Controller)
            {
                m_Controller->ToggleTransformSpace();
                UpdateToolOverlaySpaceButton();
            }
        });
    }

    // Wire Maya-style four-view layout toggle. Space remains the quick viewport toggle.
    if (auto* btn = dynamic_cast<Button*>(FindById("QuadViewToggle")))
    {
        btn->SetTooltip("Toggle Four View Layout (Space). Right-click for options.");
        UpdateQuadViewButtonState();
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
            ToggleQuadView();
        });
        btn->AddManipulator(
            ContextMenuManipulator::Create([this] { return BuildQuadViewMenuItems(); }));
    }

    // Subscribe to keyboard-driven changes so the overlay stays in sync.
    if (auto* tool = m_Controller->GetTransformTool())
    {
        tool->SetOnModeChangedCallback([this](Mode) {
            UpdateToolOverlayModeButtons();
        });
        tool->SetOnAxisSpaceChangedCallback(
            [this](Editor::SceneTools::TransformAxisSpace) {
                UpdateToolOverlaySpaceButton();
            });
    }

    // Apply persisted toolbar icon visibility and wire right-click menu for toggling.
    ApplyToolbarIconVisibility();
    if (m_Toolbar)
    {
        m_Toolbar->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
            if (e.Button != Input::kMouseButton_Right)
                return;
            // Only the toolbar background itself opens the icon-visibility menu.
            // Right-clicks that started on a child element (a Button) must either
            // be consumed by that button's own right-click handler or ignored
            // entirely — they should not fall through to this menu.
            if (e.Target != m_Toolbar)
                return;
            ShowToolbarIconContextMenu(e.X, e.Y);
            e.Stop();
        });
    }
}

void SceneViewPanel::ApplySceneToolIconBackgrounds()
{
    static constexpr struct
    {
        const char* id;
        std::string_view path;
    } kButtons[] = {
        {"SelectModeBtn",     "Icons/arrowmouse.png"},
        {"InlineSelectBtn",   "Icons/arrowmouse.png"},
        {"TranslateModeBtn",  "Icons/GiszmoTranslate.png"},
        {"InlineTranslateBtn", "Icons/GiszmoTranslate.png"},
        {"RotateModeBtn",     "Icons/GizmoRotate.png"},
        {"InlineRotateBtn",   "Icons/GizmoRotate.png"},
        {"ScaleModeBtn",      "Icons/Giszmo_scale.png"},
        {"InlineScaleBtn",    "Icons/Giszmo_scale.png"},
        {"MeasureToolBtn",    "Icons/Ruler.png"},
        {"InlineMeasureBtn",  "Icons/Ruler.png"},
    };

    for (const auto& entry : kButtons)
    {
        if (UIElement* button = FindById(entry.id))
            SetEditorBackgroundPath(*button, entry.path);
    }
}

void SceneViewPanel::PrepareToolOverlayReveal()
{
    UIElement* overlay = FindById("SceneToolOverlay");
    UIManager* ui = GetOwnerManager();
    if (!overlay || !ui)
        return;

    const uint32_t generation = ++m_ToolOverlayRevealGeneration;

    // Keep the floating shell and its image-backed buttons hidden together
    // until the tool icons are ready. Otherwise startup can show an empty
    // overlay for a frame while image loads complete.
    overlay->AddClass("hidden");

    auto* toggle = FindById("ToolOverlayToggle");
    const bool overlayEnabled = !toggle || toggle->HasClass("icon-active");
    if (!overlayEnabled)
        return;

    struct RevealGate
    {
        size_t remaining = 0;
        bool revealed = false;
    };

    std::vector<std::string_view> iconPaths(kSceneToolOverlayIconPaths.begin(), kSceneToolOverlayIconPaths.end());
    for (const Editor::SceneViewToolStripEntry& entry : Editor::SceneViewToolStripRegistry::Get().Entries())
        iconPaths.push_back(EditorIconAssetPath(entry.Icon));

    auto gate = std::make_shared<RevealGate>();
    gate->remaining = iconPaths.size();

    auto completeOne = [this, gate, generation]() {
        if (gate->revealed || gate->remaining == 0)
            return;
        --gate->remaining;
        if (gate->remaining != 0)
            return;

        gate->revealed = true;
        PostSafeAction([this, generation]() {
            RevealToolOverlayIfEnabled(generation);
        });
    };

    for (std::string_view path : iconPaths)
    {
        const GUID guid = ui->ResolveBackgroundImagePath(
            std::string(path), std::string(GameEngine::kAssetSourceAliasEditor));
        if (guid.IsNull())
        {
            completeOne();
            continue;
        }

        ui->RequestBackgroundTexture(
            guid,
            [completeOne](Rendering::TextureHandle, uint32_t, uint32_t) mutable {
                completeOne();
            });
    }

    // Safety fallback: some texture paths can already be in a state where no
    // readiness callback is delivered to this gate. Give the UI a couple of
    // deferred turns for the warm-up above, then reveal anyway so the overlay
    // never gets stuck hidden.
    PostSafeAction([this, generation]() {
        PostSafeAction([this, generation]() {
            RevealToolOverlayIfEnabled(generation);
        });
    });
}

void SceneViewPanel::RevealToolOverlayIfEnabled(uint32_t generation)
{
    if (generation != m_ToolOverlayRevealGeneration)
        return;

    UIElement* overlay = FindById("SceneToolOverlay");
    if (!overlay)
        return;

    auto* toggle = FindById("ToolOverlayToggle");
    const bool overlayEnabled = !toggle || toggle->HasClass("icon-active");
    if (overlayEnabled)
        overlay->RemoveClass("hidden");
}

void SceneViewPanel::ApplyToolbarIconVisibility()
{
    if (!m_Toolbar)
        return;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    for (const auto& entry : kSceneToolbarIconToggles)
    {
        UIElement* el = m_Toolbar->FindById(entry.id);
        if (!el)
            continue;
        bool visible = true;
        prefs.TryGetBool(entry.prefKey, visible);
        if (visible)
            el->RemoveClass("hidden");
        else
            el->AddClass("hidden");
    }
}

void SceneViewPanel::ShowToolbarIconContextMenu(float windowX, float windowY)
{
    if (!m_Window || !m_Toolbar)
        return;

    if (!m_ContextMenu)
    {
        m_ContextMenu = CreateContextMenu();
        if (!m_ContextMenu)
            return;
    }

    m_ContextMenu->SetCommandHandler([this](uint32_t cmd) {
        if (cmd == kCmdSceneToolbarFpsToggle)
        {
            m_FpsVisible = !m_FpsVisible;
            if (m_FpsLabel)
            {
                auto& style = m_FpsLabel->Overrides();
                if (m_FpsVisible)
                {
                    style.Reset(Style::Opacity);
                    style.Reset(Style::PointerEvents);
                }
                else
                {
                    style.Set(Style::Opacity, 0.0f);
                    style.Set(Style::PointerEvents, false);
                }
            }
            {
                Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
                std::string err;
                (void)prefs.Load(&err);
                prefs.SetBool("sceneView.fpsLabel.visible", m_FpsVisible);
                (void)prefs.Save(&err);
            }
            return;
        }

        const uint32_t index = cmd - kCmdSceneToolbarToggleBase;
        constexpr uint32_t kCount = static_cast<uint32_t>(std::size(kSceneToolbarIconToggles));
        if (index >= kCount)
            return;
        const auto& entry = kSceneToolbarIconToggles[index];

        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        bool visible = true;
        prefs.TryGetBool(entry.prefKey, visible);
        visible = !visible;
        prefs.SetBool(entry.prefKey, visible);
        prefs.Save(&err);

        if (UIElement* el = m_Toolbar ? m_Toolbar->FindById(entry.id) : nullptr)
        {
            if (visible)
                el->RemoveClass("hidden");
            else
                el->AddClass("hidden");
        }
    });

    m_ContextMenu->Clear();

    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);

    // Add items directly at the root so labels containing '/' (e.g. "2D / 3D")
    // aren't split into submenus by ContextMenuBuilder's path parser.
    uint32_t index = 0;
    for (const auto& entry : kSceneToolbarIconToggles)
    {
        bool visible = true;
        prefs.TryGetBool(entry.prefKey, visible);
        const uint32_t flags = visible ? MenuItemFlag_Checked : MenuItemFlag_None;
        m_ContextMenu->AddItem(0, entry.label, kCmdSceneToolbarToggleBase + index, flags);
        m_ContextMenu->SetItemIcon(kCmdSceneToolbarToggleBase + index, EditorIcons::kEye);
        ++index;
    }

    // FPS overlay — panel-local state, not a persisted toolbar-icon preference,
    // so it lives below the toolbar toggles with a visual separator.
    m_ContextMenu->AddSeparator(0);
    const uint32_t fpsFlags = m_FpsVisible ? MenuItemFlag_Checked : MenuItemFlag_None;
    m_ContextMenu->AddItem(0, "FPS Overlay", kCmdSceneToolbarFpsToggle, fpsFlags);
    m_ContextMenu->SetItemIcon(kCmdSceneToolbarFpsToggle, EditorIcons::kStats);

    m_ContextMenu->Show(m_Window, static_cast<int>(windowX), static_cast<int>(windowY));
}

void SceneViewPanel::ShowFpsContextMenu(float windowX, float windowY)
{
    if (!m_Window)
        return;

    if (!m_ContextMenu)
    {
        m_ContextMenu = CreateContextMenu();
        if (!m_ContextMenu)
            return;
    }

    auto* renderServices = ResolveRenderServices();
    auto* device = renderServices ? renderServices->GetDevice() : nullptr;
    if (!device)
        return;

    m_ContextMenu->SetCommandHandler([this](uint32_t cmd) {
        if (cmd != kCmdSceneFpsToggleVsync)
            return;

        auto* rs = ResolveRenderServices();
        auto* dev = rs ? rs->GetDevice() : nullptr;
        if (!dev)
            return;

        const bool enabled = !dev->IsVsyncEnabled();
        dev->SetVsync(enabled);

        Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
        std::string err;
        (void)prefs.Load(&err);
        prefs.SetBool("performance.vsync", enabled);
        (void)prefs.Save(&err);
    });

    m_ContextMenu->Clear();
    m_ContextMenu->AddItem(0,
                           "VSync",
                           kCmdSceneFpsToggleVsync,
                           device->IsVsyncEnabled() ? MenuItemFlag_Checked : MenuItemFlag_None);
    m_ContextMenu->SetItemIcon(kCmdSceneFpsToggleVsync, EditorIcons::kAlarm);
    m_ContextMenu->Show(m_Window, static_cast<int>(windowX), static_cast<int>(windowY));
}

void SceneViewPanel::ShowCameraSettingsPopup(float windowX, float windowY)
{
    if (!m_CameraSettingsPopup)
    {
        auto popup = std::make_unique<SceneCameraSettingsPopup>();
        m_CameraSettingsPopup = popup.get();
        AddChild(std::move(popup));
    }
    if (m_CameraSettingsPopup->IsVisible())
    {
        m_CameraSettingsPopup->Hide();
        return;
    }
    // The anchor arrives in window coordinates; the popup positions its panel
    // in this panel's local space (layout rects are window-absolute). Dismissal
    // is window-wide regardless — UIManager's dismissable-popup registry owns
    // it, so nothing here needs the window rect.
    const float localX = windowX - GetLayoutX();
    const float localY = windowY - GetLayoutY();
    m_CameraSettingsPopup->ShowAt(localX, localY, GetLayoutWidth(), GetLayoutHeight());
}

void SceneViewPanel::RefreshCameraSettingsPopupLayout()
{
    if (!m_CameraSettingsPopup || !m_CameraSettingsPopup->IsVisible() || !m_Toolbar)
        return;

    UIElement* anchor = m_Toolbar->FindById("CameraSettingsBtn");
    if (!anchor)
        return;

    const float localX = anchor->GetLayoutX() + anchor->GetLayoutWidth() * 0.5f - GetLayoutX();
    const float localY = anchor->GetLayoutY() + anchor->GetLayoutHeight() - GetLayoutY();
    m_CameraSettingsPopup->UpdatePlacement(localX, localY,
                                           GetLayoutWidth(), GetLayoutHeight());
}

void SceneViewPanel::SyncCameraSettingsPopupFromSettings()
{
    if (m_CameraSettingsPopup && m_CameraSettingsPopup->IsVisible())
        m_CameraSettingsPopup->SyncFromSettings();
}

void SceneViewPanel::PopulateRegisteredToolStripEntries()
{
    Editor::RegisteredToolStripActions actions;
    actions.ToggleTool = [this](const std::string& id) { ToggleRegisteredTool(id); };
    actions.SetIcon = &SetEditorIcon;
    actions.ColorPicker = [this]() { return m_OpenColorPickerWindow; };
    actions.MenuAnchor = &ToolButtonMenuAnchor;
    const Editor::SceneViewToolStripRegistry& registry = Editor::SceneViewToolStripRegistry::Get();
    if (UIElement* overlay = FindById("SceneToolOverlay"))
        m_FloatingToolStrip.Populate(*overlay, Editor::ToolStripPlacement::Floating, registry, actions);
    if (UIElement* inlineButtons = FindById("InlineToolButtons"))
        m_InlineToolStrip.Populate(*inlineButtons, Editor::ToolStripPlacement::Inline, registry, actions);
    UpdateRegisteredToolStripEntries();
}

void SceneViewPanel::ToggleRegisteredTool(const std::string& id)
{
    if (!m_Controller || !Editor::RegisteredToolRefusal(Editor::SceneViewToolStripRegistry::Get(), id, &m_Controller->GetWorld()).empty())
        return;
    if (m_MeasureToolEnabled)
        SetMeasureToolEnabled(false);
    if (m_Controller->GetActiveRegisteredToolId() == id)
        m_Controller->SetActiveTool(SceneViewController::ToolKind::Transform);
    else
        m_Controller->SetActiveRegisteredTool(id);
    UpdateToolOverlayModeButtons();
}

void SceneViewPanel::UpdateRegisteredToolStripEntries()
{
    if (!m_Controller)
        return;
    const std::string_view activeId = m_MeasureToolEnabled ? std::string_view() : m_Controller->GetActiveRegisteredToolId();
    m_FloatingToolStrip.SetActiveEntry(activeId);
    m_InlineToolStrip.SetActiveEntry(activeId);
    RefreshRegisteredToolStrips();
}

void SceneViewPanel::RefreshRegisteredToolStrips()
{
    if (!m_Controller)
        return;
    // An entry that stops being available (the last terrain deleted) takes its tool with it;
    // the tool change refreshes the strips again.
    if (const std::string_view activeId = m_Controller->GetActiveRegisteredToolId(); !activeId.empty())
    {
        const Editor::SceneViewToolStripEntry* active = Editor::SceneViewToolStripRegistry::Get().Find(activeId);
        if (active && active->IsAvailable && !active->IsAvailable(&m_Controller->GetWorld()))
        {
            m_Controller->SetActiveTool(SceneViewController::ToolKind::Transform);
            return;
        }
    }
    // Each strip refreshes only while it shows.
    m_FloatingToolStrip.Refresh(&m_Controller->GetWorld());
    m_InlineToolStrip.Refresh(&m_Controller->GetWorld());
}

void SceneViewPanel::UpdateToolOverlayModeButtons()
{
    if (!GetActiveViewportElement() || !m_Controller)
        return;

    UpdateRegisteredToolStripEntries();

    const bool allowSceneTools = !m_MeasureToolEnabled;
    const bool isSelectionTool =
        allowSceneTools && m_Controller->GetActiveToolKind() == SceneViewController::ToolKind::Selection;

    // Selection tool button
    if (auto* btn = dynamic_cast<Button*>(FindById("SelectionToolBtn")))
    {
        if (isSelectionTool)
            btn->AddClass("icon-active");
        else
            btn->RemoveClass("icon-active");
    }

    // Transform mode buttons — active only when the transform tool is active
    using Mode = Editor::SceneTools::TransformMode;
    const Mode current = m_Controller->GetTransformMode();
    const bool isTransformTool =
        allowSceneTools && m_Controller->GetActiveToolKind() == SceneViewController::ToolKind::Transform;

    static const struct { const char* id; Mode mode; } kButtons[] = {
        {"SelectModeBtn",    Mode::Select},
        {"TranslateModeBtn", Mode::Translate},
        {"RotateModeBtn",    Mode::Rotate},
        {"ScaleModeBtn",     Mode::Scale},
    };

    for (const auto& entry : kButtons)
    {
        if (auto* btn = dynamic_cast<Button*>(FindById(entry.id)))
        {
            if (isTransformTool && entry.mode == current)
                btn->AddClass("icon-active");
            else
                btn->RemoveClass("icon-active");
        }
    }

    // Sync inline toolbar buttons
    static const struct { const char* id; Mode mode; } kInlineButtons[] = {
        {"InlineSelectBtn",    Mode::Select},
        {"InlineTranslateBtn", Mode::Translate},
        {"InlineRotateBtn",    Mode::Rotate},
        {"InlineScaleBtn",     Mode::Scale},
    };

    for (const auto& entry : kInlineButtons)
    {
        if (auto* btn = dynamic_cast<Button*>(FindById(entry.id)))
        {
            if (isTransformTool && entry.mode == current)
                btn->AddClass("icon-active");
            else
                btn->RemoveClass("icon-active");
        }
    }

    UpdateMeasureToolButtons();
}

void SceneViewPanel::UpdateToolOverlaySpaceButton()
{
    if (!m_Controller)
        return;

    if (auto* btn = dynamic_cast<Button*>(FindById("ToolSpaceToggle")))
    {
        if (m_Controller->IsTransformSpaceLocal())
            btn->AddClass("icon-active");
        else
            btn->RemoveClass("icon-active");
    }
}


void SceneViewPanel::UpdatePerformanceOverlay(double deltaSeconds, const PerfBreakdown& perf)
{
    // Performance overlay removed - method kept for API compatibility
    (void)deltaSeconds;
    (void)perf;
}

void SceneViewPanel::UpdateHoverHighlightPill()
{
    if (!m_HoverHighlightLabel || m_HoverHighlightLabel->GetOwnerManager() == nullptr)
        m_HoverHighlightLabel =
            dynamic_cast<Label*>(FindById("SceneViewHoverHighlightLabel"));
    if (!m_HoverHighlightLabel)
        return;
    UIElement* hoverViewport = m_HoverHighlightLabel->GetParent();
    if (!hoverViewport)
        return;

    auto& ov = m_HoverHighlightLabel->Overrides();

    auto clearHoverPillLayoutCache = [this]() {
        m_LastHoverPillLayoutText.clear();
        m_LastHoverPillViewportWidth = -1.0f;
        m_LastHoverPillAnchoredLabelWidth = -1.0f;
        m_LastHoverPillAnchorMouseLocalX = -1.0f;
        m_LastHoverPillAnchorMouseLocalY = -1.0f;
        m_LastHoverPillUsedCursorPlacement = false;
    };

    // A hover label describes the object under a resting pointer. While any
    // viewport drag is active it is both distracting and expensive to chase
    // the cursor: changing absolute position invalidates UI style/layout every
    // drag frame. Hide it until release; the transform Inspector remains live.
    if (m_Dragging)
    {
        clearHoverPillLayoutCache();
        ov.Set(Style::Opacity, 0.0f);
        return;
    }

    if (!m_Controller)
    {
        clearHoverPillLayoutCache();
        ov.Set(Style::Opacity, 0.0f);
        return;
    }

    Editor::SceneViewSettings& svSettings = Editor::SceneViewSettings::Get();
    if (!svSettings.GetHoverNamePillEnabled())
    {
        clearHoverPillLayoutCache();
        ov.Set(Style::Opacity, 0.0f);
        return;
    }

    if (m_Controller->IsHoverNamePillSuppressedBySource())
    {
        clearHoverPillLayoutCache();
        ov.Set(Style::Opacity, 0.0f);
        return;
    }

    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    ECS::EntityHandle hover = m_Controller->GetHoveredEntity();

    std::string text;
    const bool haveValidHover =
        hover.IsValid() && world != nullptr && world->IsValid(hover);
    if (haveValidHover)
        text = FormatHoverHighlightPillText(*world, hover, m_Controller->GetHoveredDescendants());

    if (text.empty())
    {
        clearHoverPillLayoutCache();
        ov.Set(Style::Opacity, 0.0f);
        return;
    }

    // Depth-cycle indicator: "name · 2/7" while Ctrl+scroll or same-spot
    // clicks are walking the overlapping picks under the cursor.
    {
        int cycleIndex = 0, cycleCount = 0;
        if (m_Controller->GetPickCycleStatus(cycleIndex, cycleCount))
            text += " · " + std::to_string(cycleIndex) + "/" + std::to_string(cycleCount);
    }

    constexpr float kViewportResizeEpsilonPx = 1.0f;
    constexpr float kLabelWidthReanchorEpsilonPx = 1.0f;
    constexpr float kCursorFollowOffsetXPx = 14.0f;
    constexpr float kCursorFollowOffsetYPx = 18.0f;

    const bool textChanged = (text != m_LastHoverPillLayoutText);
    if (textChanged)
    {
        m_LastHoverPillLayoutText = text;
        m_LastHoverPillAnchoredLabelWidth = -1.0f;
        m_HoverHighlightLabel->SetText(text);
        m_HoverHighlightLabel->RequestRelayout();
    }

    const float vwRaw = hoverViewport->GetLayoutWidth();
    const float vhRaw = hoverViewport->GetLayoutHeight();
    const float lwRaw = m_HoverHighlightLabel->GetLayoutWidth();
    const float lhRaw = m_HoverHighlightLabel->GetLayoutHeight();
    const bool viewportSized = vwRaw > 0.0f && lwRaw > 0.0f && lhRaw > 0.0f;

    const bool useCursorPlacement =
        svSettings.GetHoverNamePillNearCursor() && m_SceneViewportPointerInside && haveValidHover;

    const bool viewportResized =
        m_LastHoverPillViewportWidth < 0.0f ||
        std::fabs(vwRaw - m_LastHoverPillViewportWidth) >= kViewportResizeEpsilonPx;

    const bool labelWidthShifted =
        m_LastHoverPillAnchoredLabelWidth < 0.0f ||
        std::fabs(lwRaw - m_LastHoverPillAnchoredLabelWidth) >= kLabelWidthReanchorEpsilonPx;

    const bool placementModeChanged = useCursorPlacement != m_LastHoverPillUsedCursorPlacement;

    bool mouseMovedForCursor = false;
    if (useCursorPlacement)
    {
        mouseMovedForCursor =
            m_LastHoverPillAnchorMouseLocalX < 0.0f ||
            std::fabs(m_SceneViewportPointerLocalX - m_LastHoverPillAnchorMouseLocalX) >= 1.0f ||
            std::fabs(m_SceneViewportPointerLocalY - m_LastHoverPillAnchorMouseLocalY) >= 1.0f;
    }

    const bool needsReanchor = textChanged || viewportResized || labelWidthShifted ||
                               placementModeChanged ||
                               (useCursorPlacement ? mouseMovedForCursor : false);

    if (!viewportSized || !needsReanchor)
    {
        ov.Set(Style::PointerEvents, false).Set(Style::Opacity, 1.0f);
        return;
    }

    if (useCursorPlacement && vhRaw > 0.0f)
    {
        const float vw = std::round(vwRaw);
        const float vh = std::round(vhRaw);
        const float lw = std::round(lwRaw);
        const float lh = std::round(lhRaw);

        float left = std::round(m_SceneViewportPointerLocalX + kCursorFollowOffsetXPx);
        float top = std::round(m_SceneViewportPointerLocalY + kCursorFollowOffsetYPx);
        left = std::clamp(left, 0.0f, std::max(0.0f, vw - lw));
        top = std::clamp(top, 0.0f, std::max(0.0f, vh - lh));

        m_LastHoverPillViewportWidth = vwRaw;
        m_LastHoverPillAnchoredLabelWidth = lwRaw;
        m_LastHoverPillAnchorMouseLocalX = m_SceneViewportPointerLocalX;
        m_LastHoverPillAnchorMouseLocalY = m_SceneViewportPointerLocalY;
        m_LastHoverPillUsedCursorPlacement = true;

        ov.Set(Style::Position, PositionType::Absolute)
            .Set(Style::PositionLeft, StyleLength::Px(left))
            .Set(Style::PositionTop, StyleLength::Px(top))
            .Reset(Style::PositionRight)
            .Reset(Style::PositionBottom)
            .Set(Style::PointerEvents, false)
            .Set(Style::Opacity, 1.0f);
    }
    else
    {
        const float vw = std::round(vwRaw);
        const float lw = std::round(lwRaw);
        const float centerX = std::max(0.0f, std::round((vw - lw) * 0.5f));

        m_LastHoverPillViewportWidth = vwRaw;
        m_LastHoverPillAnchoredLabelWidth = lwRaw;
        m_LastHoverPillAnchorMouseLocalX = -1.0f;
        m_LastHoverPillAnchorMouseLocalY = -1.0f;
        m_LastHoverPillUsedCursorPlacement = false;

        ov.Set(Style::Position, PositionType::Absolute)
            .Set(Style::PositionLeft, StyleLength::Px(centerX))
            .Set(Style::PositionBottom, StyleLength::Px(kSceneViewHudBottomInsetPx))
            .Reset(Style::PositionRight)
            .Reset(Style::PositionTop)
            .Set(Style::PointerEvents, false)
            .Set(Style::Opacity, 1.0f);
    }
}

void SceneViewPanel::UpdateFPS(float deltaTime)
{
    // The registered entries' availability and badges follow state outside the strips.
    RefreshRegisteredToolStrips();

    // A layout reload recreates the viewport; its overlay layer goes with it.
    if (UIElement* viewport = GetViewportForOverlay(); viewport && viewport != m_PublishedOverlayViewport.Get())
        PublishOverlayLayer(*viewport);

    // Update camera bookmarks widget (poll readback requests, etc.)
    // IMPORTANT: do not keep raw UIElement* pointers across UXML hot reload.
    // Layout reconciliation can destroy/recreate elements, so reacquire by id and
    // rebind controller hookups when the instance changes.
    if (m_Controller)
    {
        auto* bookmarks = dynamic_cast<CameraBookmarksWidget*>(FindById("CameraBookmarks"));
        if (bookmarks)
        {
            const uint64_t inst = bookmarks->GetInstanceId();
            if (inst != m_CameraBookmarksBoundInstanceId)
            {
                bookmarks->SetSceneController(m_Controller);
                bookmarks->SetPanel(this);
                m_Controller->SetBookmarksWidget(bookmarks);
                m_CameraBookmarksBoundInstanceId = inst;
            }
            bookmarks->Update();
        }
        else
        {
            if (m_CameraBookmarksBoundInstanceId != 0)
            {
                m_Controller->SetBookmarksWidget(nullptr);
                m_CameraBookmarksBoundInstanceId = 0;
            }
        }

        // Maya: each visible pane has its own orientation gizmo driven by that pane's camera.
        if (ViewportOverlaysNeedSync())
            SyncViewportOverlays();

        RefreshViewportRotationGizmos();
        for (size_t i = 0; i < m_RotationGizmos.size(); ++i)
        {
            ViewportRotationGizmo* gizmo = m_RotationGizmos[i];
            UIElement* viewport = m_ViewportSlots[i];
            if (!gizmo || !viewport || viewport->HasClass("hidden"))
                continue;
            if (SceneViewController* slotController = GetSceneControllerForSlot(static_cast<ViewportSlot>(i)))
                gizmo->SetSceneController(slotController);
            gizmo->SetPanel(this);
            gizmo->Tick();
        }

        // Same re-acquire + tick for per-viewport 2D ruler overlays. Rulers
        // stay with the viewport whose controller is in 2D mode, even when a
        // different split is active.
        RefreshViewportRulerOverlays();
        RefreshViewportMeasureOverlays();
        FinishMeasureDragIfReleased();
        for (size_t i = 0; i < m_RulerOverlays.size(); ++i)
        {
            SceneViewRulerOverlay* ruler = m_RulerOverlays[i];
            UIElement* viewport = m_ViewportSlots[i];
            if (!ruler || !viewport || viewport->HasClass("hidden"))
                continue;
            ruler->Tick();
        }
        for (size_t i = 0; i < m_MeasureOverlays.size(); ++i)
        {
            SceneViewMeasureOverlay* measure = m_MeasureOverlays[i];
            UIElement* viewport = m_ViewportSlots[i];
            if (!measure || !viewport || viewport->HasClass("hidden"))
                continue;
            measure->Tick();
        }

        // When the 2D ruler bands are visible, edge-anchored overlays would
        // sit on top of the ruler ticks. Toggle a "rulers-visible" class on
        // the affected elements so CSS can shift their inset offsets inward.
        // Only active in 2D mode (the rulers themselves are hidden in 3D).
        {
            auto applyRulersClass = [](UIElement* el, bool rulersOn) {
                if (!el) return;
                const bool has = el->HasClass("rulers-visible");
                if (rulersOn && !has) el->AddClass("rulers-visible");
                else if (!rulersOn && has) el->RemoveClass("rulers-visible");
            };
            for (size_t i = 0; i < m_RotationGizmos.size(); ++i)
            {
                SceneViewController* slotController =
                    GetSceneControllerForSlot(static_cast<ViewportSlot>(i));
                const bool rulersOn =
                    slotController && slotController->Is2DMode() &&
                    Editor::SceneViewSettings::Get().GetShowRulers();
                applyRulersClass(m_RotationGizmos[i], rulersOn);
            }
            const bool activeRulersOn =
                m_Controller && m_Controller->Is2DMode() &&
                Editor::SceneViewSettings::Get().GetShowRulers();
            if (UIElement* overlay = FindViewportOverlayById("SceneToolOverlay"))
                applyRulersClass(overlay, activeRulersOn);
        }

        // Honor the SceneView "Borderless Tool Overlay" preference. Polled
        // here so toggling the setting takes effect live across all open
        // scene views without bespoke broadcast plumbing.
        if (m_Viewport)
        {
            if (UIElement* overlay = FindById("SceneToolOverlay"))
            {
                const bool borderless = Editor::SceneViewSettings::Get().GetToolOverlayBorderless();
                if (borderless && !overlay->HasClass("borderless"))
                    overlay->AddClass("borderless");
                else if (!borderless && overlay->HasClass("borderless"))
                    overlay->RemoveClass("borderless");
            }
        }

        // When the FPS label is at its default placement (bottom-left),
        // auto-shift it horizontally to clear the rotation gizmo if they
        // overlap. Only runs for the default position so we never fight a
        // user drag.
        if (m_FpsLabel && m_FpsUsingDefaultPosition && m_FpsVisible && m_RotationGizmo)
        {
            UIElement* fpsViewport = m_FpsLabel->GetParent();
            const float fw = m_FpsLabel->GetLayoutWidth();
            const float fh = m_FpsLabel->GetLayoutHeight();
            const float gx = m_RotationGizmo->GetLayoutX();
            const float gy = m_RotationGizmo->GetLayoutY();
            const float gw = m_RotationGizmo->GetLayoutWidth();
            const float gh = m_RotationGizmo->GetLayoutHeight();
            const float vx = fpsViewport ? fpsViewport->GetLayoutX() : 0.0f;
            const float vy = fpsViewport ? fpsViewport->GetLayoutY() : 0.0f;
            const float vh = fpsViewport ? fpsViewport->GetLayoutHeight() : 0.0f;

            if (fw > 0.0f && fh > 0.0f && gw > 0.0f && gh > 0.0f && vh > 0.0f)
            {
                // Shift the default left-inset inward when the 2D ruler
                // occupies the left band, so the FPS label clears the ticks.
                const bool rulersOn =
                    m_Controller && m_Controller->Is2DMode() &&
                    Editor::SceneViewSettings::Get().GetShowRulers();
                const float defaultLeftInset = rulersOn ? 32.0f : 14.0f;

                // Test overlap at the *default* position to avoid oscillation
                // (push right → no overlap → reset → overlap).
                const float defaultFx = vx + defaultLeftInset;
                const float defaultFy = vy + vh - 14.0f - fh;
                const bool overlapAtDefault = !(defaultFx + fw <= gx || gx + gw <= defaultFx
                                              || defaultFy + fh <= gy || gy + gh <= defaultFy);
                auto& ov = m_FpsLabel->Overrides();
                if (overlapAtDefault)
                {
                    const float gizmoRightLocal = (gx + gw) - vx;
                    const float newLeft = gizmoRightLocal + 10.0f;
                    ov.Set(Style::PositionLeft, StyleLength::Px(newLeft));
                }
                else
                {
                    ov.Set(Style::PositionLeft, StyleLength::Px(defaultLeftInset));
                }
            }
        }
    }

    UpdateHoverHighlightPill();

    // Robustness: re-acquire on demand if the tree was reconciled.
    if (!m_FpsLabel || m_FpsLabel->GetOwnerManager() == nullptr)
    {
        m_FpsLabel = dynamic_cast<Label*>(FindById("SceneViewFpsLabel"));
    }
    if (!m_FpsLabel || !m_FpsVisible)
        return;
    if (deltaTime <= 0.0f)
        return;

    // Live per-frame refresh. The label is fixed-size, so SetText infers a
    // content-only change: the Update side idle-gates the frame while the
    // render-side drain re-emits the label's slots and range-uploads a few
    // hundred bytes — a per-frame text change costs ~idle. (The sliding-
    // window FPS below is already smoothed, so the value reads fine live.)

    // Read the spike-resistant sliding-window FPS from Application (a single
    // canonical source so this overlay agrees with the MonitorsPanel
    // Time/FPS metric and any get_render_stats consumer). Fall back to
    // 1/dt only if Application isn't constructed (unit tests, smoke harness).
    if (auto* app = Application::Get())
    {
        m_CurrentFPS = app->GetFps();
    }
    else if (m_CurrentFPS <= 0.0f && deltaTime > 1e-6f)
    {
        m_CurrentFPS = 1.0f / deltaTime;
    }

    char buf[32];
    // Display "60.0" when FPS is close to 60 (between 59.5 and 60.5).
    // No field-width padding — the fixed-size, centered container handles
    // layout stability without needing leading-space padding in the string.
    if (m_CurrentFPS >= 59.5f && m_CurrentFPS <= 60.5f)
    {
        std::snprintf(buf, sizeof(buf), "FPS: %.1f", 60.0f);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "FPS: %.1f", m_CurrentFPS);
    }
    m_FpsLabel->SetText(buf);
}

void SceneViewPanel::SetAssetPreview(const std::filesystem::path& path, bool enabled)
{
    m_AssetPreviewEnabled = enabled;
    m_AssetPreviewPath = path;
    UpdateAssetPreviewVisual();
}

void SceneViewPanel::EnsureAssetPreviewElements()
{
    if (!GetActiveViewportElement())
        return;

    if (!m_AssetPreviewOverlay || m_AssetPreviewOverlay->GetOwnerManager() == nullptr)
    {
        m_AssetPreviewOverlay = FindById("SceneAssetPreview");
    }

    if (!m_AssetPreviewImage || m_AssetPreviewImage->GetOwnerManager() == nullptr)
    {
        if (auto* img = FindById("SceneAssetPreviewImage"))
        {
            m_AssetPreviewImage = img;
        }
    }
}

void SceneViewPanel::UpdateAssetPreviewVisual()
{
    EnsureAssetPreviewElements();
    if (!m_AssetPreviewOverlay)
        return;

    const bool shouldShow = m_AssetPreviewEnabled && !m_AssetPreviewPath.empty();
    {
        m_AssetPreviewOverlay->Overrides().Set(Style::Display, shouldShow ? DisplayMode::Flex : DisplayMode::None);
    }

    if (!shouldShow)
    {
        if (m_AssetPreviewImage)
            UI::Layout::ClearBackgroundOverride(*m_AssetPreviewImage);
        return;
    }

    if (!m_Context || !m_Context->Thumbnails)
        return;

    int desiredSize = 1024;
    if (UIElement* viewport = GetActiveViewportElement())
    {
        const float w = viewport->GetLayoutWidth();
        const float h = viewport->GetLayoutHeight();
        const float maxDim = std::max(w, h);
        if (maxDim > 0.0f)
            desiredSize = static_cast<int>(std::max(256.0f, std::min(maxDim, 4096.0f)));
    }

    const std::filesystem::path path = m_AssetPreviewPath;
    auto immediate = m_Context->Thumbnails->GetOrRequest(
        path, desiredSize,
        [this, path, post = GetPostHandle()](const std::string& rel)
        {
            if (rel.empty())
                return;
            post.Post([this, path, rel]()
                             {
                if (!m_AssetPreviewEnabled || path != m_AssetPreviewPath)
                    return;
                ApplyAssetPreviewBackground(rel);
            });
        });

    if (!immediate.empty())
    {
        ApplyAssetPreviewBackground(immediate);
    }
}

void SceneViewPanel::ApplyAssetPreviewBackground(const std::string& relOrEngine)
{
    if (!m_AssetPreviewOverlay || !m_AssetPreviewImage)
        return;
    if (relOrEngine.empty())
    {
        // Hide any previous image background when the preview is cleared.
        // Reset only the visual overrides set by the texture-preview path;
        // do not Clear() everything — that would also wipe the Display
        // override owned by UpdateAssetPreviewVisual.
        UI::Layout::ClearBackgroundOverride(*m_AssetPreviewImage);
        m_AssetPreviewOverlay->Overrides()
            .Reset(Style::BackgroundColor)
            .Reset(Style::BorderRadius);
        return;
    }

    constexpr const char* kEnginePrefix = "engine:";
    constexpr size_t kEnginePrefixLen = 7;
    if (relOrEngine.rfind(kEnginePrefix, 0) == 0)
    {
        // Engine-backed previews (e.g., 3D model thumbnails rendered by the engine)
        // should keep the scene background, not a solid fill. Reset just the
        // texture-path visual overrides so the preview shows the scene through
        // the overlay while leaving Display intact.
        m_AssetPreviewOverlay->Overrides()
            .Reset(Style::BackgroundColor)
            .Reset(Style::BorderRadius);
        UI::Layout::SetBackgroundResourceName(*m_AssetPreviewImage, relOrEngine.substr(kEnginePrefixLen));
        return;
    }

    // Texture/image previews: use a neutral background behind the image so transparent
    // regions don't show the scene content. Keep this scoped to the Scene View's big
    // preview overlay so AssetViewPanel behavior remains unchanged.
    // Force square corners for the overlay and image so the large preview background
    // is not clipped by any rounded-corner theme styles.
    m_AssetPreviewOverlay->Overrides()
        .Set(Style::BackgroundColor, 0xFF202020u)
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{0.0f, 0.0f, 0.0f, 0.0f});
    m_AssetPreviewImage->Overrides()
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{0.0f, 0.0f, 0.0f, 0.0f});
    UI::Layout::SetBackgroundPath(*m_AssetPreviewImage, relOrEngine);
}

bool SceneViewPanel::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return typeId == UI::Interaction::GetPayloadTypeId<Editor::AssetPathsDragPayload>() ||
           typeId == UI::Interaction::GetPayloadTypeId<Editor::BookmarkDragPayload>() ||
           typeId == UI::Interaction::GetPayloadTypeId<Editor::OnlineAssetDragPayload>();
}

bool SceneViewPanel::HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const
{
    UIElement* viewport = GetActiveViewportElement();
    if (!viewport || !viewport->ContainsPoint(x, y))
        return false;
    out.TargetId = 0;
    out.Location = UI::Interaction::DropLocation::OnEmptySpace;
    out.IndentDepth = 0;
    return true;
}

UI::Interaction::DropFeedback SceneViewPanel::CanDrop(const UI::Interaction::DropRequest& request) const
{
    if (!m_Context || !m_Context->Assets)
        return {false, "No editor context"};
    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    if (!world)
        return {false, "No world"};
    if (!m_Controller || !GetActiveViewportElement())
        return {false, "Scene view not ready"};

    // Bookmark drag: resolve to asset path and check if it's a model.
    if (const auto* bpl = request.payload.TryGet<Editor::BookmarkDragPayload>())
    {
        if (bpl->bookmark.Type != BookmarkType::Asset || bpl->bookmark.Reference.empty())
            return {false, "Not an asset bookmark"};
        // Online asset bookmark — accept like OnlineAssetDragPayload.
        if (bpl->bookmark.Reference.rfind("polyhaven:", 0) == 0)
        {
            if (!ResolveRenderServices())
                return {false, "No render services"};
            return {true, {}};
        }
        GUID guid(bpl->bookmark.Reference);
        if (guid.IsNull())
            return {false, "Invalid GUID"};
        AssetMetadata metadata;
        if (!EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(guid, metadata))
            return {false, "Asset not found"};
        if (metadata.Path.empty())
            return {false, "No asset path"};
        std::filesystem::path absPath = metadata.Path;
        if (!absPath.is_absolute() && !m_Context->AssetsRoot.empty())
            absPath = m_Context->AssetsRoot / absPath;
        const bool isModel = IsModelFileExtension(absPath.extension().string());
        if (isModel && !ResolveRenderServices())
            return {false, "No render services"};
        return {true, {}, isModel};
    }

    // Online asset (Polyhaven): always accept models (create placeholder if not downloaded).
    if (const auto* onlinePl = request.payload.TryGet<Editor::OnlineAssetDragPayload>())
    {
        if (onlinePl->slug.empty())
            return {false, "No slug"};
        if (!ResolveRenderServices())
            return {false, "No render services"};
        return {true, {}};
    }

    const auto* pl = request.payload.TryGet<Editor::AssetPathsDragPayload>();
    if (!pl || pl->paths.empty())
        return {false, "Empty payload"};
    bool needsModelPipeline = false;
    for (const auto& p : pl->paths)
    {
        if (p.empty())
            continue;
        if (IsModelFileExtension(p.extension().string()))
        {
            needsModelPipeline = true;
            break;
        }
    }
    if (needsModelPipeline && !ResolveRenderServices())
        return {false, "No render services"};
    // Textures show the ghost thumbnail + target-mesh highlight; models show
    // an in-world 3D preview so the ghost would be redundant.
    const bool suppressGhost = needsModelPipeline;
    return {true, {}, suppressGhost};
}

void SceneViewPanel::PerformDrop(const UI::Interaction::DropRequest& request)
{
    UIElement* viewport = GetActiveViewportElement();
    if (!m_Context || !m_Context->Assets || !m_Controller || !viewport)
        return;

    // Online asset (Polyhaven): create placeholder or load if already downloaded.
    if (const auto* onlinePayload = request.payload.TryGet<Editor::OnlineAssetDragPayload>())
    {
        ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
        Engine::Renderer::RenderServices* rs = ResolveRenderServices();
        if (!world || !rs || onlinePayload->slug.empty())
            return;

        ClearDropPreviewModel();

        if (onlinePayload->type == "hdris")
        {
            std::vector<UI::Interaction::ItemId> selectionBeforeIds;
            UI::Interaction::ItemId selectionBeforeAnchor = 0;
            if (m_CaptureHierarchySelectionForDrop)
            {
                const auto captured = m_CaptureHierarchySelectionForDrop();
                selectionBeforeIds = std::move(captured.first);
                selectionBeforeAnchor = captured.second;
            }

            const ECS::EntityHandle skyEntity =
                Editor::CreateSkyboxEntityFromPolyhavenHdri(*world, *m_Context, onlinePayload->slug,
                                                            onlinePayload->name, m_ChangeNotifications);
            if (skyEntity.IsValid() && m_OnSceneAssetDropComplete)
            {
                std::vector<ECS::EntityHandle> roots{skyEntity};
                m_OnSceneAssetDropComplete(world, roots, selectionBeforeIds, selectionBeforeAnchor);
            }
            return;
        }

        UIManager* ui = GetOwnerManager();
        if (!ui) return;
        UI::Interaction::DragDropManager* ddm = ui->GetDragDropManager();
        if (!ddm) return;

        const float mx = ddm->GetLastHoverMouseX();
        const float my = ddm->GetLastHoverMouseY();
        const float localX = mx - viewport->GetLayoutX();
        const float localY = my - viewport->GetLayoutY();
        const float viewW = viewport->GetLayoutWidth();
        const float viewH = viewport->GetLayoutHeight();

        const Editor::SceneViewDropPoint dropPoint = Editor::ResolveSceneViewDropPoint(
            m_Controller->MakeGizmoRay(localX, localY, viewW, viewH), *world,
            GatherDropPreviewEntities(m_DropPreviewModel, m_DropPreviewBillboard));
        Mathematics::Vector3 worldPos = dropPoint.Position;

        std::vector<UI::Interaction::ItemId> selectionBeforeIds;
        UI::Interaction::ItemId selectionBeforeAnchor = 0;
        if (m_CaptureHierarchySelectionForDrop)
        {
            const auto captured = m_CaptureHierarchySelectionForDrop();
            selectionBeforeIds = std::move(captured.first);
            selectionBeforeAnchor = captured.second;
        }

        // Build entries list.
        std::vector<Editor::OnlineAssetEntry> items;
        if (!onlinePayload->entries.empty())
            items = onlinePayload->entries;
        else
            items.push_back({onlinePayload->slug, onlinePayload->name});

        auto& am = *m_Context->Assets;
        // New entities only. The drop's undo command deletes these roots and their subtrees,
        // so an existing mesh that received textures must never be listed here.
        std::vector<ECS::EntityHandle> droppedRoots;
        bool texturedExistingMesh = false;

        // Texture drop: check if dropping on mesh and assign to material slot.
        const bool isTextureDrop = (onlinePayload->type == "textures");
        const auto _previewIgnore = GatherDropPreviewEntities(m_DropPreviewModel, m_DropPreviewBillboard);
        const Editor::SceneViewDropMesh dropMesh = isTextureDrop
            ? Editor::PickSceneViewDropMesh(m_Controller->MakeGizmoRay(localX, localY, viewW, viewH), *world,
                                            _previewIgnore)
            : Editor::SceneViewDropMesh{};
        if (isTextureDrop ? dropMesh.Pending : dropPoint.OnPendingMesh)
        {
            Editor::ShowSceneViewNotice(Editor::kDropOnPendingMeshNotice);
            return;
        }
        const ECS::EntityHandle hitEntity = dropMesh.Entity;
        const bool onMesh = hitEntity.IsValid();

        for (const auto& item : items)
        {
            if (item.slug.empty())
            {
                Logger::Log::Warning("SceneViewPanel::PerformDrop: skipping entry with empty slug");
                continue;
            }

            // Check project assets first, then temp cache for completed early downloads.
            std::filesystem::path mainFilePath = PolyhavenService::FindDownloadedFile(item.slug, m_Context->AssetsRoot);
            if (mainFilePath.empty())
            {
                std::filesystem::path cachedFile = PolyhavenService::FindCachedDownloadFile(item.slug);
                if (!cachedFile.empty())
                {
                    std::filesystem::path projectDir = m_Context->AssetsRoot / "Polyhaven" / item.slug;
                    mainFilePath = PolyhavenService::MoveDownloadToProject(cachedFile, cachedFile.parent_path(), projectDir);
                    if (mainFilePath.empty())
                        Logger::Log::Warning("SceneViewPanel: MoveDownloadToProject failed for '{}'", item.slug);
                }
            }
            if (!mainFilePath.empty())
            {
                GUID assetGuid = am.ResolveAssetGuid(mainFilePath);
                if (!assetGuid.IsNull())
                {
                    using Clock = std::chrono::high_resolution_clock;
                    auto msElapsed = [](Clock::time_point t) {
                        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
                    };
                    const auto tDrop = Clock::now();

                    const auto tLoad = Clock::now();
                    SharedPtr<Asset> asset = am.GetAsset(assetGuid);
                    if (!asset)
                    {
                        // Not loaded yet: the model appears, and the drop is recorded, once it
                        // lands; the drop never waits for the import.
                        ECS::World* worldPtr = world;
                        Editor::RunWhenAssetLoaded(am, assetGuid, AssetLoadPriority::High, this, worldPtr,
                            [this, rs, worldPtr, assetGuid, mainFilePath, name = item.name, worldPos,
                             selectionBeforeIds, selectionBeforeAnchor]()
                            {
                                SpawnDeferredModelDrop(worldPtr, rs, assetGuid, mainFilePath, name, worldPos,
                                                       selectionBeforeIds, selectionBeforeAnchor);
                            },
                            mainFilePath.filename().string());
                        continue;
                    }
                    const double loadMs = msElapsed(tLoad);

                    auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
                    if (modelAsset && modelAsset->IsLoaded())
                    {
                        // Only create the model entity if all textures are loaded.
                        bool texturesReady = true;
                        for (const auto& img : modelAsset->GetEmbeddedImages())
                        {
                            if (!img.HasContent())
                            {
                                texturesReady = false;
                                break;
                            }
                        }
                        if (texturesReady)
                        {
                            const auto tCreate = Clock::now();
                            auto result = Engine::Renderer::ModelEntityFactory::CreateFromModel(
                                *rs, *world, *modelAsset, assetGuid, item.name, Editor::GetFbxModelEntityFactoryOptions(mainFilePath));
                            const double createMs = msElapsed(tCreate);

                            if (result.IsValid())
                            {
                                const auto tExport = Clock::now();
                                Editor::ExportModelMaterials(mainFilePath, assetGuid, result.submeshEntities, *world, am);
                                const double exportMs = msElapsed(tExport);

                                Logger::Log::Info(
                                    "[ModelLoad] SceneDrop(Polyhaven) '{}': TOTAL {:.1f}ms | assetLoad {:.1f}ms | create {:.1f}ms | exportMats {:.1f}ms",
                                    item.slug, msElapsed(tDrop), loadMs, createMs, exportMs);
                                ApplyRootWorldPosition(*world, result.rootEntity, worldPos);
                                Editor::AppendUnorderedRootsToHierarchyEnd(
                                    *world, std::span(&result.rootEntity, 1));
                                droppedRoots.push_back(result.rootEntity);
                                continue;
                            }
                            Logger::Log::Warning("SceneViewPanel: CreateFromModel failed for '{}'", item.slug);
                        }
                        else
                        {
                            Logger::Log::Info("SceneViewPanel: textures not ready for '{}', creating placeholder", item.slug);
                        }
                    }
                    else
                    {
                        Logger::Log::Warning("SceneViewPanel: model load failed for '{}' (asset={}, cast={}, loaded={})",
                            item.slug, asset ? "yes" : "null",
                            modelAsset ? "yes" : "fail",
                            modelAsset ? (modelAsset->IsLoaded() ? "yes" : "no") : "n/a");
                    }
                }
                else
                {
                    Logger::Log::Warning("SceneViewPanel: ResolveAssetGuid returned null for '{}' (path={})",
                        item.slug, mainFilePath.string());
                }
                // Model loading failed — fall through to placeholder creation.
            }

            // Texture drop on mesh: assign to material slot if already downloaded,
            // or start download and assign on completion if not yet downloaded.
            if (isTextureDrop && onMesh && hitEntity.IsValid())
            {
                if (!mainFilePath.empty())
                {
                    // Already downloaded — assign all PBR texture maps from the directory.
                    std::filesystem::path texDir = mainFilePath.parent_path();
                    int assignedCount = 0;

                    struct MapSlotMapping { const char* suffix; const char* slot; };
                    static const MapSlotMapping kMappings[] = {
                        {"_diffuse_", "albedoMap"},
                        {"_nor_gl_", "normalMap"},
                        {"_rough_", "roughnessMap"},      // Separate roughness (shader combines)
                        {"_metallic_", "metallicMap"},      // Separate metallic (shader combines)
                        {"_ao_", "aoMap"},
                        {"_arm_", "aoMap"}, // AO/Roughness/Metallic combined - use as AO
                    };

                    std::error_code ec;
                    for (const auto& entry : std::filesystem::directory_iterator(texDir, ec))
                    {
                        if (!entry.is_regular_file(ec)) continue;
                        std::string fname = entry.path().filename().string();
                        for (const auto& mapping : kMappings)
                        {
                            if (fname.find(mapping.suffix) != std::string::npos)
                            {
                                GUID texGuid = am.ResolveAssetGuid(entry.path());
                                if (!texGuid.IsNull())
                                {
                                    auto cmd = std::make_unique<Editor::TextureDropOnMeshCommand>(
                                        "Assign PBR Texture", world, hitEntity, texGuid, mapping.slot,
                                        std::string(),
                                        [ctx = m_Context]() { if (ctx && ctx->OnSceneDirty) ctx->OnSceneDirty(); },
                                        m_ChangeNotifications);
                                    if (auto* undo = m_Context->UndoRedo)
                                        undo->Execute(std::move(cmd));
                                    else
                                        cmd->Do();
                                    assignedCount++;
                                }
                                break;
                            }
                        }
                    }

                    if (assignedCount > 0)
                    {
                        Logger::Log::Info("SceneViewPanel: assigned {} PBR texture maps for '{}' to mesh entity {}",
                            assignedCount, item.slug, static_cast<uint32_t>(hitEntity.index));

                        if (m_Context && m_Context->OnSceneDirty)
                            m_Context->OnSceneDirty();

                        texturedExistingMesh = true;
                        continue;
                    }
                }
                else
                {
                    // Not downloaded yet — start download and assign on completion.
                    Logger::Log::Info("SceneViewPanel: Polyhaven texture '{}' not downloaded, starting download for mesh assignment",
                        item.slug);

                    if (m_Context->DownloadManager)
                    {
                        // Download all PBR texture maps (normal, roughness, metallic, AO, etc.)
                        std::filesystem::path destDir = m_Context->AssetsRoot / "Polyhaven" / item.slug;
                        m_Context->DownloadManager->DownloadAllTextureMaps(
                            item.slug, destDir, hitEntity, m_Context->UndoRedo, m_ChangeNotifications);
                    }

                    // The texture is assigned to the existing mesh when the download completes.
                    texturedExistingMesh = true;
                    continue;
                }
            }

            // Not downloaded or model load failed — create a placeholder billboard entity.
            auto camPose = m_Controller->GetCameraPose();
            Mathematics::Vector3 camPos{camPose.Pos[0], camPose.Pos[1], camPose.Pos[2]};
            std::filesystem::path thumbPath = PolyhavenService::GetCacheDir() / (item.slug + ".png");
            auto placeholderResult = PolyhavenPlaceholderFactory::Create(
                *world, *rs, m_Context->Assets, item.slug, onlinePayload->type,
                worldPos, camPos, thumbPath);

            if (placeholderResult.entity.IsValid())
            {
                Logger::Log::Info("SceneViewPanel: created placeholder for '{}' at ({:.1f}, {:.1f}, {:.1f})",
                    item.slug, worldPos.x, worldPos.y, worldPos.z);
                droppedRoots.push_back(placeholderResult.entity);

                if (m_Context->DownloadManager)
                {
                    // If an early download is already running, attach the placeholder to it.
                    // Otherwise start a fresh download to project assets.
                    if (m_Context->DownloadManager->IsDownloadingOrCompleted(item.slug))
                        m_Context->DownloadManager->AssignPlaceholder(item.slug, placeholderResult.entity, world);
                    else
                        m_Context->DownloadManager->StartDownloadForPlaceholder(
                            item.slug, onlinePayload->type, m_Context->AssetsRoot,
                            placeholderResult.entity, world);

                    // Hide the placeholder billboard; the download manager's pill overlay
                    // shows this download until the model lands, under the name recorded here.
                    if (world->HasComponent<Components::MeshRenderer>(placeholderResult.entity))
                        ECS::Entity(world, placeholderResult.entity).SetEnabled<Components::MeshRenderer>(false);
                    if (DownloadPillOverlay* pills = GetDownloadPillOverlay())
                        pills->SetDisplayName(item.slug, item.name.empty() ? item.slug : item.name);
                }
            }
            else
            {
                Logger::Log::Warning("SceneViewPanel: placeholder creation failed for '{}'", item.slug);
            }
        }

        NotifyWorldStructure(m_ChangeNotifications, world);
        if (m_Context && m_Context->OnSceneDirty)
            m_Context->OnSceneDirty();

        // Invalidate download cache so the asset browser shows blue text for newly downloaded items.
        if (m_Context->DownloadManager)
            if (auto cb = m_Context->DownloadManager->GetOnDownloadFinished())
                cb();

        if (droppedRoots.empty() && !texturedExistingMesh)
            Logger::Log::Warning("SceneViewPanel: PerformDrop produced no entities for '{}' ({} items)",
                onlinePayload->slug, items.size());

        if (!droppedRoots.empty() && m_OnSceneAssetDropComplete)
        {
            m_OnSceneAssetDropComplete(world, droppedRoots,
                                       selectionBeforeIds, selectionBeforeAnchor);
        }
        return;
    }

    // Bookmark drag: resolve to asset path and treat as an asset drop.
    // Online asset bookmarks (polyhaven:slug) are re-dispatched as OnlineAssetDragPayload.
    Editor::AssetPathsDragPayload bookmarkAsPaths;
    const auto* payload = request.payload.TryGet<Editor::AssetPathsDragPayload>();
    if (!payload)
    {
        if (const auto* bpl = request.payload.TryGet<Editor::BookmarkDragPayload>())
        {
            if (bpl->bookmark.Type == BookmarkType::Asset && !bpl->bookmark.Reference.empty())
            {
                if (bpl->bookmark.Reference.rfind("polyhaven:", 0) == 0)
                {
                    const std::string slug = bpl->bookmark.Reference.substr(10);
                    Editor::OnlineAssetDragPayload syntheticOnline;
                    syntheticOnline.slug = slug;
                    syntheticOnline.name = bpl->bookmark.Name;
                    syntheticOnline.type = "models";
                    syntheticOnline.entries.push_back({slug, bpl->bookmark.Name});

                    UI::Interaction::DropRequest syntheticReq = request;
                    syntheticReq.payload = UI::Interaction::DragPayload::Create(std::move(syntheticOnline));
                    PerformDrop(syntheticReq);
                    return;
                }

                GUID guid(bpl->bookmark.Reference);
                if (!guid.IsNull())
                {
                    AssetMetadata metadata;
                    if (EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(guid, metadata) &&
                        !metadata.Path.empty())
                    {
                        std::filesystem::path absPath = metadata.Path;
                        if (!absPath.is_absolute() && !m_Context->AssetsRoot.empty())
                            absPath = m_Context->AssetsRoot / absPath;
                        bookmarkAsPaths.paths.push_back(std::move(absPath));
                        payload = &bookmarkAsPaths;
                    }
                }
            }
        }
    }
    if (!payload)
        return;

    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    if (!world)
        return;

    ClearDropPreviewModel();

    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;
    UI::Interaction::DragDropManager* ddm = ui->GetDragDropManager();
    if (!ddm)
        return;

    const float mx = ddm->GetLastHoverMouseX();
    const float my = ddm->GetLastHoverMouseY();
    const float localX = mx - viewport->GetLayoutX();
    const float localY = my - viewport->GetLayoutY();
    const float viewW = viewport->GetLayoutWidth();
    const float viewH = viewport->GetLayoutHeight();

    const Editor::SceneViewDropPoint dropPoint = Editor::ResolveSceneViewDropPoint(
        m_Controller->MakeGizmoRay(localX, localY, viewW, viewH), *world,
        GatherDropPreviewEntities(m_DropPreviewModel, m_DropPreviewBillboard));
    Mathematics::Vector3 worldPos = dropPoint.Position;

    auto& am = *m_Context->Assets;

    // Single texture drop: assign to the mesh under the cursor (fast path) or
    // spawn a textured plane "sprite" at the drop position with the texture's
    // aspect ratio when the drop lands on empty space.
    if (payload->paths.size() == 1 && !payload->paths[0].empty())
    {
        const std::filesystem::path& texPath = payload->paths[0];
        if (Editor::IsHdriPath(texPath))
        {
            std::vector<UI::Interaction::ItemId> selectionBeforeIds;
            UI::Interaction::ItemId selectionBeforeAnchor = 0;
            if (m_CaptureHierarchySelectionForDrop)
            {
                const auto captured = m_CaptureHierarchySelectionForDrop();
                selectionBeforeIds = std::move(captured.first);
                selectionBeforeAnchor = captured.second;
            }

            ECS::EntityHandle skyEntity = Editor::CreateSkyboxEntityFromHdri(
                *world, am, texPath, m_ChangeNotifications,
                [ctx = m_Context]() { if (ctx && ctx->OnSceneDirty) ctx->OnSceneDirty(); });

            if (skyEntity.IsValid() && m_OnSceneAssetDropComplete)
            {
                std::vector<ECS::EntityHandle> roots{skyEntity};
                m_OnSceneAssetDropComplete(world, roots, selectionBeforeIds, selectionBeforeAnchor);
            }
            return;
        }

        const AssetType assetType = GetAssetTypeFromExtension(texPath.extension().string());
        if (assetType == AssetType::Material)
        {
            const Editor::SceneViewDropMesh dropMesh = Editor::PickSceneViewDropMesh(
                m_Controller->MakeGizmoRay(localX, localY, viewW, viewH), *world,
                GatherDropPreviewEntities(m_DropPreviewModel, m_DropPreviewBillboard));
            if (dropMesh.Pending)
            {
                Editor::ShowSceneViewNotice(Editor::kDropOnPendingMeshNotice);
                return;
            }
            const ECS::EntityHandle hitEntity = dropMesh.Entity;
            const bool onMesh = hitEntity.IsValid();
            if (onMesh)
            {
                const GUID matGuid = am.ResolveAssetGuid(texPath);
                if (!matGuid.IsNull())
                {
                    if (!am.GetAsset(matGuid))
                        am.LoadAsset(matGuid, AssetLoadResultCallback{});

                    auto cmd = std::make_unique<Editor::MaterialDropOnMeshCommand>(
                        "Assign Material", world, hitEntity, matGuid,
                        [ctx = m_Context]() { if (ctx && ctx->OnSceneDirty) ctx->OnSceneDirty(); },
                        m_ChangeNotifications);
                    if (auto* undo = (m_Context ? m_Context->UndoRedo : nullptr))
                        undo->Execute(std::move(cmd));
                    else
                        cmd->Do();
                }
            }
            return;
        }

        if (assetType == AssetType::Texture)
        {
            const Editor::SceneViewDropMesh dropMesh = Editor::PickSceneViewDropMesh(
                m_Controller->MakeGizmoRay(localX, localY, viewW, viewH), *world,
                GatherDropPreviewEntities(m_DropPreviewModel, m_DropPreviewBillboard));
            if (dropMesh.Pending)
            {
                Editor::ShowSceneViewNotice(Editor::kDropOnPendingMeshNotice);
                return;
            }
            const ECS::EntityHandle hitEntity = dropMesh.Entity;
            const bool onMesh = hitEntity.IsValid();

            if (onMesh)
            {
                const GUID texGuid = am.ResolveAssetGuid(texPath);
                if (!texGuid.IsNull())
                {
                    auto cmd = std::make_unique<Editor::TextureDropOnMeshCommand>(
                        "Assign Texture", world, hitEntity, texGuid, "albedoMap", std::string(),
                        [ctx = m_Context]() { if (ctx && ctx->OnSceneDirty) ctx->OnSceneDirty(); },
                        m_ChangeNotifications);
                    if (auto* undo = (m_Context ? m_Context->UndoRedo : nullptr))
                        undo->Execute(std::move(cmd));
                    else
                        cmd->Do();
                    return;
                }
            }
            else if (Engine::Renderer::RenderServices* rs = ResolveRenderServices())
            {
                const bool is2D = m_Controller && m_Controller->Is2DMode();
                const GUID texGuid = am.ResolveAssetGuid(texPath);
                if (!texGuid.IsNull() && !am.IsAssetLoaded(texGuid))
                {
                    // The sprite is sized from the texture: a texture not loaded yet becomes its
                    // sprite, and the drop is recorded, once the load lands; never waited for here.
                    std::vector<UI::Interaction::ItemId> selectionBeforeIds;
                    UI::Interaction::ItemId selectionBeforeAnchor = 0;
                    if (m_CaptureHierarchySelectionForDrop)
                    {
                        const auto captured = m_CaptureHierarchySelectionForDrop();
                        selectionBeforeIds = std::move(captured.first);
                        selectionBeforeAnchor = captured.second;
                    }
                    ECS::World* worldPtr = world;
                    Editor::RunWhenAssetLoaded(am, texGuid, AssetLoadPriority::High, this, worldPtr,
                        [this, rs, worldPtr, texPath, worldPos, is2D, selectionBeforeIds, selectionBeforeAnchor]()
                        {
                            const ECS::EntityHandle sprite = Editor::CreateSpriteEntityFromTexture(
                                *worldPtr, *rs, EngineCore::GetInstance().GetAssetManager(), texPath, worldPos, is2D);
                            if (sprite.IsValid())
                                CompleteDeferredDrop(*worldPtr, sprite, selectionBeforeIds, selectionBeforeAnchor);
                        },
                        texPath.filename().string());
                    return;
                }
                ECS::EntityHandle sprite = Editor::CreateSpriteEntityFromTexture(
                    *world, *rs, am, texPath, worldPos, is2D);
                if (sprite.IsValid())
                {
                    std::vector<UI::Interaction::ItemId> selectionBeforeIds;
                    UI::Interaction::ItemId selectionBeforeAnchor = 0;
                    if (m_CaptureHierarchySelectionForDrop)
                    {
                        const auto captured = m_CaptureHierarchySelectionForDrop();
                        selectionBeforeIds = std::move(captured.first);
                        selectionBeforeAnchor = captured.second;
                    }

                    NotifyWorldStructure(m_ChangeNotifications, world);
                    if (m_Context && m_Context->OnSceneDirty)
                        m_Context->OnSceneDirty();

                    if (m_OnSceneAssetDropComplete && viewport)
                    {
                        std::vector<ECS::EntityHandle> roots{sprite};
                        m_OnSceneAssetDropComplete(world, roots,
                                                   selectionBeforeIds, selectionBeforeAnchor);
                    }
                    return;
                }
            }
        }
    }

    // Models, prefabs and lens flares are placed at the drop point: not on a stand-in for a mesh.
    if (dropPoint.OnPendingMesh)
    {
        Editor::ShowSceneViewNotice(Editor::kDropOnPendingMeshNotice);
        return;
    }

    std::vector<UI::Interaction::ItemId> selectionBeforeIds;
    UI::Interaction::ItemId selectionBeforeAnchor = 0;
    if (m_CaptureHierarchySelectionForDrop)
    {
        const auto captured = m_CaptureHierarchySelectionForDrop();
        selectionBeforeIds = std::move(captured.first);
        selectionBeforeAnchor = captured.second;
    }

    std::vector<ECS::EntityHandle> droppedRoots;
    droppedRoots.reserve(payload->paths.size());

    for (const auto& absPath : payload->paths)
    {
        if (absPath.empty())
            continue;

        const std::string ext = absPath.extension().string();
        const AssetType assetType = GetAssetTypeFromExtension(ext);
        if (assetType == AssetType::LensFlareDefinition)
        {
            const GUID flareGuid = am.ResolveAssetGuid(absPath);
            if (flareGuid.IsNull())
                continue;

            ECS::EntityHandle flareEntity = world->CreateEntity();
            if (!flareEntity.IsValid())
                continue;

            Components::Name name{};
            const std::string stem = absPath.stem().string();
            std::strncpy(name.value, stem.c_str(), sizeof(name.value) - 1);
            world->AddComponentImmediate(flareEntity, name);

            Components::Transform transform{};
            transform.SetIdentity();
            transform.matrix[12] = worldPos.x;
            transform.matrix[13] = worldPos.y;
            transform.matrix[14] = worldPos.z;
            world->AddComponentImmediate(flareEntity, transform);

            Components::LensFlareSource source{};
            source.Flare.Set(flareGuid);
            world->AddComponentImmediate(flareEntity, source);
            Editor::AppendUnorderedRootsToHierarchyEnd(*world, std::span(&flareEntity, 1));
            droppedRoots.push_back(flareEntity);
            continue;
        }

        const bool isModel = IsModelFileExtension(ext);

        if (isModel)
        {
            Engine::Renderer::RenderServices* rs = ResolveRenderServices();
            if (!rs)
                continue;

            const GUID assetGuid = am.ResolveAssetGuid(absPath);
            if (assetGuid.IsNull())
                continue;

            // A loaded model joins this drop. One not loaded yet is spawned, and
            // recorded as its own drop (selection, one undo step), when its load
            // lands, so the drop never blocks the editor on a first import. The
            // panel is the post target: the continuation is dropped with it.
            if (am.IsAssetLoaded(assetGuid))
            {
                const ECS::EntityHandle root =
                    SpawnDroppedModel(*world, *rs, assetGuid, absPath, absPath.stem().string(), worldPos);
                if (root.IsValid())
                    droppedRoots.push_back(root);
                continue;
            }
            ECS::World* worldPtr = world;
            Editor::RunWhenAssetLoaded(am, assetGuid, AssetLoadPriority::High, this, worldPtr,
                [this, rs, worldPtr, assetGuid, absPath, worldPos, selectionBeforeIds, selectionBeforeAnchor]()
                {
                    SpawnDeferredModelDrop(worldPtr, rs, assetGuid, absPath, absPath.stem().string(), worldPos,
                                           selectionBeforeIds, selectionBeforeAnchor);
                },
                absPath.filename().string());
            continue;
        }

        ECS::EntityHandle e = world->CreateEntity();
        if (!e.IsValid())
            continue;

        Components::Name nm{};
        std::memset(nm.value, 0, sizeof(nm.value));
        const std::string stem = absPath.stem().string();
        std::strncpy(nm.value, stem.c_str(), sizeof(nm.value) - 1);
        world->AddComponentImmediate(e, nm);

        {
            Components::Transform dropXf{};
            dropXf.SetIdentity();
            dropXf.matrix[12] = worldPos.x;
            dropXf.matrix[13] = worldPos.y;
            dropXf.matrix[14] = worldPos.z;
            world->AddComponentImmediate(e, dropXf);
        }

        Components::SceneBlueprintInstance inst{};
        std::memset(inst.sourcePath, 0, sizeof(inst.sourcePath));
        std::memset(inst.sourceGuid, 0, sizeof(inst.sourceGuid));
        // Drop payloads carry registry paths, which are folded on
        // case-insensitive platforms while the root carries its own spelling;
        // a lexical subtraction compares case and would persist an absolute
        // path into the scene file.
        std::string relStr = Editor::TryMakeAssetRelativePathString(
            EngineCore::GetInstance().GetAssetManager(), absPath);
        if (relStr.empty())
            relStr = absPath.generic_string();
        std::strncpy(inst.sourcePath, relStr.c_str(), sizeof(inst.sourcePath) - 1);
        world->AddComponentImmediate(e, inst);
        Editor::AppendUnorderedRootsToHierarchyEnd(*world, std::span(&e, 1));
        droppedRoots.push_back(e);
    }

    NotifyWorldStructure(m_ChangeNotifications, world);
    if (m_Context && m_Context->OnSceneDirty)
        m_Context->OnSceneDirty();

    // Run immediately (not PostAction). Deferring pushed the Scene Drop undo after later edits
    // (e.g. hierarchy selection), so Undo would remove the drop before the selection change.
    if (!droppedRoots.empty() && m_OnSceneAssetDropComplete && viewport)
    {
        ECS::World* worldPtr = world;
        std::vector<ECS::EntityHandle> rootsCopy = std::move(droppedRoots);
        std::vector<UI::Interaction::ItemId> beforeIds = std::move(selectionBeforeIds);
        const UI::Interaction::ItemId beforeAnchor = selectionBeforeAnchor;
        m_OnSceneAssetDropComplete(worldPtr, rootsCopy, beforeIds, beforeAnchor);
    }
}

void SceneViewPanel::SpawnDeferredModelDrop(ECS::World* world, Engine::Renderer::RenderServices* renderServices,
                                            const GUID& modelGuid, const std::filesystem::path& modelPath,
                                            const std::string& name, const Mathematics::Vector3& worldPos,
                                            const std::vector<UI::Interaction::ItemId>& selectionBefore,
                                            UI::Interaction::ItemId anchorBefore)
{
    if (!world || !renderServices)
        return;
    const ECS::EntityHandle root = SpawnDroppedModel(*world, *renderServices, modelGuid, modelPath, name, worldPos);
    if (root.IsValid())
        CompleteDeferredDrop(*world, root, selectionBefore, anchorBefore);
}

ECS::EntityHandle SceneViewPanel::SpawnDroppedModel(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                                                    const GUID& modelGuid, const std::filesystem::path& modelPath,
                                                    const std::string& name, const Mathematics::Vector3& worldPos)
{
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    SharedPtr<Asset> asset = assets.GetAsset(modelGuid);
    auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
    if (!modelAsset || !modelAsset->IsLoaded())
    {
        Logger::Log::Warning("SceneViewPanel: dropped model '{}' did not load; nothing was added",
                             modelPath.string());
        return {};
    }
    auto result = Engine::Renderer::ModelEntityFactory::CreateFromModel(
        renderServices, world, *modelAsset, modelGuid, name, Editor::GetFbxModelEntityFactoryOptions(modelPath));
    if (!result.IsValid())
        return {};
    Editor::ExportModelMaterials(modelPath, modelGuid, result.submeshEntities, world, assets);
    ApplyRootWorldPosition(world, result.rootEntity, worldPos);
    Editor::AppendUnorderedRootsToHierarchyEnd(world, std::span(&result.rootEntity, 1));
    return result.rootEntity;
}

void SceneViewPanel::CompleteDeferredDrop(ECS::World& world, ECS::EntityHandle root,
                                          const std::vector<UI::Interaction::ItemId>& selectionBefore,
                                          UI::Interaction::ItemId anchorBefore)
{
    NotifyWorldStructure(m_ChangeNotifications, &world);
    if (m_Context && m_Context->OnSceneDirty)
        m_Context->OnSceneDirty();
    if (m_OnSceneAssetDropComplete)
    {
        const std::vector<ECS::EntityHandle> roots{root};
        m_OnSceneAssetDropComplete(&world, roots, selectionBefore, anchorBefore);
    }
}

bool SceneViewPanel::PollDragPreviewDownload()
{
    // Nothing to do if no billboard preview is active.
    if (!m_DropPreviewBillboard.IsValid() || m_DropPreviewOnlineSlug.empty())
        return false;
    if (!m_Context || !m_Context->Assets)
        return false;

    // Check project assets first, then temp cache.
    bool movedFromCache = false;
    std::filesystem::path mainFile = PolyhavenService::FindDownloadedFile(m_DropPreviewOnlineSlug, m_Context->AssetsRoot);
    if (mainFile.empty())
    {
        std::filesystem::path cachedFile = PolyhavenService::FindCachedDownloadFile(m_DropPreviewOnlineSlug);
        if (!cachedFile.empty())
        {
            std::filesystem::path projectDir = m_Context->AssetsRoot / "Polyhaven" / m_DropPreviewOnlineSlug;
            mainFile = PolyhavenService::MoveDownloadToProject(cachedFile, cachedFile.parent_path(), projectDir);
            movedFromCache = !mainFile.empty();
        }
    }
    if (mainFile.empty())
        return false;

    Engine::Renderer::RenderServices* rs = ResolveRenderServices();
    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    if (!rs || !world)
        return movedFromCache;

    auto& am = *m_Context->Assets;
    GUID assetGuid = am.ResolveAssetGuid(mainFile);
    if (assetGuid.IsNull())
        return movedFromCache;

    // Read position and slug from current billboard before attempting model load.
    Mathematics::Vector3 worldPos{};
    if (auto* xf = world->GetComponent<Components::Transform>(m_DropPreviewBillboard))
    {
        worldPos.x = xf->matrix[12];
        worldPos.y = xf->matrix[13];
        worldPos.z = xf->matrix[14];
    }
    const std::string savedSlug = m_DropPreviewOnlineSlug;

    // Load the model BEFORE destroying the billboard, so we don't leave a
    // gap if the model fails to load. A model not loaded yet is requested
    // and picked up by a later poll; the poll never waits for the import.
    SharedPtr<Asset> asset = am.GetAsset(assetGuid);
    if (!asset)
    {
        (void)am.LoadAssetAsync(assetGuid, AssetLoadPriority::High);
        return movedFromCache;
    }
    auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
    if (!modelAsset || !modelAsset->IsLoaded())
        return movedFromCache;

    // Don't show 3D preview until all textures are loaded — keep billboard.
    // Unload the asset so the next poll reloads from disk (textures may have
    // just been written by the download thread).
    for (const auto& img : modelAsset->GetEmbeddedImages())
    {
        if (!img.HasContent())
        {
            am.UnloadAssetAsync(assetGuid).wait();
            return movedFromCache;
        }
    }

    const std::string name = std::string("(Preview) ") + savedSlug;
    auto created = Engine::Renderer::ModelEntityFactory::CreateFromModel(
        *rs, *world, *modelAsset, assetGuid, name, Editor::GetFbxModelEntityFactoryOptions(mainFile));
    if (!created.IsValid())
        return movedFromCache;

    // Model created successfully — now destroy the old billboard.
    ClearDropPreviewModel();

    m_DropPreviewModel = std::move(created);
    m_DropPreviewAssetGuid = assetGuid;
    ApplyRootWorldPosition(*world, m_DropPreviewModel->rootEntity, worldPos);
    return movedFromCache;
}

void SceneViewPanel::ClearDropPreviewModel()
{
    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    if (m_DropPreviewModel.has_value() && world)
        DestroyModelHierarchy(*world, *m_DropPreviewModel);
    m_DropPreviewModel.reset();
    m_DropPreviewAssetGuid = {};

    if (m_DropPreviewBillboard.IsValid() && world)
        world->DestroyEntityImmediate(m_DropPreviewBillboard);
    m_DropPreviewBillboard = {};
    m_DropPreviewOnlineSlug.clear();
    // The drag pill lives exactly as long as the online-asset preview anchor.
    if (DownloadPillOverlay* pills = GetDownloadPillOverlay())
        pills->HideDragPill();

    if (m_DropTargetHighlight.IsValid() && m_Controller)
        m_Controller->SetHoverEntity({});
    m_DropTargetHighlight = {};
}

void SceneViewPanel::PublishOverlayLayer(UIElement& viewport)
{
    // The overlays project with the camera of the pane this viewport is, read when they ask:
    // a quad view pane can switch which view it shows.
    UIElement* layer = Editor::ViewOverlayHost::Get().PublishViewport(
        Editor::ViewOverlayView::Scene, viewport,
        [this, pane = MakeWeakRef(&viewport)]() -> SceneViewController* {
            const UIElement* element = pane.Get();
            return element ? GetSceneControllerForViewport(*element) : nullptr;
        });
    m_PublishedOverlayViewport = MakeWeakRef(&viewport);
    // The download pills stack under the overlay layer's column.
    if (DownloadPillOverlay* pills = GetDownloadPillOverlay())
        pills->SetHost(&viewport, layer);
}

DownloadPillOverlay* SceneViewPanel::GetDownloadPillOverlay() const
{
    if (!m_Context || !m_Context->DownloadManager)
        return nullptr;
    return &m_Context->DownloadManager->PillOverlay();
}

void SceneViewPanel::SetDropPreview(const UI::Interaction::DropPreviewState& state)
{
    if (!state.Visible || !state.Allowed)
    {
        // Also retires the drag pill. A dropped download is shown by the download
        // manager's pill overlay until the model replaces the placeholder.
        ClearDropPreviewModel();
        return;
    }
    UIElement* viewport = GetActiveViewportElement();
    if (!m_Controller || !viewport || !m_Context || !m_Context->Assets)
    {
        ClearDropPreviewModel();
        return;
    }

    UIManager* ui = GetOwnerManager();
    if (!ui)
    {
        ClearDropPreviewModel();
        return;
    }
    UI::Interaction::DragDropManager* ddm = ui->GetDragDropManager();
    if (!ddm || !ddm->IsDragging())
    {
        ClearDropPreviewModel();
        return;
    }

    const auto& payload = ddm->GetPayload();

    // Online asset (Polyhaven): show 3D model if downloaded, billboard if not.
    if (const auto* onlinePl = payload.TryGet<Editor::OnlineAssetDragPayload>())
    {
        if (onlinePl->slug.empty())
        {
            ClearDropPreviewModel();
            return;
        }

        if (onlinePl->type == "hdris")
        {
            ClearDropPreviewModel();
            if (m_Context->DownloadManager)
            {
                m_Context->DownloadManager->StartHDRIDownload(onlinePl->slug, "1k", m_Context->AssetsRoot);
                const std::string preferred = PolyhavenService::GetPreferredHDRIResolution();
                if (preferred != "1k")
                    m_Context->DownloadManager->StartHDRIDownload(onlinePl->slug, preferred, m_Context->AssetsRoot);
            }
            ddm->SetDisplayLabel((onlinePl->name.empty() ? onlinePl->slug : onlinePl->name) + " (HDRI skybox)");
            return;
        }

        Engine::Renderer::RenderServices* rs = ResolveRenderServices();
        ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
        if (!rs || !world)
        {
            ClearDropPreviewModel();
            return;
        }

        const float mx = ddm->GetLastHoverMouseX();
        const float my = ddm->GetLastHoverMouseY();
        const float localX = mx - viewport->GetLayoutX();
        const float localY = my - viewport->GetLayoutY();
        const float viewW = viewport->GetLayoutWidth();
        const float viewH = viewport->GetLayoutHeight();

        const Editor::SceneViewDropPoint dropPoint = Editor::ResolveSceneViewDropPoint(
            m_Controller->MakeGizmoRay(localX, localY, viewW, viewH), *world,
            GatherDropPreviewEntities(m_DropPreviewModel, m_DropPreviewBillboard));
        Mathematics::Vector3 worldPos = dropPoint.Position;

        // Polyhaven texture: highlight the mesh under cursor as drop target.
        // Polyhaven model: show outline on hover target.
        // A mesh still being prepared for picking highlights by its bounds; the drop refuses it.
        const ECS::EntityHandle hoverEntity =
            Editor::PickSceneViewDropMesh(m_Controller->MakeGizmoRay(localX, localY, viewW, viewH), *world,
                                          GatherDropPreviewEntities(m_DropPreviewModel, m_DropPreviewBillboard))
                .Entity;
        const bool hasHoverTarget = hoverEntity.IsValid();
        
        if (hasHoverTarget)
        {
            // Highlight/outline the target entity.
            if (hoverEntity != m_DropTargetHighlight)
            {
                m_DropTargetHighlight = hoverEntity;
                m_Controller->SetHoverEntity(hoverEntity);
            }
        }
        else
        {
            // Not hovering over a mesh — clear highlight.
            if (m_DropTargetHighlight.IsValid())
            {
                m_DropTargetHighlight = {};
                m_Controller->SetHoverEntity({});
            }
        }

        // Check project assets first, then temp cache for completed early downloads.
        std::filesystem::path mainFile = PolyhavenService::FindDownloadedFile(onlinePl->slug, m_Context->AssetsRoot);
        if (mainFile.empty())
        {
            // Early download may have completed to temp cache — move to project assets.
            std::filesystem::path cachedFile = PolyhavenService::FindCachedDownloadFile(onlinePl->slug);
            if (!cachedFile.empty())
            {
                std::filesystem::path projectDir = m_Context->AssetsRoot / "Polyhaven" / onlinePl->slug;
                mainFile = PolyhavenService::MoveDownloadToProject(cachedFile, cachedFile.parent_path(), projectDir);
            }
        }

        if (!mainFile.empty())
        {
            auto& am = *m_Context->Assets;
            GUID assetGuid = am.ResolveAssetGuid(mainFile);
            if (!assetGuid.IsNull())
            {
                if (!m_DropPreviewModel.has_value() || m_DropPreviewAssetGuid != assetGuid)
                {
                    ClearDropPreviewModel();

                    // A model not loaded yet is requested; the preview appears on a
                    // later hover update once it has loaded, without waiting here.
                    SharedPtr<Asset> asset = am.GetAsset(assetGuid);
                    if (!asset)
                    {
                        (void)am.LoadAssetAsync(assetGuid, AssetLoadPriority::High);
                        return;
                    }
                    auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
                    if (!modelAsset || !modelAsset->IsLoaded())
                        return;

                    // Don't show 3D preview until all textures are loaded.
                    for (const auto& img : modelAsset->GetEmbeddedImages())
                    {
                        if (!img.HasContent())
                            return; // textures not ready — keep billboard
                    }

                    const std::string name = std::string("(Preview) ") + onlinePl->slug;
                    auto created = Engine::Renderer::ModelEntityFactory::CreateFromModel(
                        *rs, *world, *modelAsset, assetGuid, name, Editor::GetFbxModelEntityFactoryOptions(mainFile));
                    if (!created.IsValid())
                        return;
                    m_DropPreviewModel = std::move(created);
                    m_DropPreviewAssetGuid = assetGuid;

                    // Update ghost label to show the asset is ready.
                    ddm->SetDisplayLabel(onlinePl->name.empty() ? onlinePl->slug : onlinePl->name);
                }

                if (m_DropPreviewModel.has_value() && m_DropPreviewModel->IsValid())
                    ApplyRootWorldPosition(*world, m_DropPreviewModel->rootEntity, worldPos);
            }
            else
            {
                ClearDropPreviewModel();
            }
            return;
        }

        // Update ghost label to show downloading state.
        // The screen-space download pill (below) is the sole download indicator;
        // clear the drag ghost label so the two do not duplicate each other.
        ddm->SetDisplayLabel("");

        // Start downloading early (to temp cache) so it's ready by the time the user drops.
        if (m_Context->DownloadManager)
            m_Context->DownloadManager->StartEarlyDownload(onlinePl->slug, onlinePl->type, m_Context->AssetsRoot);

        // Not downloaded: show billboard placeholder at drop position.
        if (!m_DropPreviewBillboard.IsValid() || m_DropPreviewOnlineSlug != onlinePl->slug)
        {
            ClearDropPreviewModel();

            auto camPose = m_Controller->GetCameraPose();
            Mathematics::Vector3 camPos{camPose.Pos[0], camPose.Pos[1], camPose.Pos[2]};
            std::filesystem::path thumbPath = PolyhavenService::GetCacheDir() / (onlinePl->slug + ".png");
            auto previewResult = PolyhavenPlaceholderFactory::Create(
                *world, *rs, m_Context->Assets, onlinePl->slug, "models",
                worldPos, camPos, thumbPath, {}, false);
            m_DropPreviewBillboard = previewResult.entity;
            m_DropPreviewOnlineSlug = onlinePl->slug;
            // Hide the world-space plane; the screen-space pill is the visual now.
            if (world->HasComponent<Components::MeshRenderer>(m_DropPreviewBillboard))
                ECS::Entity(world, m_DropPreviewBillboard).SetEnabled<Components::MeshRenderer>(false);
        }
        else
        {
            // Keep the invisible anchor tracking the drop position.
            ApplyRootWorldPosition(*world, m_DropPreviewBillboard, worldPos);
        }

        // Screen-space download progress pill at the cursor (replaces the plane).
        if (DownloadPillOverlay* pills = GetDownloadPillOverlay())
            pills->ShowDragPill(onlinePl->slug, onlinePl->name.empty() ? onlinePl->slug : onlinePl->name,
                                localX, localY);
        return;
    }

    const auto* pathsPayload = payload.TryGet<Editor::AssetPathsDragPayload>();

    // Resolve bookmark drag to an asset path for preview.
    // Online asset bookmarks show a billboard placeholder (same as OnlineAssetDragPayload hover).
    Editor::AssetPathsDragPayload bookmarkPaths;
    std::string bookmarkOnlineSlug;
    std::string bookmarkOnlineName;
    if (!pathsPayload)
    {
        if (const auto* bpl = payload.TryGet<Editor::BookmarkDragPayload>())
        {
            if (bpl->bookmark.Type == BookmarkType::Asset && !bpl->bookmark.Reference.empty())
            {
                if (bpl->bookmark.Reference.rfind("polyhaven:", 0) == 0)
                {
                    bookmarkOnlineSlug = bpl->bookmark.Reference.substr(10);
                    bookmarkOnlineName = bpl->bookmark.Name;
                }
                else
                {
                    GUID guid(bpl->bookmark.Reference);
                    if (!guid.IsNull())
                    {
                        AssetMetadata metadata;
                        if (EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(guid, metadata) &&
                            !metadata.Path.empty())
                        {
                            std::filesystem::path absPath = metadata.Path;
                            if (!absPath.is_absolute() && m_Context && !m_Context->AssetsRoot.empty())
                                absPath = m_Context->AssetsRoot / absPath;
                            bookmarkPaths.paths.push_back(std::move(absPath));
                            pathsPayload = &bookmarkPaths;
                        }
                    }
                }
            }
        }
    }

    // Online asset bookmark: show billboard placeholder at drop position.
    if (!bookmarkOnlineSlug.empty())
    {
        Engine::Renderer::RenderServices* rs = ResolveRenderServices();
        ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
        if (!rs || !world) { ClearDropPreviewModel(); return; }

        const float mx = ddm->GetLastHoverMouseX();
        const float my = ddm->GetLastHoverMouseY();
        const float localX = mx - viewport->GetLayoutX();
        const float localY = my - viewport->GetLayoutY();
        const float viewW = viewport->GetLayoutWidth();
        const float viewH = viewport->GetLayoutHeight();
        const Editor::SceneViewDropPoint dropPoint = Editor::ResolveSceneViewDropPoint(
            m_Controller->MakeGizmoRay(localX, localY, viewW, viewH), *world,
            GatherDropPreviewEntities(m_DropPreviewModel, m_DropPreviewBillboard));
        Mathematics::Vector3 worldPos = dropPoint.Position;

        if (m_Context->DownloadManager)
            m_Context->DownloadManager->StartEarlyDownload(bookmarkOnlineSlug, "models", m_Context->AssetsRoot);

        ddm->SetDisplayLabel("");

        if (!m_DropPreviewBillboard.IsValid() || m_DropPreviewOnlineSlug != bookmarkOnlineSlug)
        {
            ClearDropPreviewModel();
            auto camPose = m_Controller->GetCameraPose();
            Mathematics::Vector3 camPos{camPose.Pos[0], camPose.Pos[1], camPose.Pos[2]};
            std::filesystem::path thumbPath = PolyhavenService::GetCacheDir() / (bookmarkOnlineSlug + ".png");
            auto previewResult = PolyhavenPlaceholderFactory::Create(
                *world, *rs, m_Context->Assets, bookmarkOnlineSlug, "models",
                worldPos, camPos, thumbPath, {}, false);
            m_DropPreviewBillboard = previewResult.entity;
            m_DropPreviewOnlineSlug = bookmarkOnlineSlug;
            if (world->HasComponent<Components::MeshRenderer>(m_DropPreviewBillboard))
                ECS::Entity(world, m_DropPreviewBillboard).SetEnabled<Components::MeshRenderer>(false);
        }
        else
        {
            ApplyRootWorldPosition(*world, m_DropPreviewBillboard, worldPos);
        }

        if (DownloadPillOverlay* pills = GetDownloadPillOverlay())
            pills->ShowDragPill(bookmarkOnlineSlug,
                                bookmarkOnlineName.empty() ? bookmarkOnlineSlug : bookmarkOnlineName,
                                localX, localY);
        return;
    }
    if (!pathsPayload || pathsPayload->paths.empty())
    {
        ClearDropPreviewModel();
        return;
    }

    const std::filesystem::path& first = pathsPayload->paths[0];

    // Single-texture or single-material drag: highlight the mesh under the
    // cursor as the drop target.
    if (pathsPayload->paths.size() == 1 && !first.empty())
    {
        const AssetType dragType = GetAssetTypeFromExtension(first.extension().string());
        if (dragType == AssetType::Texture || dragType == AssetType::Material)
        {
            ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
            if (!world)
            {
                ClearDropPreviewModel();
                return;
            }

            const float mx = ddm->GetLastHoverMouseX();
            const float my = ddm->GetLastHoverMouseY();
            const float localX = mx - m_Viewport->GetLayoutX();
            const float localY = my - m_Viewport->GetLayoutY();
            const float viewW = m_Viewport->GetLayoutWidth();
            const float viewH = m_Viewport->GetLayoutHeight();

            const ECS::EntityHandle hitEntity =
                Editor::PickSceneViewDropMesh(m_Controller->MakeGizmoRay(localX, localY, viewW, viewH), *world,
                                              GatherDropPreviewEntities(m_DropPreviewModel, m_DropPreviewBillboard))
                    .Entity;

            if (hitEntity != m_DropTargetHighlight)
            {
                m_Controller->SetHoverEntity(hitEntity);
                m_DropTargetHighlight = hitEntity;
            }
            return;
        }
    }

    if (first.empty() || !IsModelFileExtension(first.extension().string()))
    {
        ClearDropPreviewModel();
        return;
    }

    Engine::Renderer::RenderServices* rs = ResolveRenderServices();
    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    if (!rs || !world)
    {
        ClearDropPreviewModel();
        return;
    }

    auto& am = *m_Context->Assets;
    const GUID assetGuid = am.ResolveAssetGuid(first);
    if (assetGuid.IsNull())
    {
        ClearDropPreviewModel();
        return;
    }

    const float mx = ddm->GetLastHoverMouseX();
    const float my = ddm->GetLastHoverMouseY();
    const float localX = mx - viewport->GetLayoutX();
    const float localY = my - viewport->GetLayoutY();
    const float viewW = viewport->GetLayoutWidth();
    const float viewH = viewport->GetLayoutHeight();

    const Editor::SceneViewDropPoint dropPoint = Editor::ResolveSceneViewDropPoint(
        m_Controller->MakeGizmoRay(localX, localY, viewW, viewH), *world,
        GatherDropPreviewEntities(m_DropPreviewModel, m_DropPreviewBillboard));
    Mathematics::Vector3 worldPos = dropPoint.Position;

    if (!m_DropPreviewModel.has_value() || m_DropPreviewAssetGuid != assetGuid)
    {
        ClearDropPreviewModel();

        // A model not loaded yet is requested; the preview appears on a later
        // hover update once it has loaded, without waiting here.
        SharedPtr<Asset> asset = am.GetAsset(assetGuid);
        if (!asset)
        {
            (void)am.LoadAssetAsync(assetGuid, AssetLoadPriority::High);
            return;
        }
        auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
        if (!modelAsset || !modelAsset->IsLoaded())
            return;

        const std::string name = std::string("(Preview) ") + first.stem().string();
        Engine::Renderer::ModelEntityResult created = Engine::Renderer::ModelEntityFactory::CreateFromModel(
            *rs, *world, *modelAsset, assetGuid, name, Editor::GetFbxModelEntityFactoryOptions(first));
        if (!created.IsValid())
            return;
        m_DropPreviewModel = std::move(created);
        m_DropPreviewAssetGuid = assetGuid;
    }

    if (m_DropPreviewModel.has_value() && m_DropPreviewModel->IsValid())
        ApplyRootWorldPosition(*world, m_DropPreviewModel->rootEntity, worldPos);
}

} // namespace GameEngine
