#pragma once

#include "SceneView/SceneViewGizmos.h"
#include "SceneView/SceneViewTools.h"

#include "AssetCore/GUID.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Transform.h"
#include "ECS/ECS.h"
#include "Mathematics/Vector3.h"
#include "TerrainECS/TerrainZoneAuthoring.h"

#include <cstdint>
#include <vector>

namespace GameEngine {

class SceneViewController;

namespace Editor { class UndoRedoService; class EditorChangeNotifications; }

namespace Editor::SceneTools {

// What a brush dab writes into the active zone's payload (design §8, §3.2).
enum class TerrainBrushMode : uint8_t
{
    Raise, // sculpt: add a positive height offset
    Lower, // sculpt: add a negative height offset
    Paint, // paint: add weight into the target splat layer's mask
};

// One step of a stroke's lifecycle, independent of what produced the surface point.
// The viewport maps pointer down/move/up onto these; automation drives them directly.
enum class TerrainStrokePhase : uint8_t
{
    Begin,    // open the stroke (undo boundary) and apply its first dab
    Continue, // apply another dab into the open stroke
    End,      // commit the stroke, recording its single undo entry
};

// Ring cursor tessellated to the terrain hit, oriented to the surface normal.
class TerrainBrushCursorGizmo : public IGizmo
{
public:
    void Render(GizmoRenderContext& context) override;

    Mathematics::Vector3 Center{};
    Mathematics::Vector3 Normal{0.0f, 1.0f, 0.0f};
    float Radius = 8.0f;
    TerrainBrushMode Mode = TerrainBrushMode::Raise;
    bool Active = false;
};

// Terrain sculpt/paint brush. Strokes never touch the heightfield: they paint
// into a transformable TerrainSculptZone / TerrainPaintZone entity's payload
// (edit-pipeline §3.2), so every edit is non-destructive, movable, and undoable.
// With no matching zone selected, the first stroke auto-creates one fitted to
// the stroke and auto-grows it up to a cap.
class TerrainBrushTool : public ISceneTool
{
public:
    explicit TerrainBrushTool(SceneViewController& owner);

    const char* GetName() const override { return "TerrainBrushTool"; }
    void OnActivated() override;
    void OnDeactivated() override;
    void OnPointerEvent(const ScenePointerEvent& event) override;
    void OnKeyEvent(const SceneKeyEvent& event) override;
    void GatherGizmos(GizmoCollector& collector) override;

    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_UndoRedo = undo; }
    void SetChangeNotifications(Editor::EditorChangeNotifications* n) { m_ChangeNotifications = n; }

    // Debug/automation control of brush parameters (mirrors the '[' / ']' / '-' / '=' keys),
    // so headless stroke harnesses can drive a specific brush footprint. Radius/strength are
    // clamped to the tool's min/max at apply time.
    void SetRadius(float radius) { m_Radius = radius; }
    void SetStrength(float strength) { m_Strength = strength; }
    void SetMode(TerrainBrushMode mode) { m_Mode = mode; }
    float GetRadius() const { return m_Radius; }
    float GetStrength() const { return m_Strength; }
    TerrainBrushMode GetMode() const { return m_Mode; }

    // The channel role a Paint stroke writes into — the splat channel a new paint zone takes,
    // and through the terrain's role binding, the material it shades with. Clamped here because
    // the value reaches a bake that indexes a fixed four channels; an out-of-range one would paint
    // a different channel from the one the picker is showing. One value for the whole editor:
    // every window's scene view and quad views own their own brush tool, and a role picked in
    // one of them — or from an inspector, which has no view — means the same thing in all of
    // them, so every tool paints the one shared role.
    static void SetPaintLayer(uint32_t layer);
    static uint32_t GetPaintLayer() { return s_PaintLayer; }

    // Drive one stroke phase at an already-resolved surface point, dispatching to the
    // planet or planar stroke model by the active terrain's domain. This is the body
    // OnPointerEvent runs once it has turned a pointer ray into a surface point, so an
    // automated stroke and a viewport stroke execute the same code — including zone
    // auto-creation, falloff, dirty-region signalling and the undo boundary (one
    // Begin..End is one undo entry). `surfacePoint` is unused for End; End closes
    // whichever stroke model is open, so it is safe with no stroke in flight.
    // The return differs by phase, and none of it means "texels moved":
    //   planar Begin          — whether the stroke opened, i.e. a zone payload was resolved
    //   planar Continue       — whether the point was dispatched; the dab is still dropped
    //                           if the stroke holds no payload
    //   planet Begin/Continue — whether the point named a surface direction; the dab is
    //                           still dropped in Paint mode (planet sculpt is raise|lower)
    //   End                   — whether an undo entry was recorded
    // Whether a stroke actually wrote is an effect-side question: the zone payload's
    // written magnitude for planar, SphereSculptVersion() for the planet.
    bool ApplyStrokePhase(TerrainStrokePhase phase, const Mathematics::Vector3& surfacePoint);

