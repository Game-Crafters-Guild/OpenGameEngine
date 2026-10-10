#pragma once

#include "Rendering/Common/Math.h"

namespace GameEngine { namespace Rendering {

// Shared frustum math helpers used by engine systems and rendering examples.
// All math assumes the canonical Mathematics types:
// - Matrix4x4 is a thin wrapper over glm::mat4 (column-major)
// - Left-handed coordinate system with Z+ forward
// - Depth range [0, 1] (D3D/Vulkan-style clip space)

// Extract and normalize the 6 view frustum planes from a row-major
// view-projection matrix. Planes are returned in the order:
// 0 = Left, 1 = Right, 2 = Bottom, 3 = Top, 4 = Near, 5 = Far.
void ExtractFrustumPlanes(const Matrix4x4& viewProj, Vector4* outPlanes);

// Conservative sphere vs. frustum test. Returns true if the sphere is
// at least partially inside the frustum defined by the given planes.
// The planes are expected to come from ExtractFrustumPlanes.
bool TestSphereFrustum(const Vector3& center, float radius, const Vector4* planes);

// Exact-per-plane axis-aligned box vs. frustum test. Returns false only when
// the box lies entirely outside one plane; a box that straddles a frustum
// corner can still report true. The planes are expected to come from
// ExtractFrustumPlanes.
bool TestAabbFrustum(const Vector3& boxMin, const Vector3& boxMax, const Vector4* planes);

} } // namespace GameEngine::Rendering

