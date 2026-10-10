#pragma once

#include "UI/Controls/DockPanel.h"
#include "UI/Interaction/ContextMenuManipulator.h"
#include "UI/Interaction/DropTarget.h"

#include "AssetCore/GUID.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "SceneView/SceneViewToolStrip.h"
#include "UI/Controls/Widgets/SceneViewMeasureOverlay.h"
#include "UI/Interaction/Types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine {
struct AssetLoadHandle;
namespace Mathematics { struct Vector3; }
namespace Engine::Renderer
{
class RenderServices;
}

struct EditorContext;
class INativeContextMenu;
namespace Platform { class Window; }
class UIElement;
class Label;
class SceneCameraSettingsPopup;
class SceneViewController;
class SceneViewToolbar;
class CameraBookmarksWidget;
class DownloadPillOverlay;

class SceneViewPanel : public DockPanel, public UI::Interaction::IDropTarget {
public:
    std::string_view DeclaredTabIconClass() const override { return "dock-scene-view-icon"; }

    enum class ViewportSlot : uint8_t
    {
        Perspective = 0,
        Top = 1,
        Front = 2,
        Side = 3,
        Count = 4
    };

    struct PerfBreakdown
    {
        double pollMs = 0.0;
        double inputMs = 0.0;
        double appUpdateMs = 0.0;
        double engineUpdateMs = 0.0;
        double beginFrameMs = 0.0;
        double thumbnailsMs = 0.0;
        double uiRecordMs = 0.0;
        double rgCompileMs = 0.0;
        double rgExecuteMs = 0.0;
        double presentMs = 0.0;
    };

    SceneViewPanel();
    ~SceneViewPanel() override; // Defined in .cpp where INativeContextMenu is complete
    void OnDockTabActivationArmed(float contentWidth, float contentHeight) override;

    void SetContext(const EditorContext* ctx);
    void SetChangeNotifications(Editor::EditorChangeNotifications* notifications);
    // Link this panel to its SceneViewController (not owned).
    void SetSceneController(SceneViewController* controller);
    void SetSceneControllerForSlot(ViewportSlot slot, SceneViewController* controller);
    SceneViewController* GetSceneControllerForSlot(ViewportSlot slot) const;
    // The controller of the pane `viewport` is, or null for an element that is no pane.
    SceneViewController* GetSceneControllerForViewport(const UIElement& viewport) const;
    // Expose current camera angles (degrees) so renderer can consume them
    float GetYawDeg() const { return m_Yaw; }
    float GetPitchDeg() const { return m_Pitch; }

    // Setter to camera angles
    void SetYawPitch(float yawDeg, float pitchDeg)
    {
        m_Yaw = yawDeg;
        m_Pitch = pitchDeg;
    }

    // Viewport element (used by the renderer to size the Scene View texture)
    UIElement* GetViewportElement() const { return m_Viewport; }
    UIElement* GetViewportElementForSlot(ViewportSlot slot) const;
    bool IsQuadViewEnabled() const { return m_QuadViewEnabled; }
    ViewportSlot GetActiveViewportSlot() const { return m_ActiveViewportSlot; }
    bool ActivateViewportForController(SceneViewController* controller);
    void ToggleQuadView();
    void NotifyRotationGizmoOrbit(size_t viewportIndex);
    void NotifyRotationGizmoAxisSnap(size_t viewportIndex, int axis);

    // Move keyboard focus to the active scene viewport (Unity-style scene view shortcuts).
    void FocusSceneViewViewport();

    // FPS/perf overlay label (nullptr before RegisterViewportEvents has run).
    UIElement* GetFpsLabelElement() const;

    // used by bookmark widget
    UIElement* GetPreviewPopup() const { return m_BookmarkPreviewPopup; }

    // Render Services from engine
    Engine::Renderer::RenderServices* GetRenderServices() const { return ResolveRenderServices(); }
    void SetRenderServices(Engine::Renderer::RenderServices* services) { m_RenderServices = services; }

