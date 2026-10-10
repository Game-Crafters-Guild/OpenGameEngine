#pragma once

#include "Types/Color.h"

namespace GameEngine::Components
{
struct WorldTransform;
}

namespace GameEngine::Editor::SceneTools
{
class GizmoRenderContext;

// The shapes local volumes share (post process, wind), with the same meaning for their scale.
enum class VolumeShape
{
    Box,
    Sphere,
    Capsule,
    Cylinder,
};

// A local volume whose unit shape is scaled, rotated and placed by `xf`, drawn as a wireframe, and its
// blend region, every half-extent grown by `blendDistance`, as a second one in `blendColor`.
void DrawLocalVolume(GizmoRenderContext& context,
                     const Components::WorldTransform& xf,
                     VolumeShape shape,
                     float blendDistance,
                     const Color& innerColor,
                     const Color& blendColor);

} // namespace GameEngine::Editor::SceneTools
