#pragma once

#include "ECS/ECS.h"
#include "Editor/Settings/ProjectUIScaleBinding.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Mathematics/Ray.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "SceneView/SceneViewTools.h"
#include "SceneView/ViewPresentationSnapshot.h"
#include "SceneView/SelectionTool.h"
#include "SceneView/TransformTool.h"
#include "SceneView/NavDebugGizmo.h"
#include "SceneView/NavGridBrushTool.h"
#include "SceneView/ComponentGizmos.h"
#include "SceneView/Gizmos/DDGIVolumeGizmo.h"
#include "SceneView/ReflectionProbeGizmo.h"
#include "SceneView/TerrainModifierGizmo.h"
#include "SceneView/LightGizmo.h"
#include "SceneView/CameraFrameGizmo.h"
#include "SceneView/MeasureSceneGizmo.h"
#include "Markups/MarkupGizmo.h"
#include "SceneView/SceneViewGridRenderer.h"
#include "SceneView/SplineSceneGizmo.h"
#include "UI/UITextureSpace.h"
#include <algorithm>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{

namespace Editor::Picking
{
struct PickHit;
}

namespace Rendering
{
namespace RenderGraph
{
class RGFrame;
struct RGTexture;
}
}

namespace Engine::Renderer
{
class ExposureReadbackFeature;
class RenderServices;
namespace Pipeline
{
struct ViewTargetsRG;
}
}

struct SceneViewCameraPose
{
    float Pos[3];
    float YawDeg;
    float PitchDeg;
    float Distance;
    bool Is2D = false;
};

class CameraBookmarksWidget;
class GameUIHost;

class SceneViewController
{
  public:
    enum class FixedViewOrientation : std::uint8_t
    {
        Free = 0,
        Top,
        Front,
        Side
    };

    enum class ToolKind : std::uint8_t
    {
        Selection = 0,
        Transform,
        NavGridBrush,
        // A tool another module registered with the tool strip
        // (SceneView/SceneViewToolStripRegistry.h); GetActiveRegisteredToolId names it.
        Registered
    };

    /// What last updated the hovered entity: the Scene View's ray, or a panel's row or link
    /// (the Hierarchy's preview, a mark-up row, an entity link) hovered outside the view.
    enum class HoverEntitySource : std::uint8_t
    {
        SceneViewPointer,
        Panel,
    };

    SceneViewController(Engine::Renderer::RenderServices* renderServices, ECS::World& world);
    ~SceneViewController();

    // RenderGraph arm (slice 7d): refresh view/camera registry state and import this
    // view's pool targets into the frame; returns false when the view cannot
    // render. The WINDOW DRIVER makes the single BuildFrameGraph call with
    // the collected targets — controllers never call the spine. Overlays
    // (gizmos/outline) land in 7e; bookmark previews stay old-arm (7g).
    bool DeclareTargetsRG(Rendering::RenderGraph::RGFrame& frame, uint32_t width, uint32_t height,
                           uint64_t windowId,
                           Engine::Renderer::Pipeline::ViewTargetsRG& outTargets);
    void PrepareForActivation();
    bool IsWaitingForExtraction() const { return m_WaitingForExtractionRG; }
    bool UpdatePresentedSnapshotRG(Rendering::RenderGraph::RGFrame& frame,
                                   Rendering::RenderGraph::RGTexture source,
                                   UI::UITextureSpace sourceSpace);
    // Invalid once `device` has been rebuilt since the copy was written — the
    // handle would otherwise be a freed id, and ImportExternalTexture takes it
    // at face value.
    Rendering::TextureHandle GetLastPresentedTexture(const Rendering::IDevice& device) const
    {
        return m_PresentationSnapshot.Texture(device);
    }
    uint32_t GetLastPresentedWidth() const { return m_PresentationSnapshot.Width(); }
    uint32_t GetLastPresentedHeight() const { return m_PresentationSnapshot.Height(); }
    // The space the last-presented copy was rendered in (#767) — what the
    // waiting-for-extraction re-publish must carry, not the current mode.
    UI::UITextureSpace GetLastPresentedSpace() const { return m_PresentationSnapshot.Space(); }
    // RenderGraph arm (slice 7e): declare the editor overlay passes — gizmos into a
    // transient GizmoColor, selection mask + Sobel outline, and the gizmo
    // composite — as fresh per-frame passes attaching the pipeline's ACTUAL
    // FinalColor (out.Out from GetPipelineOutputRG, never the imported
    // Color/Resolve) with Load. Call AFTER the spine; all live state is
    // snapshotted at declaration (exec lambdas capture by value).
    // Implemented in SceneViewOverlaysRG.cpp.
    void DeclareOverlaysRG(Rendering::RenderGraph::RGFrame& frame,
                            Rendering::RenderGraph::RGTexture finalColor,
                            Rendering::RenderGraph::RGTexture depth,
                            Engine::Renderer::RenderServices* rs);
    // RenderGraph arm (slice 8c-3): declares the bookmark preview as a THIRD span
    // view while a render is pending. Pool-imported 128×72 targets, the same
    // serial machinery as the old arm, and the serial CLOSES here —
    // declaration IS submission in immediate mode. Returns false when no
    // preview render is pending.
    bool DeclarePreviewTargetsRG(Rendering::RenderGraph::RGFrame& frame, uint64_t windowId,
                                  Engine::Renderer::Pipeline::ViewTargetsRG& outTargets);
    // Post-spine half (call AFTER BuildFrameGraph): on the serial-close frame
    // copies the preview pipeline's output into the controller-owned device
    // snapshot the popup binds (immune to pool aging — the popup may stay
    // open indefinitely), and arms any pending widget screenshot tickets.
    void FinalizePreviewRG(Rendering::RenderGraph::RGFrame& frame);
    void DeactivateRenderView();
    void SetRenderNamePrefix(std::string prefix) { m_RenderNamePrefix = std::move(prefix); }
    void SetFrameGraphBuildEnabled(bool enabled) { m_FrameGraphBuildEnabled = enabled; }
    void SetRenderSampleCountOverride(uint32_t samples) { m_RenderSampleCountOverride = samples; }
    void SetMeasureViewportScaleCompensation(float scale) { m_MeasureSceneGizmo.SetViewportScaleCompensation(scale); }
    void SetFixedViewOrientation(FixedViewOrientation orientation);
    FixedViewOrientation GetFixedViewOrientation() const { return m_FixedViewOrientation; }
    /** 2D and fixed-axis views pan and zoom where a free 3D view tumbles and orbits. */
    bool UsesFixedViewPanControls() const
    {
        return m_Is2DMode || m_FixedViewOrientation != FixedViewOrientation::Free;
    }