    // Simple accessors for FPS-style navigation state
    bool IsLooking() const { return m_Dragging && m_DragButton == 1 && !m_IsAltOrbiting && !m_IsPanning && !m_IsDollying; }
    bool IsOrbiting() const { return (m_Dragging && m_DragButton == 2 && !m_IsPanning) || m_IsAltOrbiting; }
    bool IsPanning() const { return m_IsPanning; }
    bool IsDollying() const { return m_IsDollying; }

    // Returns true if orbiting just started this frame (for skipping look delta on first frame).
    // Also updates the internal tracking state, so call this once per frame before ApplyLookDelta.
    bool IsFirstOrbitFrame()
    {
        const bool nowOrbiting = IsOrbiting();
        const bool firstFrame = nowOrbiting && !m_WasOrbiting;
        m_WasOrbiting = nowOrbiting;
        return firstFrame;
    }

    // Apply yaw/pitch delta in degrees (driven by InputSystem look axes).
    void ApplyLookDelta(float deltaYawDeg, float deltaPitchDeg);

    // 2D mode + Shift: lock pan motion to horizontal or vertical from the gesture's direction.
    void ConstrainPanDelta2DShiftAxis(bool shiftHeld, float& deltaX, float& deltaY);

    // Update the small FPS/frametime overlay label (best-effort; no-op until bound).
    void UpdatePerformanceOverlay(double deltaSeconds, const PerfBreakdown& perf);
    void UpdatePerformanceOverlay(double deltaSeconds)
    {
        UpdatePerformanceOverlay(deltaSeconds, PerfBreakdown{});
    }
    
    // Update FPS overlay label in the viewport.
    void UpdateFPS(float deltaTime);

    // Controls whether FPS is shown in the perf label (toggle with P key).
    bool IsFpsVisible() const { return m_FpsVisible; }

    // Asset preview overlay (2D in Scene View)
    void SetAssetPreview(const std::filesystem::path& path, bool enabled);

    void OnPostLayout() override;

    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;

    // Called every frame from EditorApplication. If a billboard preview is active
    // and the early download has completed, swaps the billboard for the real 3D model.
    // Returns true if files were moved from cache to project assets.
    bool PollDragPreviewDownload();

    /// Capture hierarchy selection at drop time (before spawning entities).
    void SetCaptureHierarchySelectionForDrop(
        std::function<std::pair<std::vector<UI::Interaction::ItemId>, UI::Interaction::ItemId>()> cb)
    {
        m_CaptureHierarchySelectionForDrop = std::move(cb);
    }

    /// After a successful drop: refresh hierarchy, multi-select dropped roots, optional undo (see Editor wiring).
    /// Runs synchronously at end of PerformDrop so undo order stays before subsequent user actions.
    void SetOnSceneAssetDropComplete(std::function<void(ECS::World*,
                                                        const std::vector<ECS::EntityHandle>&,
                                                        const std::vector<UI::Interaction::ItemId>&,
                                                        UI::Interaction::ItemId)> cb)
    {
        m_OnSceneAssetDropComplete = std::move(cb);
    }

    /// Called once when the scene view toolbar has been bound (UXML loaded asynchronously).
    /// If the toolbar is already bound, the callback fires immediately.
    void SetOnToolbarReady(std::function<void(SceneViewToolbar*)> cb)
    {
        m_OnToolbarReady = std::move(cb);
        if (m_Toolbar && m_OnToolbarReady)
            m_OnToolbarReady(m_Toolbar);
    }

    /// The window child widgets anchor their own native menus to.
    Platform::Window* GetWindow() const { return m_Window; }

    // Right-click menu on the FPS overlay label.
    void ShowFpsContextMenu(float windowX, float windowY);

    // Left-click on the camera toolbar button: quick-settings popup for the
    // scene camera (FOV, clip planes, speed/acceleration, exposure), anchored
    // below the button. Coordinates are in window space.
    void ShowCameraSettingsPopup(float windowX, float windowY);

    // An open camera popup re-reads the settings it edits. The toolbar's
    // post-process menu calls this after the auto-exposure toggle seeds Fixed
    // EV100 (AE-lock), or the popup's stale field would clobber the seed.
    void SyncCameraSettingsPopupFromSettings();

