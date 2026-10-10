#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "Components/Transform.h"
#include "ECS/Entity.h"
#include "EditorChangeNotifications.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "SceneView/SelectionTool.h"
#include "UndoRedo/UndoRedoService.h"
#include "SceneViewTools.h"

namespace GameEngine::ECS { class World; }

namespace GameEngine
{
namespace Editor
{
namespace SceneTools
{

// What a scene pick did: the entity that ended up selected (invalid on empty
// space), or that it was deferred. A pick is deferred while the scene TLAS
// refits on the job system; the owner applies it, as a plain click, on the
// frame the refit finishes, so the press must not also be read as a miss.
struct ScenePickOutcome
{
    GameEngine::ECS::EntityHandle Entity{};
    bool Deferred = false;
};

// Delegate that performs a scene pick for the TransformTool (the owner runs
// the shared raycast + icon test + click-through resolution).
// `clearOnMiss=false` leaves the selection untouched on empty space (used at
// pointer-down so arming a marquee drag doesn't deselect at press).
using ScenePickDelegate = std::function<ScenePickOutcome(const ScenePointerEvent&, bool clearOnMiss)>;

// Callback invoked when the TransformTool commits a marquee drag-select.
// The bool indicates whether the selection should be added to (true) or
// replace (false) the existing selection.
using OnEntitiesMarqueeCallback =
    std::function<void(const std::vector<GameEngine::ECS::EntityHandle>&, bool additive)>;

enum class TransformMode : std::uint8_t
{
    Select = 0,
    Translate,
    Rotate,
    Scale
};

// Space used for transform axes (world-aligned vs local entity axes).
enum class TransformAxisSpace : std::uint8_t
{
    World = 0,
    Local
};

class TransformTool;

// World-space X/Y/Z gizmo axis directions, indexed by axis.
using GizmoAxisDirections = std::array<Mathematics::Vector3, 3>;

// Simple translate gizmo for TransformTool. Responsible for rendering the
// axis handles around a pivot and handling basic hit-testing / dragging along
// a single axis.
class TransformTranslateGizmo : public IGizmo
{
  public:
    TransformTranslateGizmo() = default;

    void SetPivot(const Mathematics::Vector3& position);
    void SetAxisLength(float length);
    const Mathematics::Vector3& GetPivot() const { return m_Pivot; }

    void SetOwner(TransformTool* owner) { m_Owner = owner; }

    // Draw-only entry point. Render must not read/write ECS or mutate gizmo
    // state; the owning TransformTool is responsible for updating the pivot
    // and drag state during the update phase.
    void Render(GizmoRenderContext& context) override;
    GizmoHitResult HitTest(const GizmoRay& ray) override;
    bool HandlePointerEvent(const ScenePointerEvent& event,
                            const GizmoHit& hit) override;
    // Update hover state used purely for visual highlighting. Hover does not
    // start a drag; actual drags are initiated via HandlePointerEvent on
    // pointer-down as usual.
    void SetHover(const GizmoHit& hit);

  private:
    TransformTool* m_Owner = nullptr; // non-owning
    Mathematics::Vector3 m_Pivot{0.0f, 0.0f, 0.0f};
    float m_AxisLength = 1.0f;

    // Drag state (shared for axis & plane drags).
    bool m_IsDragging = false;
    GizmoHitKind m_DragKind = GizmoHitKind::None;