    // Exposing current open View ID
    Rendering::ViewId GetViewId() const { return m_ViewId; }
    Rendering::CameraId GetCameraId() const { return m_CameraId; }

    // Exposing Render Service
    Engine::Renderer::RenderServices* GetRenderServices() const { return m_RenderServices; }

    // The projection the Scene View last submitted, with the viewport it was
    // built from. The pair is captured in one assignment, so a reader can never
    // combine this frame's matrix with the next frame's size across a resize.
    // Returns false until the first frame has been recorded.
    bool GetLastFrameProjection(float outProj[16],
                                std::uint32_t& outViewportWidthPx,
                                std::uint32_t& outViewportHeightPx) const
    {
        if (!m_HasFrameCameraData)
            return false;
        for (int i = 0; i < 16; ++i)
            outProj[i] = m_FrameCameraData.proj[i];
        outViewportWidthPx = m_FrameCameraViewportW;
        outViewportHeightPx = m_FrameCameraViewportH;
        return true;
    }

    // Exposing SceneView camera pose
    SceneViewCameraPose GetCameraPose() const
    {
        SceneViewCameraPose p{};
        p.Pos[0] = static_cast<float>(m_CamPos[0]);
        p.Pos[1] = static_cast<float>(m_CamPos[1]);
        p.Pos[2] = static_cast<float>(m_CamPos[2]);
        p.YawDeg = m_CamYawDeg;
        p.PitchDeg = m_CamPitchDeg;
        p.Distance = m_CamDistance;
        p.Is2D = m_Is2DMode;
        return p;
    }

    void SetCameraPose(const SceneViewCameraPose& p)
    {
        ResetFlyVelocity(); // teleports (bookmarks, 2D/3D toggle) drop fly momentum
        m_CamPos[0] = p.Pos[0];
        m_CamPos[1] = p.Pos[1];
        m_CamPos[2] = p.Pos[2];
        m_CamYawDeg = p.YawDeg;
        m_CamPitchDeg = p.PitchDeg;
        m_CamDistance = p.Distance;
        m_Is2DMode = p.Is2D;
        Sync2DModeToTransformTool();

        // update orbit so it doesn't yank the camera back to pivot
        UpdateOrbit(false);
    }

    // Camera control from UI (degrees). Distance is used for potential orbit/zoom controls.
    void SetCameraAnglesDeg(float yaw, float pitch)
    {
        if (IsTweenActive())
        {
            return; // Ignore UI panel during tween
        }

        m_CamYawDeg = yaw;
        m_CamPitchDeg = pitch;
    }
    void SetCameraDistance(float dist) { m_CamDistance = (dist > 0.1f) ? dist : 0.1f; }

    void SetBookmarksWidget(CameraBookmarksWidget* widget) { m_BookmarksWidget = widget; }

    // returns if we are blocking SceneView camera from Input
    bool IsTweenActive() const { return m_CameraTween.Active; }

    // Stating a tween to target pose
    void StartCameraTween(const SceneViewCameraPose& target, float duration = 0.7f);

    // Called when the tween ends / player pressed input
    void OnCameraTweenFinish();

    // General Update for scene view
    void Update(float deltaSeconds, bool moveForward, bool moveBackward, bool moveLeft, bool moveRight,
                bool moveUp, bool moveDown, float speedMultiplier);

    // FPS-style camera integration step (called once per frame from the editor).
    // speedMultiplier allows for faster movement (e.g., 2.0f when Shift is held).
    // moveUp / moveDown are world +Y / -Y (Q/E while RMB look is held).
    void UpdateCamera(float deltaSeconds, bool moveForward, bool moveBackward, bool moveLeft, bool moveRight,
                      bool moveUp = false, bool moveDown = false, float speedMultiplier = 1.0f);

    // Update cached viewport dimensions before UpdateCamera so frustum culling
    // uses the current frame's layout size during the ECS update phase.
    void SetLastViewportSize(uint32_t w, uint32_t h)
    {
        const bool sizeChanged = (w != m_LastW || h != m_LastH);
        m_LastW = w;
        m_LastH = h;
        if (sizeChanged)
            ApplyPixelPerfectIfActive();
    }

    // Orbit-style camera update (keeps the camera revolving around a pivot while yaw/pitch change).
    void UpdateOrbit(bool orbiting);

    // Pan camera (strafe horizontally/vertically) based on mouse delta in pixels.
    // Orthographic panning uses viewportHeightPx to preserve 1:1 screen/world motion.
    // Used by Scene View camera drag modes.
    void UpdatePan(float deltaX, float deltaY, float viewportHeightPx = 0.0f);

    // Dolly camera (zoom in/out) based on mouse delta in pixels.
    // Used for Alt+Ctrl/Cmd+RMB drag.
    void UpdateDolly(float deltaY);
    bool ZoomOrthographicStep(int direction);