    // Wired by EditorApplication so the spline menu can open a native color
    // picker window for knot/spline colors.
    void SetOpenColorPickerWindow(OpenColorPickerWindowFn fn) { m_OpenColorPickerWindow = std::move(fn); }

private:
    // BindFromAssetsDeferred and it's inner setup functions
    void BindFromAssetsDeferred();
    void LoadBindAttachLayoutAndStyle(UIManager* ui);
    void MarkBindFailed(std::string_view reason);
    void RefreshElementPointers();
    void RegisterViewportEvents();
    void BindToolbarWidgets();
    void WireToolOverlay();
    void ApplySceneToolIconBackgrounds();
    void RefreshCameraSettingsPopupLayout();
    void PrepareToolOverlayReveal();
    void RevealToolOverlayIfEnabled(uint32_t generation);
    void UpdateToolOverlayModeButtons();
    // The strips' registered entries (SceneView/SceneViewToolStripRegistry.h): a button
    // per entry after the built-in ones in the floating strip and the inline mirror, and
    // its badge button when the entry has one.
    void PopulateRegisteredToolStripEntries();
    // A click on a registered entry's button: its tool, or back to the transform tool.
    void ToggleRegisteredTool(const std::string& id);
    // The registered buttons' active state, when the view's tool changes; then a refresh.
    void UpdateRegisteredToolStripEntries();
    // The visible strip's availability and badge counts, once per frame; an active
    // registered tool that stops being available returns the view to the transform tool.
    void RefreshRegisteredToolStrips();
    void UpdateToolOverlaySpaceButton();
    enum class QuadSplitDragAxis : uint8_t { None, VerticalTop, VerticalBottom, Horizontal };
    enum class QuadViewKind : uint8_t { Perspective = 0, Top, Front, Side, Count };
    // Applies the persisted toolbar icon visibility (right-click toggle menu).
    void ApplyToolbarIconVisibility();
    // Shows the right-click "hide icons" menu for the scene view toolbar.
    void ShowToolbarIconContextMenu(float windowX, float windowY);
    void UpdateHoverHighlightPill();
    void UpdateQuadViewButtonState();
    void ResetQuadViewSplits();
    void ResetQuadViewAssignments();
    void SetQuadViewKind(ViewportSlot slot, QuadViewKind kind);
    QuadViewKind GetQuadViewKind(ViewportSlot slot) const;
    SceneViewController* GetSceneControllerForQuadViewKind(QuadViewKind kind) const;
    ViewportSlot FindSlotForQuadViewKind(QuadViewKind kind) const;
    bool ToggleActiveViewport2DMode();
    void ApplyQuadViewKind(ViewportSlot slot);
    void UpdateQuadViewLabel(ViewportSlot slot);
    static const char* GetQuadViewKindLabel(QuadViewKind kind);
    static const char* GetQuadViewKindMenuLabel(QuadViewKind kind);
    static const char* GetViewportSlotMenuLabel(ViewportSlot slot);
    static bool IsQuadViewKindAllowedForSlot(ViewportSlot slot, QuadViewKind kind);
    UIElement* GetActiveViewportElement() const;
    UIElement* GetViewportForOverlay() const;
    UIElement* FindViewportOverlayById(const char* id) const;
    bool ViewportOverlaysNeedSync() const;
    void RequestViewportOverlaySync();
    void RefreshOverlayElementPointers();
    void RefreshViewportRotationGizmos();
    void RefreshViewportRulerOverlays();
    void RefreshViewportMeasureOverlays();
    void SyncViewportOverlays();
    static void GetMayaQuadCorner(ViewportSlot slot, bool& leftColumn, bool& topRow);
    void ApplyQuadViewLayout();
    void SetActiveViewportSlot(ViewportSlot slot);
    void SetHoveredViewportSlot(ViewportSlot slot);
    ViewportSlot ResolveSpaceToggleViewportSlot(ViewportSlot fallback) const;
    void BeginViewportSpaceToggle(ViewportSlot fallback, UIEvent& ev);
    void EndViewportSpaceToggle(ViewportSlot fallback, UIEvent& ev);
    void BeginCameraDragTracking(float x, float y);
    void EndCameraDragTracking();
    bool ApplyImmediatePan(SceneViewController* controller, UIElement* viewport, UIEvent& ev);
    bool HandleSceneViewCommandShortcut(SceneViewController* controller, UIEvent& ev);
    void FocusViewport(UIElement* viewport);
    void SetMeasureToolEnabled(bool enabled);
    void ToggleMeasureTool();
    void UpdateMeasureToolButtons();
    void ApplyMeasureSoloMode(bool enabled);
    // The tool buttons' menus, declared at their attach sites. The select and
    // measure sets are fixed with show-time state hooks; spline and quad-view
    // sets are computed per show (curve-type-gated submenus, per-slot rows).
    std::vector<ContextMenuManipulator::Item> BuildSelectToolMenuItems();
    std::vector<ContextMenuManipulator::Item> BuildMeasureMenuItems();
    std::vector<ContextMenuManipulator::Item> BuildQuadViewMenuItems();
    void SetMeasureUnitSystem(SceneViewMeasureOverlay::UnitSystem unitSystem);
    void SetMeasureTwoDMode(SceneViewMeasureOverlay::TwoDMode mode);
    ECS::EntityHandle CreateMeasureEntity(const std::array<float, 3>& start,
                                          const std::array<float, 3>& end,
                                          bool is2D);
    bool HandleMeasurePointerDown(SceneViewController* controller, UIElement* viewport, ViewportSlot slot, UIEvent& ev);
    bool HandleMeasurePointerMove(SceneViewController* controller, UIElement* viewport, ViewportSlot slot, UIEvent& ev);
    bool HandleMeasurePointerUp(SceneViewController* controller, UIElement* viewport, ViewportSlot slot, UIEvent& ev);
    void FinishMeasureDrag(bool allowCreateEntity);
    void FinishMeasureDragIfReleased();
    void RegisterAuxViewportEvents(UIElement* viewport, ViewportSlot slot);
    void RegisterQuadSplitHandleEvents();
    void BeginQuadSplitDrag(UIElement* handle, QuadSplitDragAxis axis, UIEvent& ev);
    void UpdateQuadSplitDrag(UIEvent& ev);
    void EndQuadSplitDrag(UIEvent& ev);
    void EnsureAssetPreviewElements();
    void UpdateAssetPreviewVisual();
    void ApplyAssetPreviewBackground(const std::string& relOrEngine);

