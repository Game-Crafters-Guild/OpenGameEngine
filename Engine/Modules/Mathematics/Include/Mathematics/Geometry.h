#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

#include "Mathematics/Types.h"
#include "Mathematics/Ray.h"

namespace GameEngine {
namespace Mathematics {

// Simple plane representation: normal (unit-length preferred) and offset d,
// using the convention n . x + d = 0.
struct Plane
{
	Vector3 normal{0.0f, 1.0f, 0.0f};
	float   d = 0.0f;
};

// Axis-aligned bounding box in 3D (min/max representation).
struct AABB
{
	Vector3 min{0.0f, 0.0f, 0.0f};
	Vector3 max{0.0f, 0.0f, 0.0f};

	// Inverted box (min = max float, max = lowest float) that the first
	// Expand overwrites. Finite sentinels: an empty box never carries inf.
	static AABB Empty()
	{
		constexpr float kMax = std::numeric_limits<float>::max();
		constexpr float kLowest = std::numeric_limits<float>::lowest();
		return {{kMax, kMax, kMax}, {kLowest, kLowest, kLowest}};
	}

	void Expand(const Vector3& point)
	{
		min.x = std::min(min.x, point.x);
		min.y = std::min(min.y, point.y);
		min.z = std::min(min.z, point.z);
		max.x = std::max(max.x, point.x);
		max.y = std::max(max.y, point.y);
		max.z = std::max(max.z, point.z);
	}

	// Component-wise union; expanding by an Empty() box leaves this box unchanged.
	void Expand(const AABB& other)
	{
		min.x = std::min(min.x, other.min.x);
		min.y = std::min(min.y, other.min.y);
		min.z = std::min(min.z, other.min.z);
		max.x = std::max(max.x, other.max.x);
		max.y = std::max(max.y, other.max.y);
		max.z = std::max(max.z, other.max.z);
	}

	// True when the box holds no point: Empty(), or min above max on any axis.
	bool IsEmpty() const
	{
		return min.x > max.x || min.y > max.y || min.z > max.z;
	}

	// Moves every face outward by `amount` on its axis. An empty box stays empty.
	void Inflate(const Vector3& amount)
	{
		if (IsEmpty())
			return;
		min = min - amount;
		max = max + amount;
	}

	// The overlap of two boxes; Empty() when they share no point.
	static AABB Intersection(const AABB& a, const AABB& b)
	{
		const AABB overlap{{std::max(a.min.x, b.min.x), std::max(a.min.y, b.min.y), std::max(a.min.z, b.min.z)},
		                   {std::min(a.max.x, b.max.x), std::min(a.max.y, b.max.y), std::min(a.max.z, b.max.z)}};
		return overlap.IsEmpty() ? Empty() : overlap;
	}

	// The 8 corners. Corner i takes max.x when bit 0 of i is set, max.y for
	// bit 1 and max.z for bit 2, so corner 0 is min and corner 7 is max.
	std::array<Vector3, 8> Corners() const
	{
		return {{{min.x, min.y, min.z}, {max.x, min.y, min.z}, {min.x, max.y, min.z}, {max.x, max.y, min.z},
		         {min.x, min.y, max.z}, {max.x, min.y, max.z}, {min.x, max.y, max.z}, {max.x, max.y, max.z}}};
	}

	// Full surface area, 2 * (xy + yz + zx). An inverted (empty) box has area 0.
	float SurfaceArea() const
	{
		const float dx = max.x - min.x;
		const float dy = max.y - min.y;
		const float dz = max.z - min.z;
		if (dx < 0.0f || dy < 0.0f || dz < 0.0f)
			return 0.0f;
		return 2.0f * (dx * dy + dy * dz + dz * dx);
	}
};

// Bounding box stored as center + per-axis half-extents.
// Reused across mesh GPU entries, runtime models, and ECS components.
struct BoundingBox
{
	Vector3 center{0.0f, 0.0f, 0.0f};
	Vector3 halfExtents{0.5f, 0.5f, 0.5f};

	float RadiusSquared() const
	{
		return Vector3::Dot(halfExtents, halfExtents);
	}

	float Radius() const
	{
		return std::sqrt(RadiusSquared());
	}

	AABB ToAABB() const
	{
		return {center - halfExtents, center + halfExtents};
	}

	static BoundingBox FromMinMax(const Vector3& min, const Vector3& max)
	{
		return {(min + max) * 0.5f, (max - min) * 0.5f};
	}