    // Gizmos and framing controls.
    void ToggleGizmos() { m_ShowGizmos = !m_ShowGizmos; }
    void SetGizmosVisible(bool visible) { m_ShowGizmos = visible; }
    bool AreGizmosVisible() const { return m_ShowGizmos; }

    // Per-gizmo-type visibility, gated by the master toggle above: the overlay pass
    // reads these only while gizmos are visible.
    bool AreTransformGizmosVisible() const { return m_ShowTransformGizmos; }
    void SetTransformGizmosVisible(bool v) { m_ShowTransformGizmos = v; }
    bool AreLightGizmosVisible() const { return m_ShowLightGizmos; }
    void SetLightGizmosVisible(bool v) { m_ShowLightGizmos = v; }
    // Mark-up volumes and their labels.
    bool AreMarkupGizmosVisible() const { return m_ShowMarkupGizmos; }
    void SetMarkupGizmosVisible(bool v) { m_ShowMarkupGizmos = v; }

    // Camera Frame Guide: overlay the active game camera's visible frame as a
    // blue line box (rect in 2D/ortho, frustum in 3D). Persisted to settings.
    bool GetCameraFrameGuide() const { return m_ShowCameraFrameGuide; }
    void SetCameraFrameGuide(bool v);
    
    // Grid visibility controls.
    void ToggleGrid() { m_ShowGrid = !m_ShowGrid; }
    void SetGridVisible(bool visible) { m_ShowGrid = visible; }
    bool IsGridVisible() const { return m_ShowGrid; }

    // Post-processing master toggle for the Scene View (LDR stack + tonemap curve).
    // When disabled, a per-view PostProcessSettings override strips bloom, color filter,
    // CAS, CRT, and uses Neutral tonemap; gameplay views are unaffected.
    void TogglePostProcessing();
    void SetPostProcessingEnabled(bool enabled);
    bool IsPostProcessingEnabled() const { return m_PostProcessingEnabled; }

    // Grid snapping.
    void ToggleGridSnap() { m_GridSnapEnabled = !m_GridSnapEnabled; }
    void SetGridSnapEnabled(bool enabled) { m_GridSnapEnabled = enabled; }
    bool IsGridSnapEnabled() const { return m_GridSnapEnabled; }
    float GetGridSnapSize() const { return m_GridSnapSize; }
    void SetGridSnapSize(float size) { m_GridSnapSize = (size > 0.001f) ? size : 0.001f; }

    // Grid opacity.
    float GetGridOpacity() const { return m_GridOpacity; }
    void SetGridOpacity(float opacity) { m_GridOpacity = std::clamp(opacity, 0.0f, 1.0f); }

    // 2D/3D mode: orthographic side view (looking along +Z, Y up) vs perspective.
    // Switching tweens the camera to/from the locked 2D pose and flips projection.
    void Toggle2DMode();
    bool Is2DMode() const { return m_Is2DMode; }
    // Returns the 2D state the camera is transitioning to (or current state if no tween).
    bool Is2DTarget() const { return m_CameraTween.Active ? m_CameraTween.To.Is2D : m_Is2DMode; }
    // Explicitly set 2D/3D mode (used by bookmark recall). When entering 2D, saves
    // the current 3D pose so Toggle2DMode can restore it later.
    void Set2DMode(bool is2D);

    // Pixel-perfect mode for 2D views. When enabled and the view is in 2D mode,
    // the orthographic visible height is locked so one world unit maps to an
    // integer number of screen pixels (pixelScale). Must be called whenever the
    // user changes the setting so the projection updates immediately.
    void RefreshPixelPerfect2D();

    // Reference texel count one world unit represents in pixel-perfect 2D. The
    // Set the 2D ortho visible height. In pixel-perfect mode, snaps to the
    // largest power-of-2 scale (1..64) whose visible height still encloses
    // the requested height.
    void SetOrthoHeight2D(float requestedHeight);

    // In 2D + pixel-perfect mode, cycle the pixel scale by `scaleDelta` steps
    // (+1 = zoom in / higher scale, -1 = zoom out) while keeping the world
    // point currently under the cursor anchored on screen. localX/localY are
    // viewport-local pixel coordinates; viewW/viewH are the viewport size.
    // Returns true if the scale changed.
    bool ZoomPixelPerfectAtCursor(int scaleDelta, float localX, float localY, float viewW, float viewH);

    // Perspective/orthographic toggle for the main 3D camera (independent of
    // the top-down 2D mode — orthographic keeps the current orbit orientation
    // but swaps the projection matrix). Rig size matches the current orbit
    // distance so objects stay roughly the same apparent size when flipping.
    void ToggleOrthographic() { m_Orthographic = !m_Orthographic; }
    bool IsOrthographic() const { return m_Orthographic; }

    // Scene camera world position, for picking helpers that size camera-facing
    // icons consistently with rendering. The authoritative position is double
    // (planet-scale precision); this rounds to fp32, matching what rendering
    // receives.
    Mathematics::Vector3 GetCameraWorldPos() const
    {
        return Mathematics::Vector3(static_cast<float>(m_CamPos[0]),
                                    static_cast<float>(m_CamPos[1]),
                                    static_cast<float>(m_CamPos[2]));
    }

    // Vertical extent of the scene camera's orthographic frustum in world
    // units when the view is in 2D or orthographic mode; zero in perspective
    // mode. Used by camera-facing icons to keep a constant on-screen size.
    float GetGizmoOrthoHeightOrZero() const
    {
        return (m_Is2DMode || m_Orthographic) ? m_CamDistance : 0.0f;
    }
    void SetOrthographic(bool ortho) { m_Orthographic = ortho; }