    void ClearDropPreviewModel();
    // Spawns a loaded model where a drop aimed: its entities, its exported materials, its
    // place at the end of the hierarchy. Returns the root, or an invalid handle (logged)
    // when the model did not load.
    ECS::EntityHandle SpawnDroppedModel(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                                        const GUID& modelGuid, const std::filesystem::path& modelPath,
                                        const std::string& name, const Mathematics::Vector3& worldPos);
    // Spawns a dropped model once its load has landed and records the drop
    // (CompleteDeferredDrop). Runs from RunWhenAssetLoaded, which drops it when a scene
    // opened meanwhile cleared the world the drop was aimed at.
    void SpawnDeferredModelDrop(ECS::World* world, Engine::Renderer::RenderServices* renderServices,
                                const GUID& modelGuid, const std::filesystem::path& modelPath,
                                const std::string& name, const Mathematics::Vector3& worldPos,
                                const std::vector<UI::Interaction::ItemId>& selectionBefore,
                                UI::Interaction::ItemId anchorBefore);
    // Records a drop whose root appeared once its asset loaded, as an immediate drop is
    // recorded: the structure notification, the scene's dirty mark, and the drop
    // completion (the selection and one undo step).
    void CompleteDeferredDrop(ECS::World& world, ECS::EntityHandle root,
                              const std::vector<UI::Interaction::ItemId>& selectionBefore,
                              UI::Interaction::ItemId anchorBefore);
    // The download manager's pill overlay, or nullptr without a download manager.
    DownloadPillOverlay* GetDownloadPillOverlay() const;
    // Publishes `viewport` to the view overlay host and hands the download pill overlay the
    // viewport and the overlay layer its pills stack under.
    void PublishOverlayLayer(UIElement& viewport);

