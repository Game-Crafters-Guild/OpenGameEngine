#pragma once

#include "ECS/ECS.h"
#include "SceneViewGizmos.h"

#include <cstdint>
#include <functional>

namespace GameEngine::Editor::SceneTools
{

struct MeasureEndpointSelection
{
    ECS::EntityHandle Entity{};
    std::uint8_t Endpoint = 0; // 1 = start, 2 = end

    bool IsValid() const { return Entity.IsValid() && (Endpoint == 1u || Endpoint == 2u); }
};

class MeasureSceneGizmo : public IGizmo
{
public:
    explicit MeasureSceneGizmo(ECS::World& world);
    MeasureEndpointSelection& Selection() { RefreshSelection(); return m_Selection; }
    const MeasureEndpointSelection& Selection() const { RefreshSelection(); return m_Selection; }

    using EndpointPickedCallback = std::function<void(ECS::EntityHandle, std::uint8_t)>;
    using EndpointDragStartedCallback = std::function<void(ECS::EntityHandle, std::uint8_t)>;
    using EndpointDraggedCallback = std::function<void(const Mathematics::Vector3& deltaWorld)>;
    using EndpointDragEndedCallback = std::function<void()>;

    void SetEndpointPickedCallback(EndpointPickedCallback callback);
    void SetEndpointDragStartedCallback(EndpointDragStartedCallback callback);
    void SetEndpointDraggedCallback(EndpointDraggedCallback callback);
    void SetEndpointDragEndedCallback(EndpointDragEndedCallback callback);
    void SetHovered(ECS::EntityHandle entity);
    void SetViewportScaleCompensation(float scale);

    void Render(GizmoRenderContext& context) override;
    GizmoHitResult HitTest(const GizmoRay& ray) override;
    bool HandlePointerEvent(const ScenePointerEvent& event, const GizmoHit& hit) override;

private:
    struct EndpointHit
    {
        ECS::EntityHandle Entity{};
        std::uint8_t Endpoint = 0;
    };

    ECS::World* const m_World;
    void RefreshSelection() const;
    mutable uint64 m_WorldGeneration = 0;
    mutable MeasureEndpointSelection m_Selection;
    EndpointPickedCallback m_OnEndpointPicked;
    EndpointDragStartedCallback m_OnEndpointDragStarted;
    EndpointDraggedCallback m_OnEndpointDragged;
    EndpointDragEndedCallback m_OnEndpointDragEnded;
    ECS::EntityHandle m_HoveredEntity{};
    mutable EndpointHit m_LastHit{};
    float m_ViewportScaleCompensation = 1.0f;
    bool m_DraggingEndpoint = false;
    Mathematics::Vector3 m_DragPlaneOrigin{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_DragPlaneNormal{0.0f, 0.0f, 1.0f};
    Mathematics::Vector3 m_LastDragHit{0.0f, 0.0f, 0.0f};
};

} // namespace GameEngine::Editor::SceneTools