    // Transform mode (translate / rotate / scale).
    void SetTransformMode(Editor::SceneTools::TransformMode mode);
    Editor::SceneTools::TransformMode GetTransformMode() const;

    // Transform axis space: world-aligned vs entity-local.
    void ToggleTransformSpace();
    bool IsTransformSpaceLocal() const;
    void FrameOrigin();
    void FrameAll();
    // Build a world-space gizmo ray from a Scene View pixel position.
    Editor::SceneTools::GizmoRay MakeGizmoRay(float viewX, float viewY, float viewWidth, float viewHeight) const;

    // Fills the camera state fields (basis, fov/ortho) on a ScenePointerEvent.
    // Call after setting viewX/viewY/viewW/viewH.
    void PopulatePointerCameraState(Editor::SceneTools::ScenePointerEvent& ev) const;

    Editor::SceneTools::SceneToolContext& GetToolContext() { return m_ToolContext; }
    const Editor::SceneTools::SceneToolContext& GetToolContext() const { return m_ToolContext; }

    // Camera Bookmarks Extension
    std::uint64_t RequestBookmarkPreview(const SceneViewCameraPose& pose);
    void CancelBookmarkPreview();
    // The frozen preview the popup binds (device texture, written by the
    // snapshot copy on the serial-close frame). Invalid until the first
    // preview of a session completes, and invalid once `device` has been
    // rebuilt since the copy was written — the handle would otherwise be a
    // freed id that IsValid() still accepts.
    Rendering::TextureHandle GetPreviewSnapshotTexture(const Rendering::IDevice& device) const
    {
        return m_PreviewSnapshot.Texture(device);
    }
    // Space of the pixels the snapshot currently holds, stamped by the copy
    // that wrote them (#767): a frozen capture keeps the space it was rendered
    // in no matter what the display does afterwards. Meaningful only while
    // GetPreviewSnapshotTexture() is valid.
    UI::UITextureSpace GetPreviewSnapshotSpace() const { return m_PreviewSnapshot.Space(); }
    // Readback of the preview pipeline's output (bookmark screenshots).
    // Valid only on frames where the preview view declared (post-spine).
    std::shared_ptr<Rendering::RGReadbackTicket> RequestPreviewReadbackRG(
        Rendering::RenderGraph::RGFrame& frame);

    Rendering::ViewId GetPreviewViewId() const { return m_PreviewViewId; }
    bool HasPreviewRenderForSerial(std::uint64_t serial) const { return m_PreviewLastSubmittedSerial >= serial; }

    // Tool management: switch between selection and transform tools. This is the
    // primary hook that Scene View UI (or hotkeys) will use until a dedicated
    // Scene Tools UI layer is implemented.
    void SetActiveTool(ToolKind kind);
    ToolKind GetActiveToolKind() const { return m_ActiveToolKind; }
    // Activates the tool the tool strip entry `id` registered, built on first use and
    // owned by this view. False, changing nothing, when no entry has the id or the entry
    // is not available (RegisteredToolRefusal says why).
    bool SetActiveRegisteredTool(std::string_view id);
    // The active registered tool's entry id; empty unless ToolKind::Registered is active.
    std::string_view GetActiveRegisteredToolId() const;
    // The tool the tool strip entry `id` registered, built on first use and owned by this
    // view; null when no entry has the id.
    Editor::SceneTools::ISceneTool* GetRegisteredTool(std::string_view id);
    Editor::UndoRedoService* GetUndoRedoService() const { return m_UndoRedo; }
    Editor::EditorChangeNotifications* GetChangeNotifications() const { return m_ChangeNotifications; }

    Editor::SceneTools::TransformTool* GetTransformTool() { return m_TransformTool.get(); }
    const Editor::SceneTools::TransformTool* GetTransformTool() const { return m_TransformTool.get(); }

    Editor::SceneTools::NavGridBrushTool* GetNavGridBrushTool() { return m_NavGridBrushTool.get(); }
    const Editor::SceneTools::NavGridBrushTool* GetNavGridBrushTool() const { return m_NavGridBrushTool.get(); }

    // Optional editor change notifications service used to keep gizmo pivots
    // and other editor-only state in sync when components change (inspector edits,
    // undo/redo, gizmo preview).
    void SetUndoRedoService(Editor::UndoRedoService* undoRedo) { m_UndoRedo = undoRedo; }
    void SetChangeNotifications(Editor::EditorChangeNotifications* notifications);

    // The construction-time world binding remains fixed for this view's lifetime.
    [[nodiscard]] ECS::World& GetWorld() const { return *m_World; }

    // Selection wiring: Scene tools notify via OnEntityPicked, and the editor
    // registers callbacks to propagate selection to Inspector, etc.
    // Modifier mapping (interpreted inside OnEntityPicked):
    //   No modifier:   Replace selection.
    //   Shift or Ctrl: Toggle (add if absent, remove if present).
    //   Shift+Ctrl:    Add only.
    using OnSelectEntityCallback = std::function<void(ECS::EntityHandle)>;
    using OnSelectEntitiesCallback = std::function<void(const std::vector<ECS::EntityHandle>&)>;
    using OnActiveToolChangedCallback = std::function<void(ToolKind)>;
    void SetOnSelectEntityCallback(OnSelectEntityCallback callback);
    void SetOnSelectEntitiesCallback(OnSelectEntitiesCallback callback);
    void SetOnActiveToolChangedCallback(OnActiveToolChangedCallback callback);
    // skipPickRootResolve: the caller already resolved the pick target (e.g.
    // click-through / depth cycle candidates); don't re-apply root resolution.
    void OnEntityPicked(ECS::EntityHandle entity,
                        bool shift = false,
                        bool ctrl = false,
                        bool skipPickRootResolve = false);