    Engine::Renderer::RenderServices* ResolveRenderServices() const;

    // did we bind widgets to layout?
    bool m_LayoutBound = false;
    bool m_ViewportEventsBound = false;
    bool m_AuxViewportEventsBound = false;
    bool m_QuadSplitHandleEventsBound = false;
    bool m_QuadViewEnabled = false;
    bool m_ViewportOverlaySyncPosted = false;
    ViewportSlot m_ActiveViewportSlot = ViewportSlot::Perspective;
    bool m_HasHoveredViewportSlot = false;
    ViewportSlot m_HoveredViewportSlot = ViewportSlot::Perspective;
    bool m_SpacePressedForQuadToggle = false;
    bool m_SpacePanDragStarted = false;
    QuadSplitDragAxis m_QuadSplitDragAxis = QuadSplitDragAxis::None;
    float m_QuadSplitTopX = 0.5f;
    float m_QuadSplitBottomX = 0.5f;
    float m_QuadSplitY = 0.5f;
    std::array<QuadViewKind, static_cast<size_t>(ViewportSlot::Count)> m_QuadViewKinds{
        QuadViewKind::Perspective,
        QuadViewKind::Top,
        QuadViewKind::Front,
        QuadViewKind::Side
    };

    // Viewport element to capture input for editor camera
    UIElement* m_Viewport = nullptr; // not owned
    UIElement* m_OverlayViewport = nullptr; // not owned
    std::array<UIElement*, static_cast<size_t>(ViewportSlot::Count)> m_ViewportSlots{};
    std::array<Label*, static_cast<size_t>(ViewportSlot::Count)> m_ViewportLabels{};
    UIElement* m_QuadSplitVerticalTop = nullptr; // not owned
    UIElement* m_QuadSplitVerticalBottom = nullptr; // not owned
    UIElement* m_QuadSplitHorizontal = nullptr; // not owned

    // EngineRender Services
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;

    // Scene View controllers keyed by their default view kind (not owned).
    SceneViewController* m_Controller = nullptr; // not owned
    std::array<SceneViewController*, static_cast<size_t>(ViewportSlot::Count)> m_SceneControllers{};
    SceneViewToolbar* m_Toolbar = nullptr; // not owned
    Label* m_FpsLabel = nullptr; // not owned (optional)

    // The viewport last published to the view overlay host.
    UIElement::WeakRef<UIElement> m_PublishedOverlayViewport;

    Label* m_HoverHighlightLabel = nullptr; // not owned (optional)
    // Hover highlight pill: cache layout inputs so we don't relayout or re-anchor every frame (reduces jitter).
    std::string m_LastHoverPillLayoutText;
    float m_LastHoverPillViewportWidth = -1.0f;
    float m_LastHoverPillAnchoredLabelWidth = -1.0f;
    float m_LastHoverPillAnchorMouseLocalX = -1.0f;
    float m_LastHoverPillAnchorMouseLocalY = -1.0f;
    bool m_LastHoverPillUsedCursorPlacement = false;

    bool m_SceneViewportPointerInside = false;
    float m_SceneViewportPointerLocalX = 0.0f;
    float m_SceneViewportPointerLocalY = 0.0f;

    // Per-viewport orientation gizmos (Maya: one view cube per pane in quad layout).
    std::array<class ViewportRotationGizmo*, static_cast<size_t>(ViewportSlot::Count)> m_RotationGizmos{};
    class ViewportRotationGizmo* m_RotationGizmo = nullptr; // active slot, not owned