	// Compute a world-space AABB by transforming through a 4x4 column-major
	// matrix (Arvo's method).
	AABB TransformToAABB(const float worldMatrix[16]) const
	{
		const float* m = worldMatrix;

		const float wcx = m[0] * center.x + m[4] * center.y + m[8]  * center.z + m[12];
		const float wcy = m[1] * center.x + m[5] * center.y + m[9]  * center.z + m[13];
		const float wcz = m[2] * center.x + m[6] * center.y + m[10] * center.z + m[14];

		const float whx = std::abs(m[0]) * halfExtents.x + std::abs(m[4]) * halfExtents.y + std::abs(m[8])  * halfExtents.z;
		const float why = std::abs(m[1]) * halfExtents.x + std::abs(m[5]) * halfExtents.y + std::abs(m[9])  * halfExtents.z;
		const float whz = std::abs(m[2]) * halfExtents.x + std::abs(m[6]) * halfExtents.y + std::abs(m[10]) * halfExtents.z;

		return {{wcx - whx, wcy - why, wcz - whz},
		        {wcx + whx, wcy + why, wcz + whz}};
	}
};

// Signed distance from a point to a plane: > 0 in the direction of the normal.
inline float DistanceToPlane(const Plane& plane, const Vector3& point)
{
	return plane.normal.x * point.x +
	       plane.normal.y * point.y +
	       plane.normal.z * point.z +
	       plane.d;
}

// Ray-plane intersection. Returns false if the ray is parallel to the plane or
// if the intersection lies behind the ray origin (t < 0).
inline bool IntersectRayPlane(const Ray3D& ray,
		                      const Plane& plane,
		                      float& outT,
		                      Vector3& outPoint)
{
	const float denom =
	    plane.normal.x * ray.direction.x +
	    plane.normal.y * ray.direction.y +
	    plane.normal.z * ray.direction.z;
	const float kEpsilon = 1e-5f;
	if (std::fabs(denom) < kEpsilon)
	{
	    return false; // Parallel or nearly parallel
	}

	const float num = -(
	    plane.normal.x * ray.origin.x +
	    plane.normal.y * ray.origin.y +
	    plane.normal.z * ray.origin.z +
	    plane.d);
	const float t = num / denom;
	if (t < 0.0f)
	{
	    return false; // Intersection behind origin
	}

	outT = t;
	outPoint = ray.origin + ray.direction * t;
	return true;
}

// Ray/sphere intersection for a unit-length ray direction. On a hit, outT is the
// nearest intersection at or in front of the origin: the entry point, or the exit
// point when the origin is inside the sphere. Returns false when the ray misses
// the sphere or the sphere lies entirely behind the origin.
inline bool IntersectRaySphere(const Ray3D& ray,
                               const Vector3& center,
                               float radius,
                               float& outT)
{
	const Vector3 toCenter = center - ray.origin;
	const float projection = Vector3::Dot(toCenter, ray.direction);
	const float discriminant = projection * projection - (toCenter.LengthSquared() - radius * radius);
	if (discriminant < 0.0f)
	    return false;

	const float root = std::sqrt(discriminant);
	const float nearT = projection - root;
	const float t = nearT >= 0.0f ? nearT : projection + root;
	if (!(t >= 0.0f))
	    return false; // Behind the origin, or NaN input.

	outT = t;
	return true;
}

// Standard slab-based ray/AABB intersection. Returns false if there is no
// intersection. On success, outTMin/outTMax give the parametric entry/exit
// distances along the ray (t >= 0).
inline bool IntersectRayAABB(const Ray3D& ray,
	                         const AABB& box,
	                         float& outTMin,
	                         float& outTMax)
{
	const float kInfinity = std::numeric_limits<float>::infinity();
	const float kEpsilon  = 1e-8f;

	float tMin = 0.0f;
	float tMax = kInfinity;

	for (int axis = 0; axis < 3; ++axis)
	{
	    const float origin = ray.origin[axis];
	    const float dir    = ray.direction[axis];
	    const float bmin   = box.min[axis];
	    const float bmax   = box.max[axis];

	    if (std::fabs(dir) < kEpsilon)
	    {
	        // Ray is nearly parallel to this slab; reject if origin is outside.
	        if (origin < bmin || origin > bmax)
	        {
	            return false;
	        }
	        continue;
	    }

	    const float invD = 1.0f / dir;
	    float t0 = (bmin - origin) * invD;
	    float t1 = (bmax - origin) * invD;
	    if (t0 > t1)
	    {
	        std::swap(t0, t1);
	    }

	    tMin = std::max(tMin, t0);
	    tMax = std::min(tMax, t1);
	    if (tMax < tMin)
	    {
	        return false;
	    }
	}

	outTMin = tMin;
	outTMax = tMax;
	return true;
}

// Even-odd point-in-polygon test in 2D. The polygon closes implicitly (the last
// vertex connects back to the first); fewer than three vertices contain nothing.
// The boundary is half-open under either winding: points on the left and bottom edges
// are inside, points on the right and top edges outside, so outlines sharing an edge
// never both contain a point on it.
inline bool PointInPolygon(const Vector2& point, std::span<const Vector2> polygon)
{
	if (polygon.size() < 3)
	    return false;

	bool inside = false;
	for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
	{
	    const Vector2& a = polygon[i];
	    const Vector2& b = polygon[j];
	    // Only an edge that straddles point.y reaches the division, so b.y != a.y there.
	    if ((a.y > point.y) != (b.y > point.y) &&
	        point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x)
	    {
	        inside = !inside;
	    }
	}
	return inside;
}

// Twice the signed area of a 2D polygon (the shoelace sum), closing implicitly:
// positive when the vertices run counter-clockwise with +y up; 0 for fewer than three
// vertices. Summed relative to the first vertex, so a small polygon far from the origin keeps
// its precision (absolute products at 30 km cancel to float noise).
inline float PolygonDoubledSignedArea(std::span<const Vector2> polygon)
{
	if (polygon.size() < 3)
	    return 0.0f;
	const Vector2 origin = polygon.front();
	float sum = 0.0f;
	for (std::size_t i = 2; i < polygon.size(); ++i)
	{
	    const float ax = polygon[i - 1].x - origin.x;
	    const float ay = polygon[i - 1].y - origin.y;
	    const float bx = polygon[i].x - origin.x;
	    const float by = polygon[i].y - origin.y;
	    sum += ax * by - bx * ay;
	}
	return sum;
}

// Whether segments ab and cd share a point, ends and collinear overlaps included.
inline bool SegmentsIntersect(const Vector2& a, const Vector2& b, const Vector2& c, const Vector2& d)
{
	const auto cross = [](const Vector2& o, const Vector2& p, const Vector2& q) {
	    return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x);
	};
	const auto within = [](const Vector2& p, const Vector2& q, const Vector2& r) {
	    return std::min(p.x, q.x) <= r.x && r.x <= std::max(p.x, q.x) && std::min(p.y, q.y) <= r.y &&
	           r.y <= std::max(p.y, q.y);
	};
	const float d1 = cross(c, d, a);
	const float d2 = cross(c, d, b);
	const float d3 = cross(a, b, c);
	const float d4 = cross(a, b, d);
	if (((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) && ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f)))
	    return true;
	return (d1 == 0.0f && within(c, d, a)) || (d2 == 0.0f && within(c, d, b)) || (d3 == 0.0f && within(a, b, c)) ||
	       (d4 == 0.0f && within(a, b, d));
}