    // Scene-view click selection with Unity-style click-through: first click
    // selects the model-instance root (exact entity in exact-pick mode), a
    // click with the instance already selected drills to the exact entity
    // under the cursor, and further clicks at the same spot cycle through
    // deeper hits, wrapping. Light/probe icon hits override mesh hits and
    // bypass click-through. Applies the selection modifiers carried by the
    // event. Shared by SelectionTool and TransformTool so the behavior is
    // identical in every tool mode. Returns the entity that ended up picked
    // (invalid on empty space). `clearOnMiss=false` leaves the selection
    // untouched on empty space — TransformTool's pointer-down uses this so
    // arming a marquee drag doesn't deselect at press.
    //
    // While the scene TLAS is refitting on the job system the click is
    // deferred, not waited for: it returns Deferred, and the pick applies, as a
    // plain click, on the frame the refit finishes (ResolveDeferredPicks, from
    // Update).
    Editor::SceneTools::ScenePickOutcome PickViaClickThrough(const Editor::SceneTools::ScenePointerEvent& event,
                                                             bool clearOnMiss = true);

    using IsEntityPickablePredicate = std::function<bool(ECS::EntityHandle)>;
    void SetIsEntityPickable(IsEntityPickablePredicate pred) { m_IsEntityPickable = std::move(pred); }
    bool IsEntityPickable(ECS::EntityHandle entity) const;

    /// Clear the scene selection without firing OnSelect callbacks (cross-panel sync).
    void SilentlyClearSelection();
    void SilentlySetSelection(const std::vector<ECS::EntityHandle>& entities);
    void RebuildSelectionDescendants();
    bool HasMeasureEndpointAtPointer(const Editor::SceneTools::ScenePointerEvent& event);

    // Marquee drag-select result: replace the current selection with this
    // entity list (or, when additive is true, toggle entries into the existing
    // set). Forwarded to the selection gizmo and the editor's selection
    // callbacks the same way OnEntityPicked would.
    void OnEntitiesMarqueeSelected(const std::vector<ECS::EntityHandle>& entities,
                                   bool additive);

    const std::vector<ECS::EntityHandle>& GetSelectedEntities() const { return m_SelectedEntities; }

    // Hover wiring: Hierarchy panel and Scene View pointer can set hovered entity.
    // allowDisabled=true lets hidden/disabled entities (and descendants of
    // disabled ancestors) outline — used by the "reveal hidden" hover modifier.
    // source=HierarchyPanel keeps the outline preview but suppresses the Scene View
    // hover name pill until the pointer drives hover again.
    void SetHoverEntity(ECS::EntityHandle entity,
                        bool allowDisabled = false,
                        HoverEntitySource source = HoverEntitySource::SceneViewPointer);
    void HandleHoverDetection(const Editor::SceneTools::ScenePointerEvent& event);

    ECS::EntityHandle GetHoveredEntity() const { return m_HoveredEntity; }
    bool IsHoverNamePillSuppressedBySource() const { return m_SuppressHoverNamePill; }

    // Depth position of the current pick within the candidates under the
    // cursor (1-based), shown as "2/7" on the hover name pill while the user
    // is cycling (Ctrl+scroll or same-spot clicks). Valid only while the
    // hover pill mirrors the cycled selection; false otherwise.
    bool GetPickCycleStatus(int& outIndex, int& outCount) const
    {
        if (m_PickCycleIndex < 0 || m_PickCycleCount <= 1)
            return false;
        if (m_SelectedEntities.empty() || m_HoveredEntity != m_SelectedEntities.back())
            return false;
        outIndex = m_PickCycleIndex + 1;
        outCount = m_PickCycleCount;
        return true;
    }
    const std::vector<ECS::EntityHandle>& GetHoveredDescendants() const { return m_HoveredDescendants; }

    // Cycle through pick candidates under the cursor (Ctrl/Cmd+scroll, all view
    // modes). direction > 0 cycles deeper, direction < 0 shallower; when the
    // current selection is not under the cursor the cycle starts at the
    // nearest candidate regardless of direction.
    void CycleEntityUnderCursor(float viewX, float viewY, float viewW, float viewH, int direction);

    // Camera position history (navigated via `[` / `]` keys).
    // A snapshot is auto-pushed when the pose stays stable for a short interval;
    // Step* tweens to the neighbouring entry and suppresses the next auto-push.
    bool StepCameraHistoryBack();
    bool StepCameraHistoryForward();

  private:
    // Clicks and hovers that arrived while the scene TLAS was refitting on the
    // job system. They apply on the frame the refit finishes, so the main
    // thread never waits for it; a newer hover replaces an older one.
    // Applied as a plain click (clearing the selection on a miss): no marquee
    // follows a deferred press.
    std::optional<Editor::SceneTools::ScenePointerEvent> m_DeferredClick;
    std::optional<Editor::SceneTools::ScenePointerEvent> m_DeferredHover;
    // Set while ResolveDeferredPicks replays them: readiness then means "the
    // work they waited for is done", not "nothing moved since".
    bool m_ResolvingDeferredPicks = false;
    void ResolveDeferredPicks();
    // Who is asking the scene TLAS: a click always brings it current; a hover
    // starts new work at most once per kHoverTlasRefresh and otherwise answers
    // from the last finished tree, so hovering over a moving scene does not
    // snapshot and refit every frame.
    enum class PickPurpose
    {
        Click,
        Hover,
    };
    static constexpr std::chrono::milliseconds kHoverTlasRefresh{100};
    std::chrono::steady_clock::time_point m_LastHoverTlasRequest{};
    // True when the scene TLAS can answer a pick now; otherwise the work to
    // bring it current is running and the caller defers.
    bool SceneTlasReadyForPick(ECS::World& world, PickPurpose purpose);

