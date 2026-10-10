#pragma once

#include "Rendering/CameraTypes.h"

namespace GameEngine::Engine::Renderer
{

// Screen projection of an infinitely distant directional source (the sun), shared by every
// pass that needs to know where the sun lands on screen: the lens flare's per-source
// placement and occlusion probe, and the sun glare's occlusion probe.
//
// Kept in one place because the two callers must agree — a glare that fades on a silhouette
// the flare thinks it is clear of is a bug nothing would catch.

// Project a world position through the column-major view-projection matrix.
// Returns false when the point is at or behind the camera (w <= 0).
bool ProjectToNdc(const float* viewProj, float x, float y, float z, float& outNdcX,
                  float& outNdcY, float& outNdcDepth);

// Project an infinite directional source. Perspective views can transform the direction
// directly with homogeneous w = 0, avoiding camera-position precision loss in large worlds.
// Editor 2D views are orthographic, so use a finite camera-relative point there. Sun depth
// is always the reverse-Z far plane (0), allowing any scene geometry at the source screen
// position to occlude it.
//
// `direction` is the toward-sun direction (surface -> sun), the same convention
// SkySettings::scatteringSunDir carries.
bool ProjectSunDirectionToNdc(const ::GameEngine::Rendering::CameraData& cam,
                              const float direction[3], float& outNdcX, float& outNdcY,
                              float& outNdcDepth);

} // namespace GameEngine::Engine::Renderer