    // Resolve a world-space target onto the active terrain's surface with no camera:
    // a planar terrain is probed straight down the target's XZ column, a planet inward
    // along the direction from the planet centre. Runs the SAME picks the pointer path
    // uses, so an automated stroke lands where a viewport stroke would. False when no
    // terrain surface lies under the target.
    bool ProbeSurfaceAt(const Mathematics::Vector3& worldTarget, Mathematics::Vector3& outHit,
                        Mathematics::Vector3& outNormal);

    // True while a stroke is open (pointer held, or an automated stroke not yet ended).
    bool IsStroking() const { return m_IsStroking; }

    // The zone the current/last planar stroke writes into — the effect-side handle an
    // automated stroke reports so a caller can verify the payload actually changed.
    // Invalid entity / null GUID until a planar stroke has run.
    ECS::EntityHandle GetActiveZone() const { return m_ActiveZone; }
    const GUID& GetActivePayload() const { return m_ActivePayload; }

private:
    bool RaycastTerrain(const GizmoRay& ray, Mathematics::Vector3& outHit,
                        Mathematics::Vector3& outNormal);

    // Resolve the pointer/probe ray against whichever surface the active terrain draws.
    bool RaycastSurface(const GizmoRay& ray, Mathematics::Vector3& outHit,
                        Mathematics::Vector3& outNormal);

    // Sphere-stroke undo boundary, the sphere sibling of BeginStroke/FinalizeStroke.
    // Begin arms the service's lazy per-page pre-image capture; End snapshots the
    // touched pages' post-images and pushes ONE SphereSculptStrokeCommand. End returns
    // whether an entry was pushed (false: nothing written, no undo service, or no
    // stroke active).
    void BeginSphereStroke();
    bool EndSphereStroke();

    // Planet editing v1 (plan §planet-editing). When the active terrain is a spherical CBT
    // planet the brush edits the CBT sphere sculpt layer (an additive per-face height
    // layer) instead of a planar zone: ray -> sphere hit -> the feature's ApplySphereSculptDab.
    // Zone entities on the sphere are a documented v2 deferral. Returns the planet radius via
    // outRadius when a spherical terrain is active.
    bool IsSphericalPlanet(float& outRadius) const;
    // Ray vs the planet sphere (centred at the world origin, radius R). Nearest positive hit.
    bool RaycastPlanet(const GizmoRay& ray, float radius, Mathematics::Vector3& outHit,
                       Mathematics::Vector3& outNormal) const;
    // Drive one planet sculpt dab at surface direction `hitDir` (unit) with footprint from
    // the current brush radius / planetRadius. Raise/Lower only (paint on the planet is v2).
    void ApplySphereDab(const Mathematics::Vector3& hitDir, float planetRadius);

    // Resolve the zone to paint into: a selected matching zone, else the
    // stroke's auto-created zone. Auto-creates + grows as needed. Returns false
    // when no terrain is under the cursor / creation failed.
    bool EnsureActiveZone(const Mathematics::Vector3& hit);

    // Write one brush dab into the active zone's payload and record its dirty
    // texel rect via the service store.
    void ApplyDab(const Mathematics::Vector3& hit);

    void BeginStroke(const Mathematics::Vector3& hit);
    void FinalizeStroke();

    SceneViewController& m_Owner;
    TerrainBrushCursorGizmo m_Cursor;

    // Brush setting defaults.
    static constexpr float kDefaultBrushRadius = 8.0f;   // world meters
    static constexpr float kDefaultBrushStrength = 2.0f; // meters/dab (sculpt) or weight/dab (paint)
    static constexpr uint32_t kDefaultPaintLayer = 2;    // splat layer for Paint mode (0-3)

    // Brush settings.
    TerrainBrushMode m_Mode = TerrainBrushMode::Raise;
    float m_Radius = kDefaultBrushRadius;
    float m_Strength = kDefaultBrushStrength;
    static inline uint32_t s_PaintLayer = kDefaultPaintLayer;
    TerrainECS::ZoneAuthoringSettings m_Authoring;

    // Not owned.
    Editor::UndoRedoService* m_UndoRedo = nullptr;
    Editor::EditorChangeNotifications* m_ChangeNotifications = nullptr;

    // Stroke state.
    bool m_IsStroking = false;
    bool m_SphereStroking = false; // a sphere-stroke capture is armed (undo boundary open)
    ECS::EntityHandle m_ActiveZone{};
    GUID m_ActivePayload{};
    bool m_CreatedZoneThisStroke = false;
    TerrainECS::ZoneFootprint m_Footprint{};
    uint32_t m_CapRestartGuard = 0; // bounds cap-crossing zone restarts per stroke

    // Undo capture: the active zone's pre-stroke component + transform + payload.
    // A stroke that grows the zone changes the component extent + transform, so
    // the undo must revert those WITH the payload (design §8: growth folds into
    // the same entry) — captured surgically, without a whole-world snapshot.
    bool m_ZoneWasPaint = false;
    Components::TerrainSculptZone m_SculptBefore{};
    Components::TerrainPaintZone m_PaintBefore{};
    Components::Transform m_TransformBefore{};
    Components::WorldTransform m_WorldTransformBefore{};
    TerrainECS::TerrainZonePayload m_PayloadBefore;
};

} // namespace Editor::SceneTools
} // namespace GameEngine
