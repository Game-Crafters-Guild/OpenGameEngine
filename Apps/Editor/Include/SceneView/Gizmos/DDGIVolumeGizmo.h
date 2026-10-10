#pragma once

#include <vector>

#include "ECS/Entity.h"
#include "SceneView/SceneViewGizmos.h"

namespace GameEngine::Editor::SceneTools
{

// Editor-only debug visualization for DDGIVolume (component-only authoring —
// there is no scene-wide auto-fit, so this gizmo is the only way to see
// where a volume's probe grid actually sits before it renders GI). Always
// draws a small icon at the volume's center; the grid bounding box and
// per-probe position markers only draw when hovered or selected, mirroring
// ReflectionProbeGizmo's emphasize-on-demand shape.
//
// v1 has no live GPU readback — there is no precedent anywhere in this
// codebase for a gizmo reading back GPU compute state (NavDebugGizmo, the
// closest analog, reads CPU-precomputed debug data), and DDGI's per-probe
// classify/active flags live in a device-local SSBO with no CPU mirror.
// Markers are drawn in one uniform color, not colored by active state; a
// CPU-visible active-flag mirror is a candidate for a later milestone if
// that turns out to matter for authoring.
class DDGIVolumeGizmo : public IGizmo
{
public:
    DDGIVolumeGizmo() = default;

    void SetSelection(const std::vector<GameEngine::ECS::EntityHandle>& entities);
    void SetHovered(GameEngine::ECS::EntityHandle entity);

    void Render(GizmoRenderContext& context) override;

private:
    std::vector<GameEngine::ECS::EntityHandle> m_SelectedEntities;
    GameEngine::ECS::EntityHandle m_HoveredEntity{};
};

} // namespace GameEngine::Editor::SceneTools