    // Makes the first registered tool whose entry claims the new selection the active one
    // (a spline selected activates the spline tool).
    void ActivateRegisteredToolForSelection(ECS::EntityHandle primary);
    // Ray-test the camera-facing light/probe icon footprints; nearest hit per
    // type, probe icons take precedence over light icons (their draw order).
    // Icons render on top of scene geometry, so PickViaClickThrough lets an
    // icon hit override closer mesh hits. revealHidden relaxes the disabled/
    // locked filters (the hover "reveal hidden" modifier).
    ECS::EntityHandle PickIconAtRay(const Editor::SceneTools::ScenePointerEvent& event,
                                    bool revealHidden = false);

    // Raycast the scene (meshes only), drop unpickable hits, and build the
    // ordered click-through candidate list for the active pick mode, into
    // m_PickHits / m_PickCandidates. Cached: while the ray, pick mode, and
    // world (structural + write versions) are unchanged — same-spot
    // click-through and Ctrl+scroll bursts — the previous lists are reused,
    // skipping the raycast and its TLAS sync entirely.
    void UpdatePickCandidates(ECS::World& world, const Mathematics::Ray3D& ray);

    struct CameraTween
    {
        SceneViewCameraPose From;
        SceneViewCameraPose To;
        float Duration = 0.7f;
        float Elapsed = 0.0f;
        bool Active = false;
    };

    CameraTween m_CameraTween;
    void TweenUpdate(float deltaSeconds, bool cameraMoved);

    struct CameraHistory
    {
        std::vector<SceneViewCameraPose> Entries;
        int Cursor = -1;
        SceneViewCameraPose LastStable{};
        bool HasLastStable = false;
        float StableSeconds = 0.0f;
        bool SuppressAutoPush = false;
    };
    CameraHistory m_CameraHistory;
    void UpdateCameraHistoryTracking(float deltaSeconds);
    void PushCameraHistorySnapshot(const SceneViewCameraPose& pose);
    void StartHistoryTweenTo(int targetIndex);

    // Push the current m_Is2DMode state into the TransformTool so the gizmo
    // center hit region is enabled/disabled to match the scene view mode.
    void Sync2DModeToTransformTool();

    // When 2D mode and pixel-perfect are both active, force the orthographic
    // visible height to match an integer world-unit-per-pixel ratio derived
    // from the current viewport pixel height and the configured pixel scale.
    void ApplyPixelPerfectIfActive();

    CameraBookmarksWidget* m_BookmarksWidget = nullptr;

    Engine::Renderer::RenderServices* m_RenderServices = nullptr; // non-owning

    // Lazily fetched on the first metered frame (non-owning; lives on
    // RenderServices) so the feature is never created for views that never
    // use auto exposure.
    Engine::Renderer::ExposureReadbackFeature* m_ExposureReadback = nullptr;

    // Logical camera & view registered with RenderServices (shared with ECS rendering).
    Rendering::CameraId m_CameraId{0};
    Rendering::ViewId m_ViewId{0};
    bool m_WaitingForExtractionRG = false;
    Editor::ViewPresentationSnapshot m_PresentationSnapshot;

    // Preview camera for CameraBookmarks
    Rendering::ViewId m_PreviewViewId = 0;
    Rendering::CameraId m_PreviewCameraId = 0;

    // Controller-owned frozen-preview device texture (8c-3): written by the
    // snapshot copy pass on the serial-close frame, sampled by the popup via
    // a stable device-handle bind. Never pooled (pool persistents age out).
    Editor::ViewPresentationSnapshot m_PreviewSnapshot;
    bool m_PreviewSnapshotPending = false; // serial closed this frame ⇒ copy due

    bool m_RequestPreview = false;
    // One-shot: old-arm Record published registry targets for the MAIN view;
    // the next RenderGraph frame must ClearViewTargets or the old retained world
    // pass keeps rendering this view on top of the spine (the GameView
    // handoff, mirrored — see DeclareTargetsRG).
    bool m_OldArmTargetsPublished = false;
    // RenderGraph-frame consumer of the deferred preview freeze (the confirm runs in
    // old-arm Record; the teardown must also run when the window is back on
    // RenderGraph). Returns true when the freeze ran this call.
    bool FreezePreviewViewIfPending(Engine::Renderer::RenderServices* rs);
    bool m_PreviewPendingFreeze = false; // deferred teardown: set when hold expires, applied next Record()
    std::uint64_t m_PreviewRequestSerial = 0;
    std::uint64_t m_PreviewPendingSerial = 0;
    std::uint64_t m_PreviewLastSubmittedSerial = 0;
    bool m_PreviewSubmittedThisFrame = false;
    bool m_PreviewHadDrawItemsThisFrame = false;
    int m_PreviewNoDrawRetryFrames = 0;
    bool m_PreviewTargetsBound = false;
    SceneViewCameraPose m_PreviewPose;

    // Authoritative camera data for the current frame's Scene View rendering.
    // Recorded during Record() and reused by gizmo passes to avoid drift.
    Rendering::CameraData m_FrameCameraData{};
    // Viewport m_FrameCameraData's projection was built from — set in the same
    // assignment, never sampled separately.
    std::uint32_t m_FrameCameraViewportW = 0;
    std::uint32_t m_FrameCameraViewportH = 0;
    bool m_HasFrameCameraData = false;

    void ResetFlyVelocity()
    {
        m_CamVelocity[0] = 0.0f;
        m_CamVelocity[1] = 0.0f;
        m_CamVelocity[2] = 0.0f;
    }

