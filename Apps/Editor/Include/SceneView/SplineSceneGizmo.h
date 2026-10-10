#pragma once

#include "SceneViewGizmos.h"
#include "SceneView/SplineTool.h"

#include "ECS/Entity.h"
#include "SceneView/SplineDrapePolylines.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace GameEngine::Editor::SceneTools
{

// Persistent gizmo that renders CatmullRom curves for ALL SplineComponent
// entities in the scene. Unlike SplineGizmo (which only renders the active
// tool stroke), this gizmo keeps splines visible after creation.
class SplineSceneGizmo : public IGizmo
{
public:
    explicit SplineSceneGizmo(ECS::World& world) : m_State(AcquireSplineInteractionState(world)) {}
    SplineInteractionState& State() const { return *m_State; }

    void SetSelectionQuery(std::function<ECS::EntityHandle()> query)
    {
        m_SelectionQuery = std::move(query);
    }

    void SetIsEntitySelectedQuery(std::function<bool(ECS::EntityHandle)> query)
    {
        m_IsEntitySelectedQuery = std::move(query);
    }

    // IGizmo
    void Render(GizmoRenderContext& context) override;

private:
    std::shared_ptr<SplineInteractionState> m_State;
    struct PassiveLineCache
    {
        uint32_t DataIndex = 0;
        uint32_t Generation = 0;
        uint64_t DataVersion = 0;
        float Matrix[16] = {};
        std::vector<Mathematics::Vector3> Vertices;
        uint64_t LastSeen = 0;
    };

    // Draped display polylines per spline (centerline when draping is on;
    // ±width envelope edges for the selected spline), built by conform
    // raycasts and rebuilt only when the spline data, its version, the entity
    // transform, the edge requirement, or the ground it was draped onto
    // changes. Stored in world space — the drape is view-independent; only the
    // camera-facing thick-line expansion runs per frame.
    struct DrapeCache
    {
        uint32_t DataIndex = 0;
        uint32_t Generation = 0;
        uint64_t DataVersion = 0;
        float Matrix[16] = {};
        bool HasEdges = false;
        // The ground these polylines were draped onto (ConformSurfaceRevision).
        // The drape is MEASURED, so a bake, a sculpt or a moved terrain
        // invalidates it with no spline-side input changing — and the displayed
        // centerline must not disagree with where the placement controllers put
        // the pieces, which observe the same revision.
        uint64_t SurfaceRevision = 0;
        SplineDrapedPolylines Polylines;
        // Each knot's draped height (the surface under it plus the drape's lift), NaN where the
        // conform ray missed: the foot of the knot's stem.
        std::vector<float> KnotDrapedY;
        uint64_t LastSeen = 0;
    };

    std::function<ECS::EntityHandle()> m_SelectionQuery;
    std::function<bool(ECS::EntityHandle)> m_IsEntitySelectedQuery;
    mutable uint64_t m_CacheEpoch = 0;
    mutable std::unordered_map<uint32_t, PassiveLineCache> m_PassiveLineCache;
    mutable std::unordered_map<uint32_t, DrapeCache> m_DrapeCache;
};

} // namespace GameEngine::Editor::SceneTools
