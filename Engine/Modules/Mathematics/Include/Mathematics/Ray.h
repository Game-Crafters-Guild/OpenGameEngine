#pragma once

#include "Mathematics/Types.h"

namespace GameEngine {
namespace Mathematics {

// Generic 3D ray type shared across editor, physics, picking, etc.
// Uses the canonical Mathematics::Vector3 type for origin/direction.
struct Ray3D
{
	Vector3 origin{0.0f, 0.0f, 0.0f};
	Vector3 direction{0.0f, 0.0f, 1.0f};
};

// -----------------------------------------------------------------------------
// Ray helpers
// -----------------------------------------------------------------------------

// Evaluate a point along the ray at parameter t: P(t) = origin + t * direction.
// This is a small, header-only helper so it can be used freely in hot paths
// (e.g., picking, gizmos) without pulling in additional dependencies.
inline Vector3 RayPointAt(const Ray3D& ray, float t)
{
	return Vector3(
		ray.origin.x + ray.direction.x * t,
		ray.origin.y + ray.direction.y * t,
		ray.origin.z + ray.direction.z * t);
}

// Intersect a ray with an infinite plane defined by a point on the plane and a
// (not necessarily normalised) plane normal. Returns false when the ray is
// nearly parallel to the plane; otherwise writes out the parametric distance
// along the ray and the corresponding world-space point.
//
// The caller remains responsible for clamping t (e.g., discarding t <= 0 when
// used for camera picking) or for enforcing any finite bounds on the plane.
inline bool IntersectRayPlane(const Ray3D& ray,
	                          const Vector3& planePoint,
	                          const Vector3& planeNormal,
	                          float& outT,
	                          Vector3& outPoint)
{
	// Dot(planeNormal, ray.direction)
	const float denom = Vector3::Dot(planeNormal, ray.direction);
	if (denom > -1.0e-4f && denom < 1.0e-4f)
	{
		return false; // Ray nearly parallel to plane.
	}

	// Signed distance from ray origin to plane along plane normal.
	Vector3 w0(
		ray.origin.x - planePoint.x,
		ray.origin.y - planePoint.y,
		ray.origin.z - planePoint.z);
	const float num = -Vector3::Dot(planeNormal, w0);
	const float t   = num / denom;

	outT     = t;
	outPoint = RayPointAt(ray, t);
	return true;
}

} // namespace Mathematics
} // namespace GameEngine