    // Editor camera state (simple FPS-style: free position with yaw/pitch orientation).
    // Defaults align with seeded Main Camera: (0,3,-10), identity rot → world +Z forward.
    // Position and orbit pivot are DOUBLE: at planetary coordinates
    // (|pos| ~5e4 today, 6.4e6 on the roadmap) fp32 quantizes to ~4 mm / ~0.5 m
    // and pan/fly deltas below the ULP round to zero. All rig integration runs
    // in double (SceneViewCameraRig.h); fp32 is produced once at each hand-off
    // (CameraData, pose, rays). See camera-planet-scale-controller-investigation.
    float m_CamYawDeg = 90.0f;
    float m_CamPitchDeg = 0.0f;
    double m_CamPos[3] = {0.0, 3.0, -10.0};
    float m_MoveSpeed = 5.0f;
    // Smoothed fly-camera velocity (world units/s), eased toward the input
    // direction by the Scene View move-acceleration setting (0 = instant).
    float m_CamVelocity[3] = {0.0f, 0.0f, 0.0f};
    float m_CamDistance = 6.0f; // kept for now for potential zoom control

    bool m_IsOrbiting = false;
    double m_OrbitPivot[3] = {0.0, 0.0, 0.0};
    bool m_ShowGizmos = true;
    bool m_ShowTransformGizmos = true;
    bool m_ShowLightGizmos = true;
    bool m_ShowMarkupGizmos = true;
    bool m_ShowCameraFrameGuide = false;
    bool m_ShowGrid = false;
    bool m_PostProcessingEnabled = true;
    bool m_GridSnapEnabled = false;
    bool m_FrameGraphBuildEnabled = true;
    uint32_t m_RenderSampleCountOverride = 0;
    float m_GridSnapSize = 0.5f;
    float m_GridOpacity = 0.3f;
    bool m_Is2DMode = false;
    bool m_Orthographic = false;
    FixedViewOrientation m_FixedViewOrientation = FixedViewOrientation::Free;
    std::string m_RenderNamePrefix = "SceneView";
    SceneViewCameraPose m_Saved3DPose{}; // restored when leaving 2D mode

    Editor::SceneTools::SceneToolContext m_ToolContext;
    Editor::SceneTools::SelectionGizmo m_SelectionGizmo;
    Editor::SceneTools::HoverGizmo m_HoverGizmo;
    Editor::SceneTools::NavDebugGizmo m_NavDebugGizmo;
    Editor::SceneTools::ComponentGizmos m_ComponentGizmos;
    Editor::SceneTools::ReflectionProbeGizmo m_ReflectionProbeGizmo;
    Editor::SceneTools::DDGIVolumeGizmo m_DDGIVolumeGizmo;
    Editor::SceneTools::TerrainModifierGizmo   m_TerrainModifierGizmo;
    Editor::SceneTools::LightGizmo             m_LightGizmo;
    Editor::SceneTools::CameraFrameGizmo       m_CameraFrameGizmo;
    Editor::SceneTools::MeasureSceneGizmo      m_MeasureSceneGizmo;
    Editor::MarkupGizmo                        m_MarkupGizmo;
    Editor::SceneTools::SplineSceneGizmo m_SplineSceneGizmo;
    std::unique_ptr<Editor::SceneTools::SelectionTool> m_SelectionTool;
    std::unique_ptr<Editor::SceneTools::TransformTool> m_TransformTool;
    std::unique_ptr<Editor::SceneTools::NavGridBrushTool> m_NavGridBrushTool;
    // Tools registered with the tool strip, by entry id, built on first activation.
    std::unordered_map<std::string, std::unique_ptr<Editor::SceneTools::ISceneTool>> m_RegisteredTools;
    std::string m_ActiveRegisteredToolId;

    // Game UI overlay: previews the world's UIDocument HUDs over the editing
    // viewport. Lazy-created on first overlay declaration; visibility is the
    // SceneViewSettings "Show Game UI" toggle (default on).
    std::unique_ptr<GameUIHost> m_GameUI;
    // The project's authored UI scale policy, so the preview scales the HUD the
    // way the Game View and a build do.
    Editor::ProjectUIScaleBinding m_GameUiScale;

    ECS::EntityHandle m_HoveredEntity{};
    // Raw-hit → resolved-root cache for scene hover in root-pick mode, so the
    // hierarchy walk doesn't rerun on every small mouse move over one entity.
    ECS::EntityHandle m_LastRawSceneHover{};
    ECS::EntityHandle m_LastResolvedSceneHover{};
    // Click-through state: set by the depth cycle so the next click confirms
    // the cycled pick; repeated clicks near the same spot cycle candidates.
    bool m_DepthCycleAnchor = false;
    float m_LastClickViewX = -1.0e6f;
    float m_LastClickViewY = -1.0e6f;
    // Depth-cycle pill status (see GetPickCycleStatus). -1/0 = no cycle.
    int m_PickCycleIndex = -1;
    int m_PickCycleCount = 0;

    // Pick cache (see UpdatePickCandidates). The vectors double as reusable
    // scratch, so steady-state picking does not heap-allocate.
    Mathematics::Ray3D m_PickCacheRay{};
    std::uint64_t m_PickCacheWriteVersion = 0;
    std::size_t m_PickCacheStructuralVersion = 0;
    bool m_PickCacheExactMode = false;
    bool m_PickCacheValid = false;
    std::vector<Editor::Picking::PickHit> m_PickHits;
    std::vector<ECS::EntityHandle> m_PickCandidates;
    bool m_SuppressHoverNamePill = false;
    std::vector<ECS::EntityHandle> m_HoveredDescendants;
    std::vector<ECS::EntityHandle> m_SelectionDescendants;
    // Reusable scratch for accumulating each selected root's subtree: DescendantsOf
    // clears its out-param per call, so unioning across roots needs a temp.
    std::vector<ECS::EntityHandle> m_DescendantScratch;
    float m_LastHoverX = -1.0f;
    float m_LastHoverY = -1.0f;
    Mathematics::Ray3D m_LastHoverRay{};
    bool  m_LastHoverRayValid = false;