    // Per-viewport 2D mode ruler + magnification overlays (fill each viewport) — not owned.
    std::array<class SceneViewRulerOverlay*, static_cast<size_t>(ViewportSlot::Count)> m_RulerOverlays{};
    class SceneViewRulerOverlay* m_RulerOverlay = nullptr; // active slot, not owned
    std::array<class SceneViewMeasureOverlay*, static_cast<size_t>(ViewportSlot::Count)> m_MeasureOverlays{};
    class SceneViewMeasureOverlay* m_MeasureOverlay = nullptr; // active slot, not owned
    bool m_MeasureToolEnabled = false;
    bool m_MeasureDragActive = false;
    bool m_MeasureCreateEntitiesMode = false;
    ViewportSlot m_MeasureDragSlot = ViewportSlot::Perspective;
    std::array<float, 3> m_MeasureDragStartWorld{0.0f, 0.0f, 0.0f};
    std::array<float, 3> m_MeasureDragEndWorld{0.0f, 0.0f, 0.0f};
    bool m_MeasureDragIs2D = false;
    SceneViewMeasureOverlay::UnitSystem m_MeasureUnitSystem = SceneViewMeasureOverlay::UnitSystem::Metric;
    SceneViewMeasureOverlay::TwoDMode m_MeasureTwoDMode = SceneViewMeasureOverlay::TwoDMode::Triangle;
    std::array<std::uint8_t, static_cast<size_t>(ViewportSlot::Count)> m_MeasureSavedToolKinds{};
    std::array<bool, static_cast<size_t>(ViewportSlot::Count)> m_MeasureSavedTransformGizmosVisible{};
    std::array<bool, static_cast<size_t>(ViewportSlot::Count)> m_MeasureSavedControllerState{};

    // Camera quick-settings popup (lazy-created, owned by this panel's UI tree).
    SceneCameraSettingsPopup* m_CameraSettingsPopup = nullptr;
    bool m_CameraSettingsPopupLayoutScheduled = false;

    // Camera bookmarks
    uint64_t m_CameraBookmarksBoundInstanceId = 0; // last bound instance id (best-effort, avoids stale pointers)
    UIElement* m_BookmarkPreviewPopup = nullptr; // owned
    UIElement* m_BookmarkPreviewImage = nullptr; // owned
    UIElement* m_AssetPreviewOverlay = nullptr; // owned
    UIElement* m_AssetPreviewImage = nullptr; // owned
    std::filesystem::path m_AssetPreviewPath;
    bool m_AssetPreviewEnabled = false;

    // Minimal editor camera state (placeholder until rendering uses it)
    float m_Yaw   = 0.0f; // degrees
    float m_Pitch = 0.0f; // degrees
    bool  m_Dragging   = false;
    int   m_DragButton = -1; // 0=LMB,1=RMB,2=MMB
    bool  m_CameraDragLastMouseValid = false;
    float m_CameraDragLastMouseX = 0.0f;
    float m_CameraDragLastMouseY = 0.0f;

    const EditorContext* m_Context = nullptr; // not owned
    Platform::Window* m_Window = nullptr; // not owned
    // Shared by the panel's three NON-manipulator menus — the viewport script-hook menu,
    // the toolbar icon-toggle menu, and the FPS overlay menu — cleared and rebuilt per
    // show. Every tool-button menu is owned by its ContextMenuManipulator instead.
    std::unique_ptr<INativeContextMenu> m_ContextMenu;
    Editor::EditorChangeNotifications* m_ChangeNotifications = nullptr; // not owned

    // RMB click vs drag detection for context menu
    bool  m_RmbCandidateMenu = false;
    float m_RmbDownX = 0.0f;
    float m_RmbDownY = 0.0f;

    // Drag-to-reposition state for the floating vertical tool overlay.
    // Activated by pressing the Select (arrow) button and dragging past a
    // small pixel threshold; repositions the overlay instead of triggering
    // the button's click.
    struct ToolOverlayDragState
    {
        bool  pressed       = false;
        bool  active        = false;
        float startMouseX   = 0.0f;
        float startMouseY   = 0.0f;
        float startOverlayX = 0.0f;
        float startOverlayY = 0.0f;
    };
    ToolOverlayDragState m_ToolOverlayDrag;
    uint32_t m_ToolOverlayRevealGeneration = 0;
    Editor::RegisteredToolStrip m_FloatingToolStrip; // the floating strip's registered entries
    Editor::RegisteredToolStrip m_InlineToolStrip;   // the inline mirror's registered entries