    // Axis drag state.
    std::uint32_t m_ActiveAxis = 0; // 0 = X, 1 = Y, 2 = Z
    Mathematics::Vector3 m_DragOrigin{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_DragAxis{1.0f, 0.0f, 0.0f};
    float m_DragStartAxisParam = 0.0f;
    float m_DragCurrentAxisParam = 0.0f;

    // Plane drag state.
    std::uint32_t m_ActivePlane = 0; // 0 = XY, 1 = YZ, 2 = ZX
    Mathematics::Vector3 m_PlaneOrigin{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_PlaneNormal{0.0f, 1.0f, 0.0f};
    Mathematics::Vector3 m_LastPlanePoint{0.0f, 0.0f, 0.0f};
    // Hover state for visual feedback.
    GizmoHitKind m_HoverKind = GizmoHitKind::None;
    std::uint32_t m_HoverAxis = 0;  // 0 = X, 1 = Y, 2 = Z
    std::uint32_t m_HoverPlane = 0; // 0 = XY, 1 = YZ, 2 = ZX
};

// Rotation gizmo for TransformTool. Renders three axis-aligned rotation
// rings (X, Y, Z) around the current pivot and converts drags on a ring into
// incremental rotation deltas applied via TransformTool.
class TransformRotateGizmo : public IGizmo
{
  public:
    TransformRotateGizmo() = default;

    void SetPivot(const Mathematics::Vector3& position);
    void SetRadius(float radius);
    void SetOwner(TransformTool* owner) { m_Owner = owner; }

    void Render(GizmoRenderContext& context) override;
    GizmoHitResult HitTest(const GizmoRay& ray) override;
    bool HandlePointerEvent(const ScenePointerEvent& event,
                            const GizmoHit& hit) override;
    void SetHover(const GizmoHit& hit);

  private:
    // Handle id for the screen-space ("view-plane") rotation ring. Rotates the
    // target around the camera forward axis. Axis rings use ids 0/1/2.
    static constexpr std::uint32_t kViewPlaneHandle = 3u;

    TransformTool* m_Owner = nullptr; // non-owning
    Mathematics::Vector3 m_Pivot{0.0f, 0.0f, 0.0f};
    float m_Radius = 1.5f;

    bool m_IsDragging = false;
    GizmoHitKind m_DragKind = GizmoHitKind::None;
    std::uint32_t m_ActiveAxis = 0; // 0 = X, 1 = Y, 2 = Z, 3 = view-plane
    // Hover state for visual feedback.
    GizmoHitKind m_HoverKind = GizmoHitKind::None;
    std::uint32_t m_HoverAxis = 0; // 0 = X, 1 = Y, 2 = Z, 3 = view-plane

    // Cached plane basis for the active ring during a drag.
    Mathematics::Vector3 m_PlaneNormal{0.0f, 1.0f, 0.0f};
    Mathematics::Vector3 m_Tangent{1.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_Bitangent{0.0f, 0.0f, 1.0f};

    // View-plane drag: when active, rotation is applied around an arbitrary
    // world axis (the frozen camera forward) rather than an X/Y/Z index.
    bool m_DragIsViewPlane = false;
    Mathematics::Vector3 m_DragWorldAxis{0.0f, 0.0f, 1.0f};

    // Edge-on drag fallback: when the picked ring is viewed nearly edge-on the
    // angle is derived from screen-space displacement (linear projection)
    // instead of atan2 around the pivot, which is hypersensitive near center.
    bool m_EdgeOnDrag = false;
    float m_DragRadius = 1.0f;

    float m_StartAngle = 0.0f;
    float m_CurrentAngle = 0.0f;
    float m_TotalAngleDelta = 0.0f;
    // Sum of radians passed to ApplyRotationDelta during the active drag (for snap with S).
    float m_CumulativeAppliedRotation = 0.0f;
};

// Scale gizmo for TransformTool. Renders three axis lines with cubes at the
// ends for per-axis scaling and a central cube for uniform scale.
class TransformScaleGizmo : public IGizmo
{
  public:
    TransformScaleGizmo() = default;

    void SetPivot(const Mathematics::Vector3& position);
    void SetAxisLength(float length);
    void SetOwner(TransformTool* owner) { m_Owner = owner; }

    void Render(GizmoRenderContext& context) override;
    GizmoHitResult HitTest(const GizmoRay& ray) override;
    bool HandlePointerEvent(const ScenePointerEvent& event,
                            const GizmoHit& hit) override;
    void SetHover(const GizmoHit& hit);

  private:
    TransformTool* m_Owner = nullptr; // non-owning
    Mathematics::Vector3 m_Pivot{0.0f, 0.0f, 0.0f};
    float m_AxisLength = 1.0f;

    bool m_IsDragging = false;
    GizmoHitKind m_DragKind = GizmoHitKind::None;
    std::uint32_t m_ActiveHandle = 0; // 0=X, 1=Y, 2=Z, 3=uniform center

    // Axis drag state (per-axis scaling).
    Mathematics::Vector3 m_DragOrigin{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_DragAxis{1.0f, 0.0f, 0.0f};
    float m_StartAxisParam = 0.0f;
    float m_LastAxisParam = 0.0f;   // legacy, kept for potential debugging
    float m_TotalAxisFactor = 0.0f; // cumulative scale factor since drag start

    // Screen-space state for uniform scale dragging.
    float m_StartViewX = 0.0f;
    float m_StartViewY = 0.0f;
    float m_CurrentViewDelta = 0.0f;
    // Hover state for visual feedback.
    GizmoHitKind m_HoverKind = GizmoHitKind::None;
    std::uint32_t m_HoverHandle = 0; // 0=X, 1=Y, 2=Z, 3=uniform
};

// Top-level Transform tool. For now this focuses on exposing the transform
// gizmos (starting with translate). Selection integration and actual transform
// application will be added incrementally on top of this skeleton.
class TransformTool : public ISceneTool
{
  public:
    explicit TransformTool(GameEngine::ECS::World& world);

    const char* GetName() const override { return "TransformTool"; }

    void OnActivated() override;
    void OnDeactivated() override;

    void OnPointerEvent(const ScenePointerEvent& event) override;
    void OnKeyEvent(const SceneKeyEvent& event) override;

    void GatherGizmos(GizmoCollector& collector) override;

    void SetMode(TransformMode mode);
    TransformMode GetMode() const { return m_Mode; }

    // Callback invoked after the transform mode changes (from keyboard or API).
    using OnModeChangedCallback = std::function<void(TransformMode)>;
    void SetOnModeChangedCallback(OnModeChangedCallback callback)
    {
        m_OnModeChanged = std::move(callback);
    }

    // Callback invoked after axis space changes (from keyboard or API).
    using OnAxisSpaceChangedCallback = std::function<void(TransformAxisSpace)>;
    void SetOnAxisSpaceChangedCallback(OnAxisSpaceChangedCallback callback)
    {
        m_OnAxisSpaceChanged = std::move(callback);
    }

    // Configure whether transform axes operate in world or local space.
    void SetAxisSpace(TransformAxisSpace space);
    TransformAxisSpace GetAxisSpace() const { return m_AxisSpace; }

    // Allow callers to explicitly set the pivot position for the transform
    // gizmos. SceneViewController wires this to the shared selection model so
    // the tool automatically tracks the active selection.
    void SetPivot(const Mathematics::Vector3& position);
    void ClearPivot();

    void SetTargetEntity(GameEngine::ECS::EntityHandle entity);
    GameEngine::ECS::EntityHandle GetTargetEntity() const { return m_TargetEntity; }
    void SetSelectedEntities(const std::vector<GameEngine::ECS::EntityHandle>& entities);

    struct ExternalPointTarget
    {
        std::function<bool(Mathematics::Vector3& outWorldPosition)> GetWorldPosition;
        std::function<void(const char* editName)> BeginEdit;
        std::function<void(const Mathematics::Vector3& deltaWorld)> ApplyTranslationDelta;
        std::function<void()> CommitEdit;
    };
    void SetExternalPointTarget(ExternalPointTarget target);
    void ClearExternalPointTarget();
    bool HasTransformTarget() const;

    // Recompute and update the gizmo pivot from the current target entity's
    // world-space transform (derived from local Transform + parent hierarchy).
    // Useful after external changes (inspector edits, undo/redo).
    void RefreshPivotFromTargetEntity();

    // Apply an incremental world-space translation to the active target
    // entity while keeping the gizmo pivot in sync.
    //
    // This is primarily used internally by the gizmo implementation, but is
    // kept public so Editor tests can drive the core transform logic without
    // going through input/picking.
    void ApplyTranslationDelta(const Mathematics::Vector3& deltaWorld);
    // Apply an incremental rotation around one of the transform axes.
    // The axisIndex parameter corresponds to X/Y/Z = 0/1/2. The actual
    // axis orientation is determined by the current TransformAxisSpace
    // (world vs local), but callers do not need to know which space is
    // active.
    void ApplyRotationDelta(std::uint32_t axisIndex, float deltaRadians);

    // Apply an incremental rotation around an explicit world-space axis. Used
    // by the rotation gizmo's screen-space ("view-plane") ring, which rotates
    // around the camera forward axis rather than a canonical X/Y/Z axis.
    void ApplyRotationDeltaWorldAxis(const Mathematics::Vector3& worldAxis, float deltaRadians);

    // Apply an incremental scale delta, specified as multiplicative scale
    // factors per axis in local space. Values are multiplied into the
    // current local Scale component.
    void ApplyScaleDelta(const Mathematics::Vector3& deltaScale);

    // Grid snapping: when enabled, translation snaps positions to the given grid size.
    void SetGridSnap(bool enabled, float size) { m_SnapEnabled = enabled; m_SnapSize = size; }
    bool IsSnapEnabled() const { return m_SnapEnabled; }
    float GetSnapSize() const { return m_SnapSize; }

    // 2D mode: when enabled, the translate gizmo exposes an origin-centered
    // "center" hit region that initiates an XY plane drag, making it easy to
    // grab the gizmo anywhere near its center rather than on a specific axis
    // or plane handle.
    void SetIs2DMode(bool enabled) { m_Is2DMode = enabled; }
    bool IsIn2DMode() const { return m_Is2DMode; }

    // Set the delegate that performs entity picking for clicks routed through
    // this tool (click on object, or click on empty space).
    void SetScenePickDelegate(ScenePickDelegate callback)
    {
        m_ScenePick = std::move(callback);
    }

    // Set callback invoked when TransformTool commits a marquee drag-select.
    void SetOnEntitiesMarqueeCallback(OnEntitiesMarqueeCallback callback)
    {
        m_OnEntitiesMarqueeCallback = std::move(callback);
    }

    // Optional predicate to exclude entities from marquee/lasso picks (e.g. locked).
    using IsEntityPickablePredicate = std::function<bool(GameEngine::ECS::EntityHandle)>;
    void SetIsEntityPickable(IsEntityPickablePredicate pred) { m_IsEntityPickable = std::move(pred); }

    // Optional editor services for undo/redo + live notifications. When unset,
    // TransformTool behaves as before (direct edits, no undo stack).
    void SetUndoRedoService(UndoRedoService* undo) { m_UndoRedo = undo; }
    [[nodiscard]] GameEngine::ECS::World& GetWorld() const;
    void SetChangeNotifications(EditorChangeNotifications* notifications) { m_ChangeNotifications = notifications; }

    bool IsInteractiveEditActive() const { return m_TransformDragActive || (bool)m_ActiveEdit; }

    // Gizmo thickness settings (in pixels) - used when constant screen size is OFF
    void SetTranslateGizmoThickness(float thickness) { m_TranslateThickness = thickness; }
    void SetRotateGizmoThickness(float thickness) { m_RotateThickness = thickness; }
    void SetScaleGizmoThickness(float thickness) { m_ScaleThickness = thickness; }
    float GetTranslateGizmoThickness() const { return m_TranslateThickness; }
    float GetRotateGizmoThickness() const { return m_RotateThickness; }
    float GetScaleGizmoThickness() const { return m_ScaleThickness; }
    
    // Constant screen-space size mode
    void SetConstantScreenSize(bool enabled) { m_ConstantScreenSize = enabled; }
    bool GetConstantScreenSize() const { return m_ConstantScreenSize; }
    
    // Gizmo scale settings (multiplier for constant screen size mode)
    void SetTranslateGizmoScale(float scale) { m_TranslateScale = scale; }
    void SetRotateGizmoScale(float scale) { m_RotateScale = scale; }
    void SetScaleGizmoScale(float scale) { m_ScaleGizmoScale = scale; }
    float GetTranslateGizmoScale() const { return m_TranslateScale; }
    float GetRotateGizmoScale() const { return m_RotateScale; }
    float GetScaleGizmoScale() const { return m_ScaleGizmoScale; }

    // Constant size thickness settings (used when constant screen size is ON)
    void SetTranslateConstantThickness(float thickness) { m_TranslateConstantThickness = thickness; }
    void SetRotateConstantThickness(float thickness) { m_RotateConstantThickness = thickness; }
    void SetScaleConstantThickness(float thickness) { m_ScaleConstantThickness = thickness; }
    float GetTranslateConstantThickness() const { return m_TranslateConstantThickness; }
    float GetRotateConstantThickness() const { return m_RotateConstantThickness; }
    float GetScaleConstantThickness() const { return m_ScaleConstantThickness; }
    
    // Rotate snap increment (degrees) applied when the user holds the snap key
    // while dragging a rotation ring. Defaults to 45°.
    void SetRotateSnapIncrementDegrees(float degrees)
    {
        if (degrees > 0.0f)
            m_RotateSnapIncrementDeg = degrees;
    }
    float GetRotateSnapIncrementDegrees() const { return m_RotateSnapIncrementDeg; }
    float GetRotateSnapIncrementRad() const;

    // Enhanced rotation gizmo behavior. When enabled (default) the rotate gizmo
    // front-face culls its rings, exposes a screen-space ("view-plane") ring,
    // and uses a linear screen-displacement mapping for near-edge-on drags. When
    // disabled it reverts to the legacy full-ring / atan2 behavior.
    void SetRotateGizmoEnhanced(bool enabled) { m_RotateEnhanced = enabled; }
    bool GetRotateGizmoEnhanced() const { return m_RotateEnhanced; }

    // Update gizmo sizes based on camera distance (call each frame when constant size is enabled)
    void UpdateGizmoSizeForCamera(const Mathematics::Vector3& cameraPos, float baseSizeMultiplier = 0.15f);

    // Orthographic view height in world units. Set by SceneViewController each
    // frame when the scene camera is orthographic. Transform gizmos use this
    // instead of camera-to-pivot distance for constant-screen-size scaling in
    // 2D/ortho mode so they stay at a fixed pixel size.
    void  SetOrthoHeight(float heightWorld)
    {
        m_OrthoHeight    = heightWorld;
        m_HasOrthoHeight = heightWorld > 0.0f;
    }
    bool  HasOrthoHeight() const { return m_HasOrthoHeight; }
    float GetOrthoHeight() const { return m_OrthoHeight; }

  private:
    friend class TransformTranslateGizmo;
    friend class TransformRotateGizmo;
    friend class TransformScaleGizmo;

    // Compute world-space directions for the transform axes X/Y/Z based on the
    // current axis space and target entity, one per axis in outAxisDirs. When no
    // valid target is available or axis space is World, the canonical world axes
    // are used.
    bool GetAxisDirections(GizmoAxisDirections& outAxisDirs) const;

    void PerformEntityPick(const ScenePointerEvent& event);
    void BeginDirectEntityDragArm(const ScenePointerEvent& event);
    void UpdateDirectEntityDrag(const ScenePointerEvent& event);
    void FinishDirectEntityDrag();
    void CancelDirectEntityDrag();

    // Marquee drag-select (Select mode only). Armed on pointer-down over empty
    // space, activates after the cursor moves past a small pixel threshold,
    // and on pointer-up either commits a multi-select or falls back to a
    // single click-pick if no drag occurred.
    void BeginMarqueeArm(const ScenePointerEvent& event);
    void UpdateMarquee(const ScenePointerEvent& event);
    void FinishMarquee(const ScenePointerEvent& event);
    void CancelMarquee();

    void CollectMarqueePicks(std::vector<GameEngine::ECS::EntityHandle>& picked) const;

    // Interactive edit lifecycle used by gizmos for undo/redo.
    void BeginTransformEdit(const char* name);
    void CommitTransformEdit();
    bool HasExternalPointTarget() const;
    bool RefreshPivotFromExternalPointTarget();
    std::vector<GameEngine::ECS::EntityHandle> GetTransformEditTargets(GameEngine::ECS::World* world) const;
    void ClearTransformDragBaselines();
    void EnsureTransformDragBaselines(GameEngine::ECS::World* world);

    TransformMode m_Mode = TransformMode::Translate;
    TransformAxisSpace m_AxisSpace = TransformAxisSpace::World;
    TransformTranslateGizmo m_TranslateGizmo;
    TransformRotateGizmo m_RotateGizmo;
    TransformScaleGizmo m_ScaleGizmo;
    bool m_HasPivot = false;
    GameEngine::ECS::EntityHandle m_TargetEntity{};
    std::vector<GameEngine::ECS::EntityHandle> m_SelectedEntities;
    ExternalPointTarget m_ExternalPointTarget{};
    GameEngine::ECS::World* const m_World;
    bool m_HasExternalPointTarget = false;
    // Hover state for the currently active gizmo/handle, used for
    // visual highlighting only.
    IGizmo* m_HoverGizmo = nullptr; // non-owning
    GizmoHit m_HoverHit{};

    // Grid snapping state.
    bool m_SnapEnabled = false;
    float m_SnapSize = 0.5f;

    // 2D editing mode (top-down / side ortho). When true, the translate
    // gizmo's center hit region is enabled.
    bool m_Is2DMode = false;

    // Orthographic view height in world units (0 when perspective).
    float m_OrthoHeight = 0.0f;
    bool  m_HasOrthoHeight = false;

    // Delegate for integrated entity picking.
    ScenePickDelegate           m_ScenePick;
    OnEntitiesMarqueeCallback   m_OnEntitiesMarqueeCallback;
    IsEntityPickablePredicate   m_IsEntityPickable;

    // Marquee drag-select state (used only in TransformMode::Select).
    MarqueeGizmo m_MarqueeGizmo;
    bool  m_MarqueeArmed  = false;       // mouse is down, may become a drag
    bool  m_MarqueeActive = false;       // drag exceeded threshold
    int   m_MarqueeShape  = 0;           // 0 = rectangle, 1 = lasso
    float m_MarqueeDownViewX = 0.0f;
    float m_MarqueeDownViewY = 0.0f;
    Mathematics::Vector3 m_MarqueeStart{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_MarqueeCurrent{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_MarqueePlaneU{1.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_MarqueePlaneV{0.0f, 1.0f, 0.0f};
    Mathematics::Vector3 m_MarqueePlaneNormal{0.0f, 0.0f, 1.0f};
    std::vector<Mathematics::Vector3> m_LassoPoints;
    ScenePointerEvent    m_MarqueeDownEvent{};

    // Screen-space marquee state (in viewport-local pixels). Populated from
    // ScenePointerEvent coords so intersection tests happen in 2D without any
    // dependency on the click-plane projection.
    float m_MarqueeScreenStartX = 0.0f;
    float m_MarqueeScreenStartY = 0.0f;
    float m_MarqueeScreenCurX   = 0.0f;
    float m_MarqueeScreenCurY   = 0.0f;
    std::vector<Mathematics::Vector2> m_LassoScreenPoints;

    // Click-drag transform state. When the pointer goes down over an entity,
    // the entity is selected immediately; if the same press moves past the
    // small drag threshold, it becomes a view-plane translate drag.
    bool  m_DirectDragArmed = false;
    bool  m_DirectDragActive = false;
    float m_DirectDragDownViewX = 0.0f;
    float m_DirectDragDownViewY = 0.0f;
    Mathematics::Vector3 m_DirectDragPlaneOrigin{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_DirectDragPlaneNormal{0.0f, 0.0f, 1.0f};
    Mathematics::Vector3 m_DirectDragLastPoint{0.0f, 0.0f, 0.0f};

    // Editor services (not owned).
    UndoRedoService* m_UndoRedo = nullptr;
    EditorChangeNotifications* m_ChangeNotifications = nullptr;

    // Active interactive edit session (valid while a gizmo drag is in progress).
    UndoRedoService::InteractiveEdit m_ActiveEdit{};
    bool m_TransformDragActive = false;

    // Callbacks for toolbar synchronisation.
    OnModeChangedCallback m_OnModeChanged;
    OnAxisSpaceChangedCallback m_OnAxisSpaceChanged;

    // Cached state for high-frequency gizmo drags. We cache world position and the
    // parent world TRS once per drag to avoid repeatedly walking ECS hierarchy
    // and calling GetComponent() every mouse-move step.
    struct TranslateDragCache
    {
        bool valid = false;
        Mathematics::Vector3 accumulatedWorldDelta{};
        Mathematics::Vector3 worldPos{};
        Mathematics::Vector3 startPivotWorld{};
        // World rotation at the start of a translate drag. Translation preserves
        // orientation, so this lets us update physics bodies without re-reading
        // ECS state on every mouse move.
        Mathematics::Quaternion worldRot{};
        Mathematics::Vector3 parentPos{};
        Mathematics::Quaternion parentRot{};
        Mathematics::Vector3 parentScale{1.0f, 1.0f, 1.0f};
        Mathematics::Quaternion localRot{};
        Mathematics::Vector3 localScale{1.0f, 1.0f, 1.0f};
    } m_TranslateCache{};

    struct TransformDragBaseline
    {
        GameEngine::ECS::EntityHandle entity{};
        Mathematics::Vector3 localPos{};
        Mathematics::Quaternion localRot{};
        Mathematics::Vector3 localScale{1.0f, 1.0f, 1.0f};
        Mathematics::Vector3 worldPos{};
        Mathematics::Quaternion worldRot{};
        Mathematics::Vector3 worldScale{1.0f, 1.0f, 1.0f};
        Mathematics::Vector3 parentPos{};
        Mathematics::Quaternion parentRot{};
        Mathematics::Vector3 parentScale{1.0f, 1.0f, 1.0f};
    };
    std::vector<TransformDragBaseline> m_TransformDragBaselines;

    // Gizmo thickness settings (in pixels) - used when constant screen size is OFF
    float m_TranslateThickness = 1.5f;
    float m_RotateThickness = 5.5f;
    float m_ScaleThickness = 1.5f;
    
    // Constant screen-space size mode
    bool m_ConstantScreenSize = true;
    
    // Gizmo scale settings (normalized multiplier for constant screen size mode)
    float m_TranslateScale = 1.0f;
    float m_RotateScale = 1.5f;
    float m_ScaleGizmoScale = 1.0f;
    // Rotate snap increment in degrees (applied while holding the snap key).
    float m_RotateSnapIncrementDeg = 45.0f;
    // Enhanced rotate gizmo behavior (culling + view-plane ring + edge-on map).
    // Off by default — the legacy full-ring gizmo is easier to grab (no front-face cull).
    bool m_RotateEnhanced = false;
    // Constant size thickness settings (used when constant screen size is ON)
    float m_TranslateConstantThickness = 0.8f;
    float m_RotateConstantThickness = 4.2f;
    float m_ScaleConstantThickness = 1.5f;
};

} // namespace SceneTools
} // namespace Editor
} // namespace GameEngine