    ToolKind m_ActiveToolKind = ToolKind::Selection;

    OnSelectEntityCallback m_OnSelectEntityCallback;
    OnSelectEntitiesCallback m_OnSelectEntitiesCallback;
    OnActiveToolChangedCallback m_OnActiveToolChangedCallback;
    IsEntityPickablePredicate m_IsEntityPickable;
    std::vector<ECS::EntityHandle> m_SelectedEntities;

    uint32_t m_LastW = 0;
    uint32_t m_LastH = 0;

    SceneViewGridRenderer m_GridRenderer;

    // Persistent descriptor-set cache for the skinned selection-mask variant,
    // keyed by the atlas buffer handle that PerFrameWritePool returns. The pool
    // cycles through a small fixed set of buffers (one per frame-in-flight) so
    // after the first few frames every handle has a cached DS and further frames
    // only do a hash-map lookup instead of allocating + updating a transient
    // set. Created with transient=false so the device owns them for the
    // editor's lifetime.
    std::unordered_map<uint64_t, Rendering::DescriptorSetHandle> m_SelectionMaskSkinnedDSByBuffer;

    // Frame-cache for the selection/hover silhouette mask. The mask is a retained
    // pool-persistent texture; the geometry pass that fills it (and the per-entity
    // draw resolution feeding it) re-runs only when this cheap dirty key
    // (dimensions + camera VP + each contributor's id, world transform, and mesh
    // identity) changes since last frame, when the last resolve was animated, or
    // when the pooled physical is reallocated (resize / age-out / device rebuild).
    // Hover-and-hold over a static object ⇒ zero mask re-renders and no resolution.
    uint64_t m_OutlineMaskKey = 0;
    bool m_OutlineMaskValid = false;
    uint64_t m_OutlineMaskPhysical = 0; // last physical the mask was rendered into
    // The last resolved mask had skinned/wind draws ⇒ its silhouette animates
    // even when the cheap key (ids + transforms + camera VP) is stable, so force
    // a per-frame re-resolve+re-render while it holds. Static content (the
    // reported building case) leaves this false and rides the cache.
    bool m_OutlineMaskAnimated = false;
    // The last resolve included plugin/procedural mask parts (e.g. an EZTree with
    // no MeshRenderer). Their content (RuntimeMeshHandleId / branch-leaf geometry)
    // is not folded into the cheap key, so force a per-frame re-resolve while such
    // a contributor is in the set — otherwise an inspector edit to a held-selected
    // tree would leave a persistently stale silhouette.
    bool m_OutlineMaskHasPlugin = false;

    // Fires a one-shot JobSystem task that pre-compiles the SV_Selection_Mask
    // and SV_Selection_Mask_Skinned pipelines on a worker thread. The runtime
    // `GetOrCreatePipelineVariant` calls hash to the same canonical desc so they
    // hit the pre-warmed cache and skip the ~10ms vkCreateGraphicsPipelines
    // stall on first selection.
    void SchedulePipelinePreWarm();
    bool m_PipelinePreWarmScheduled = false;

    struct MeasureEndpointEditState
    {
        bool Active = false;
        ECS::EntityHandle Entity{};
        std::uint8_t Endpoint = 0;
        Mathematics::Vector3 StartPosition{0.0f, 0.0f, 0.0f};
        Mathematics::Vector3 AccumulatedDelta{0.0f, 0.0f, 0.0f};
    };

    // 7e helper: the selection-mask + outline pair of DeclareOverlaysRG
    // (snapshot walk + both pass declarations; SceneViewOverlaysRG.cpp). The
    // silhouette mask is a retained (pool-persistent) texture re-rendered only
    // when its dirty key changes (selection/hover set, their world transforms,
    // or the camera VP); otherwise the Sobel pass samples the cached mask and no
    // mesh geometry is re-drawn. The mask is on-top (no depth test) with culling
    // off, matching the pre-cache outline; `samples` sizes the MSAA render target
    // it resolves into the single-sample cache.
    void DeclareSelectionOutlineRG(Rendering::RenderGraph::RGFrame& frame,
                                    Rendering::RenderGraph::RGTexture finalColor,
                                    Engine::Renderer::RenderServices* rs,
                                    uint32_t w, uint32_t h, uint32_t samples,
                                    bool includeSelectedEntities);

    void ClearMeasureEndpointTransformTarget();
    void SyncMeasureEndpointTransformTarget();
    bool GetSelectedMeasureEndpointWorldPosition(Mathematics::Vector3& outWorldPosition) const;
    void BeginMeasureEndpointTransformEdit(const char* editName);
    void ApplyMeasureEndpointTransformDelta(const Mathematics::Vector3& deltaWorld);
    void CommitMeasureEndpointTransformEdit();
    void NotifyMeasureEndpointChanged(ECS::EntityHandle entity,
                                      Editor::EditorChangeNotifications::ChangeKind kind);
    void SyncMeasureFromEndpointEntityTransform(ECS::EntityHandle endpointEntity,
                                                Editor::EditorChangeNotifications::ChangeKind kind);

    MeasureEndpointEditState m_MeasureEndpointEdit{};
    Editor::UndoRedoService::InteractiveEdit m_MeasureEndpointUndoEdit{};
    bool m_MeasureEndpointTargetSynced = false;

    // Non-owning; the caller keeps the bound world alive for the view lifetime.
    ECS::World* const m_World;

    // Editor change notification subscription (not owned).
    Editor::UndoRedoService* m_UndoRedo = nullptr;
    Editor::EditorChangeNotifications* m_ChangeNotifications = nullptr;
    Editor::EditorChangeNotifications::SubscriptionToken m_ChangeSub{};
};

} // namespace GameEngine