// The convex hull of `points` (Andrew's monotone chain), counter-clockwise with +y up, without
// collinear points; fewer than three distinct points come back as they are, deduplicated.
inline std::vector<Vector2> ConvexHull(std::span<const Vector2> points)
{
	std::vector<Vector2> sorted(points.begin(), points.end());
	std::sort(sorted.begin(), sorted.end(), [](const Vector2& p, const Vector2& q) {
	    return p.x < q.x || (p.x == q.x && p.y < q.y);
	});
	sorted.erase(std::unique(sorted.begin(), sorted.end(),
	                         [](const Vector2& p, const Vector2& q) { return p.x == q.x && p.y == q.y; }),
	             sorted.end());
	if (sorted.size() < 3)
	    return sorted;
	const auto turnsLeft = [](const Vector2& o, const Vector2& p, const Vector2& q) {
	    return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x) > 0.0f;
	};
	std::vector<Vector2> hull(2 * sorted.size());
	std::size_t count = 0;
	for (const Vector2& p : sorted)
	{
	    while (count >= 2 && !turnsLeft(hull[count - 2], hull[count - 1], p))
	        --count;
	    hull[count++] = p;
	}
	for (std::size_t i = sorted.size() - 1, lower = count + 1; i-- > 0;)
	{
	    while (count >= lower && !turnsLeft(hull[count - 2], hull[count - 1], sorted[i]))
	        --count;
	    hull[count++] = sorted[i];
	}
	hull.resize(count - 1); // the last point repeats the first
	return hull;
}

} // namespace Mathematics
} // namespace GameEngine