    // Drag-to-reposition state for the FPS/perf overlay label in the viewport.
    // Activated by pressing LMB on the label and dragging past a small pixel
    // threshold; switches from top/right anchoring to top/left pixel position.
    struct FpsLabelDragState
    {
        bool  pressed     = false;
        bool  active      = false;
        float startMouseX = 0.0f;
        float startMouseY = 0.0f;
        float startLabelX = 0.0f;
        float startLabelY = 0.0f;
    };
    FpsLabelDragState m_FpsLabelDrag;
    bool m_FpsContextMenuArmed = false;

    // UXML/CSS binding for panel contents (toolbar + viewport). m_BindPending covers the
    // whole attempt — the queued action AND the async asset load it starts — so a bind in
    // flight is not re-armed by every layout pass while the .uxml resolves. Every path
    // that can end the attempt releases it: applied, failed, "panel detached, try again",
    // and the deferred action being dropped outright (PostAction returning false, which
    // is why OnPostLayout checks it).
    bool m_BindApplied = false;
    bool m_BindPending = false;
    bool m_BindFailed = false;

    // Async asset loads for panel-local UI assets. Stored so callbacks are cancelled if the panel is destroyed.
    std::unique_ptr<AssetLoadHandle> m_LayoutLoadHandle;
    std::unique_ptr<AssetLoadHandle> m_ThemeStyleLoadHandle;
    std::unique_ptr<AssetLoadHandle> m_PanelStyleLoadHandle;

    void ClearPanShift2DConstraint();

    enum class Pan2DShiftAxisLock : uint8_t
    {
        None,
        Horizontal,
        Vertical,
    };

    Pan2DShiftAxisLock m_Pan2DShiftAxisLock = Pan2DShiftAxisLock::None;
    float m_Pan2DShiftCumDx = 0.0f;
    float m_Pan2DShiftCumDy = 0.0f;
    bool m_PanShift2DWasHeld = false;

    // Editor camera modes (detected via modifiers)
    bool m_IsAltOrbiting = false;   // Alt + LMB
    bool m_IsPanning = false;       // Alt + Ctrl/Cmd + LMB (or Alt/Space + LMB in 2D)
    bool m_IsDollying = false;      // Alt + Ctrl/Cmd + RMB
    bool m_WasOrbiting = false;     // Previous frame's orbit state (for first-frame detection)
    bool m_LmbDragIsToolMode = false; // LMB drag started as tool (gizmo/marquee), not camera
    bool m_SpaceHeld = false;       // Spacebar held (used for space+LMB panning in 2D)

    // FPS display in perf label (toggle with P key)
    bool m_FpsVisible = true;
    // True when the FPS label is using the auto-placed default position
    // (no persisted x/y). Reset on user drag so we stop auto-adjusting.
    bool m_FpsUsingDefaultPosition = false;
    // Last value read from Application::GetFps() — cached so we can avoid
    // re-resolving on every UpdateFPS() call when nothing has changed.
    float m_CurrentFPS = 0.0f;
    // Accumulator throttling FPS-label text refreshes (see UpdateFPS).

    std::optional<Engine::Renderer::ModelEntityResult> m_DropPreviewModel;
    GUID m_DropPreviewAssetGuid{};

    ECS::EntityHandle m_DropPreviewBillboard{};
    std::string m_DropPreviewOnlineSlug;

    // Entity currently outlined as the drop target for a texture-on-mesh drag.
    // Cleared when the drag leaves the viewport or the payload stops being a texture.
    ECS::EntityHandle m_DropTargetHighlight{};

    std::function<std::pair<std::vector<UI::Interaction::ItemId>, UI::Interaction::ItemId>()>
        m_CaptureHierarchySelectionForDrop;
    std::function<void(ECS::World*,
                       const std::vector<ECS::EntityHandle>&,
                       const std::vector<UI::Interaction::ItemId>&,
                       UI::Interaction::ItemId)>
        m_OnSceneAssetDropComplete;
    std::function<void(SceneViewToolbar*)> m_OnToolbarReady;
    OpenColorPickerWindowFn m_OpenColorPickerWindow;
};

} // namespace GameEngine
