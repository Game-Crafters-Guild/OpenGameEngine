// Scene View gizmo helpers: per-view line accumulation + GridGizmo.

#include "SceneView/SceneViewGizmos.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <cmath>

#include "Mathematics/Geometry.h"
#include "Mathematics/VectorOps.h"

namespace GameEngine {
namespace Editor {
namespace SceneTools {

namespace
    {
        using GroupVec = std::vector<GizmoLineGroup>;
        // NOTE:
        // Gizmo groups are produced and consumed during SceneViewController::Record(), which
        // is executed as part of RenderGraph pass execution. RenderGraph may execute passes
        // on worker threads; using a single global map here can cause data races and heap
        // corruption (e.g., vector reallocation while another thread is writing).
        //
        // Use thread-local storage to keep gizmo accumulation isolated per executing thread.
        static thread_local std::unordered_map<Rendering::ViewId, GroupVec> g_LineGroups;

        using TriangleGroupVec = std::vector<GizmoTriangleGroup>;
        static thread_local std::unordered_map<Rendering::ViewId, TriangleGroupVec> g_TriangleGroups;

        static constexpr float kPi              = GameEngine::Mathematics::Pi;

        using GameEngine::Mathematics::Vector3;

        static GizmoLineGroup& GetOrCreateGroup(Rendering::ViewId viewId,
                                                const Color& color,
                                                float thickness,
                                                GizmoDepthMode depthMode)
    {
        GroupVec& groups = g_LineGroups[viewId];
        for (auto& g : groups)
        {
            if (g.thickness == thickness &&
                g.depthMode == depthMode &&
                g.color == color)
            {
                return g;
            }
        }

	        GizmoLineGroup newGroup;
	        newGroup.color = color;
	        newGroup.thickness = thickness;
	        newGroup.depthMode = depthMode;
	        groups.push_back(std::move(newGroup));
	        return groups.back();
    }

		    static GizmoTriangleGroup& GetOrCreateTriangleGroup(Rendering::ViewId viewId,
		                                                       const Color& color,
		                                                       std::int32_t layer,
		                                                       GizmoDepthMode depthMode)
		    {
		        TriangleGroupVec& groups = g_TriangleGroups[viewId];
		        for (auto& g : groups)
		        {
		            if (g.layer   == layer &&
		                g.depthMode == depthMode &&
		                g.color == color)
		            {
		                return g;
		            }
		        }

		        GizmoTriangleGroup newGroup;
		        newGroup.color = color;
		        newGroup.layer = layer;
		        newGroup.depthMode = depthMode;
		        groups.push_back(std::move(newGroup));
		        return groups.back();
		    }

        // Append positions to a group's packed xyz vertex stream. No explicit
        // reserve: resize grows geometrically, while reserve(size + N) on every
        // call would defeat that and turn the hot path into O(N^2).
        void AppendPositions(std::vector<float>& dst, const Vector3* positions, std::size_t count)
        {
            const std::size_t base = dst.size();
            dst.resize(base + count * 3u);
            float* out = dst.data() + base;
            for (std::size_t i = 0; i < count; ++i)
            {
                out[i * 3u + 0u] = positions[i].x;
                out[i * 3u + 1u] = positions[i].y;
                out[i * 3u + 2u] = positions[i].z;
            }
        }

        void AppendTriangle(std::vector<Vector3>& verts, const Vector3& a, const Vector3& b, const Vector3& c)
        {
            verts.push_back(a);
            verts.push_back(b);
            verts.push_back(c);
        }

        // Box corners are in Mathematics::AABB::Corners order: bit 0 of the
        // index selects +X, bit 1 +Y and bit 2 +Z.
        void AppendBoxTriangles(std::vector<Vector3>& verts, const std::array<Vector3, 8>& v)
        {
            // +Z face
            AppendTriangle(verts, v[4], v[5], v[7]);
            AppendTriangle(verts, v[4], v[7], v[6]);
            // -Z face
            AppendTriangle(verts, v[0], v[3], v[1]);
            AppendTriangle(verts, v[0], v[2], v[3]);
            // +X face
            AppendTriangle(verts, v[1], v[3], v[7]);
            AppendTriangle(verts, v[1], v[7], v[5]);
            // -X face
            AppendTriangle(verts, v[0], v[6], v[2]);
            AppendTriangle(verts, v[0], v[4], v[6]);
            // +Y face
            AppendTriangle(verts, v[2], v[6], v[7]);
            AppendTriangle(verts, v[2], v[7], v[3]);
            // -Y face
            AppendTriangle(verts, v[0], v[5], v[4]);
            AppendTriangle(verts, v[0], v[1], v[5]);
        }

        // The 12 box edges as corner-index pairs in the same corner order: the
        // -Z face, the +Z face, then the edges joining them.
        constexpr std::array<std::uint8_t, 24> kBoxEdgeCorners = {
            0, 1, 1, 3, 3, 2, 2, 0,
            4, 5, 5, 7, 7, 6, 6, 4,
            0, 4, 1, 5, 3, 7, 2, 6};

        void DrawBoxEdges(GizmoRenderContext& context,
                          const std::array<Vector3, 8>& corners,
                          const Color& color,
                          float thickness)
        {
            std::array<Vector3, kBoxEdgeCorners.size()> edges;
            for (std::size_t i = 0; i < edges.size(); ++i)
                edges[i] = corners[kBoxEdgeCorners[i]];
            context.DrawColoredLines(edges.data(), edges.size() / 2u, color, thickness);
        }
}

void BuildPlaneBasis(const Vector3& unitNormal, Vector3& tangent, Vector3& bitangent)
{
    // cos(8.1 degrees): past it, world up is too close to the normal to seed a stable tangent.
    constexpr float kSeedSwitchCosine = 0.99f;
    const Vector3 up = (std::fabs(Vector3::Dot(unitNormal, Vector3(0.0f, 1.0f, 0.0f))) > kSeedSwitchCosine)
                           ? Vector3(1.0f, 0.0f, 0.0f)
                           : Vector3(0.0f, 1.0f, 0.0f);
    tangent = Vector3::Cross(up, unitNormal).NormalizeOrZero();
    bitangent = Vector3::Cross(unitNormal, tangent);
}

void ResetGizmoLineGroups(Rendering::ViewId viewId)
{
    auto it = g_LineGroups.find(viewId);
    if (it != g_LineGroups.end())
    {
        it->second.clear();
    }
}

const std::vector<GizmoLineGroup>* GetGizmoLineGroups(Rendering::ViewId viewId)
{
    auto it = g_LineGroups.find(viewId);
    if (it == g_LineGroups.end() || it->second.empty())
    {
        return nullptr;
    }
    return &it->second;
}

		void ResetGizmoTriangleGroups(Rendering::ViewId viewId)
		{
		    auto it = g_TriangleGroups.find(viewId);
		    if (it != g_TriangleGroups.end())
		    {
		        it->second.clear();
		    }
		}
	
		const std::vector<GizmoTriangleGroup>* GetGizmoTriangleGroups(Rendering::ViewId viewId)
		{
		    auto it = g_TriangleGroups.find(viewId);
		    if (it == g_TriangleGroups.end() || it->second.empty())
		    {
		        return nullptr;
		    }
		    return &it->second;
		}

		bool ProjectRayOntoLine(const GizmoRay& ray,
		                        const Vector3& lineOrigin,
		                        const Vector3& lineDirection,
		                        float& outRayT,
		                        float& outLineT,
		                        Vector3& outClosestPoint)
		{
		    const Vector3& rd = ray.direction;
		    const Vector3& ld = lineDirection;
		    const Vector3 w0 = ray.origin - lineOrigin;

		    const float a = Vector3::Dot(rd, rd);
		    const float b = Vector3::Dot(rd, ld);
		    const float c = Vector3::Dot(ld, ld);
		    const float d = Vector3::Dot(rd, w0);
		    const float e = Vector3::Dot(ld, w0);

		    const float denom = a * c - b * b;
		    if (std::fabs(denom) < 1.0e-6f)
		    {
		        return false;
		    }

		    const float invDenom = 1.0f / denom;
		    const float s = (b * e - c * d) * invDenom;
		    const float t = (a * e - b * d) * invDenom;

		    outRayT  = s;
		    outLineT = t;
		    outClosestPoint = lineOrigin + ld * t;
		    return true;
		}

void GizmoRenderContext::DrawColoredLine(const Vector3& from,
                                         const Vector3& to,
                                         const Color& color,
                                         float thickness)
{
    const Vector3 endpoints[2] = {from, to};
    DrawColoredLines(endpoints, 1u, color, thickness);
}

void GizmoRenderContext::DrawColoredLines(const Vector3* vertices,
                                          std::size_t lineCount,
                                          const Color& color,
                                          float thickness)
{
    if (!vertices || lineCount == 0u)
    {
        return;
    }

    GizmoLineGroup& group = GetOrCreateGroup(m_ViewId, color, thickness, m_DepthMode);
    AppendPositions(group.vertices, vertices, lineCount * 2u);
}

void GizmoRenderContext::DrawTriangles(const Vector3* vertices,
                                       std::size_t vertexCount,
                                       const Color& color)
{
    if (!vertices || vertexCount < 3u)
    {
        return;
    }

    GizmoTriangleGroup& group = GetOrCreateTriangleGroup(m_ViewId, color, m_TriangleLayer, m_DepthMode);
    AppendPositions(group.vertices, vertices, vertexCount);
}

void GizmoRenderContext::DrawSolidBox(const Vector3& center,
                                      const Vector3& halfExtents,
                                      const Color& color)
{
    std::vector<Vector3> verts;
    verts.reserve(36u);
    AppendBoxTriangles(verts, Mathematics::BoundingBox{center, halfExtents}.ToAABB().Corners());
    DrawTriangles(verts.data(), verts.size(), color);
}

void GizmoRenderContext::DrawSolidOrientedBox(const Vector3& center,
                                              const Vector3& halfExtents,
                                              const std::array<Vector3, 3>& axes,
                                              const Color& color)
{
    // Scale each axis by the corresponding half extent
    const Vector3 ax = axes[0] * halfExtents.x;
    const Vector3 ay = axes[1] * halfExtents.y;
    const Vector3 az = axes[2] * halfExtents.z;

    // The 8 corners of the oriented box, in AABB::Corners order.
    const std::array<Vector3, 8> v = {
        center - ax - ay - az, // 0: -x -y -z
        center + ax - ay - az, // 1: +x -y -z
        center - ax + ay - az, // 2: -x +y -z
        center + ax + ay - az, // 3: +x +y -z
        center - ax - ay + az, // 4: -x -y +z
        center + ax - ay + az, // 5: +x -y +z
        center - ax + ay + az, // 6: -x +y +z
        center + ax + ay + az  // 7: +x +y +z
    };

    std::vector<Vector3> verts;
    verts.reserve(36u);
    AppendBoxTriangles(verts, v);
    DrawTriangles(verts.data(), verts.size(), color);
}

void GizmoRenderContext::DrawSolidSphere(const Vector3& center,
	                                        float radius,
	                                        const Color& color)
	{
	    if (radius <= 0.0f)
	        return;

	    constexpr int kLatSegments = 8;
	    constexpr int kLonSegments = 16;
	    const float cx = center.x;
	    const float cy = center.y;
	    const float cz = center.z;

	    std::vector<Vector3> verts;
	    verts.reserve(static_cast<std::size_t>(kLatSegments * kLonSegments * 6u));

	    for (int lat = 0; lat < kLatSegments; ++lat)
	    {
	        float v0 = static_cast<float>(lat) / static_cast<float>(kLatSegments);
	        float v1 = static_cast<float>(lat + 1) / static_cast<float>(kLatSegments);
	        float phi0 = v0 * kPi;
	        float phi1 = v1 * kPi;

	        for (int lon = 0; lon < kLonSegments; ++lon)
	        {
	            float u0 = static_cast<float>(lon) / static_cast<float>(kLonSegments);
	            float u1 = static_cast<float>(lon + 1) / static_cast<float>(kLonSegments);
	            float theta0 = u0 * 2.0f * kPi;
	            float theta1 = u1 * 2.0f * kPi;

	            const Vector3 p0(
	                cx + radius * std::sin(phi0) * std::cos(theta0),
	                cy + radius * std::cos(phi0),
	                cz + radius * std::sin(phi0) * std::sin(theta0));
	            const Vector3 p1(
	                cx + radius * std::sin(phi1) * std::cos(theta0),
	                cy + radius * std::cos(phi1),
	                cz + radius * std::sin(phi1) * std::sin(theta0));
	            const Vector3 p2(
	                cx + radius * std::sin(phi1) * std::cos(theta1),
	                cy + radius * std::cos(phi1),
	                cz + radius * std::sin(phi1) * std::sin(theta1));
	            const Vector3 p3(
	                cx + radius * std::sin(phi0) * std::cos(theta1),
	                cy + radius * std::cos(phi0),
	                cz + radius * std::sin(phi0) * std::sin(theta1));

	            AppendTriangle(verts, p0, p1, p2);
	            AppendTriangle(verts, p0, p2, p3);
	        }
	    }

	    DrawTriangles(verts.data(), verts.size(), color);
	}

	void GizmoRenderContext::DrawSolidCylinder(const Vector3& start,
	                                          const Vector3& end,
	                                          float radius,
	                                          const Color& color)
	{
	    if (radius <= 0.0f)
	        return;

	    Vector3 axis = end - start;
	    if (axis.LengthSquared() <= 0.0f)
	        return;

	    axis = axis.NormalizeOrZero();
	    Vector3 tangent;
	    Vector3 bitangent;
	    BuildPlaneBasis(axis, tangent, bitangent);

	    constexpr int kSegments = 16;
	    std::vector<Vector3> verts;
	    verts.reserve(static_cast<std::size_t>(kSegments) * 4u * 3u);

	    for (int i = 0; i < kSegments; ++i)
	    {
	        const float a0 = static_cast<float>(i) * (2.0f * kPi / kSegments);
	        const float a1 = static_cast<float>(i + 1) * (2.0f * kPi / kSegments);
	        const Vector3 dir0 = tangent * std::cos(a0) + bitangent * std::sin(a0);
	        const Vector3 dir1 = tangent * std::cos(a1) + bitangent * std::sin(a1);

	        const Vector3 p0 = start + dir0 * radius;
	        const Vector3 p1 = start + dir1 * radius;
	        const Vector3 q0 = end + dir0 * radius;
	        const Vector3 q1 = end + dir1 * radius;

	        // Side quads
	        AppendTriangle(verts, p0, q0, q1);
	        AppendTriangle(verts, p0, q1, p1);

	        // Caps
	        AppendTriangle(verts, start, p1, p0);
	        AppendTriangle(verts, end,   q0, q1);
	    }

	    DrawTriangles(verts.data(), verts.size(), color);
	}

	void GizmoRenderContext::DrawSolidCone(const Vector3& apex,
	                                      const Vector3& baseCenter,
	                                      float baseRadius,
	                                      const Color& color)
	{
	    if (baseRadius <= 0.0f)
	        return;

	    Vector3 axis = baseCenter - apex;
	    if (axis.LengthSquared() <= 0.0f)
	        return;

	    axis = axis.NormalizeOrZero();
	    Vector3 tangent;
	    Vector3 bitangent;
	    BuildPlaneBasis(axis, tangent, bitangent);

	    constexpr int kSegments = 16;
	    std::vector<Vector3> verts;
	    verts.reserve(static_cast<std::size_t>(kSegments) * 2u * 3u);

	    for (int i = 0; i < kSegments; ++i)
	    {
	        const float a0 = static_cast<float>(i) * (2.0f * kPi / kSegments);
	        const float a1 = static_cast<float>(i + 1) * (2.0f * kPi / kSegments);
	        const Vector3 dir0 = tangent * std::cos(a0) + bitangent * std::sin(a0);
	        const Vector3 dir1 = tangent * std::cos(a1) + bitangent * std::sin(a1);

	        const Vector3 b0 = baseCenter + dir0 * baseRadius;
	        const Vector3 b1 = baseCenter + dir1 * baseRadius;

	        // Side
	        AppendTriangle(verts, apex, b0, b1);
	        // Base disc
	        AppendTriangle(verts, baseCenter, b1, b0);
	    }

	    DrawTriangles(verts.data(), verts.size(), color);
	}

	void GizmoRenderContext::DrawSolidDisc(const Vector3& center,
	                                      const Vector3& normal,
	                                      float radius,
	                                      const Color& color)
	{
	    if (radius <= 0.0f)
	        return;

	    Vector3 tangent;
	    Vector3 bitangent;
	    BuildPlaneBasis(normal.NormalizeOrZero(), tangent, bitangent);

	    constexpr int kSegments = 32;
	    std::vector<Vector3> verts;
	    verts.reserve(static_cast<std::size_t>(kSegments) * 3u);

	    for (int i = 0; i < kSegments; ++i)
	    {
	        const float a0 = static_cast<float>(i) * (2.0f * kPi / kSegments);
	        const float a1 = static_cast<float>(i + 1) * (2.0f * kPi / kSegments);
	        const Vector3 p0 = center + (tangent * std::cos(a0) + bitangent * std::sin(a0)) * radius;
	        const Vector3 p1 = center + (tangent * std::cos(a1) + bitangent * std::sin(a1)) * radius;

	        AppendTriangle(verts, center, p0, p1);
	    }

	    DrawTriangles(verts.data(), verts.size(), color);
	}

	void GizmoRenderContext::DrawWireBox(const Vector3& center,
	                                    const Vector3& halfExtents,
	                                    const Color& color,
	                                    float thickness)
	{
	    DrawBoxEdges(*this, Mathematics::BoundingBox{center, halfExtents}.ToAABB().Corners(), color, thickness);
	}

	void GizmoRenderContext::DrawWireSphere(const Vector3& center,
	                                       float radius,
	                                       const Color& color,
	                                       float thickness)
	{
	    if (radius <= 0.0f)
	        return;

	    DrawWireEllipsoid(*this, center, Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f),
	                      Vector3(0.0f, 0.0f, 1.0f), Vector3(radius, radius, radius), color, thickness);
	}

	void GizmoRenderContext::DrawWireCircle(const Vector3& center,
	                                       const Vector3& normal,
	                                       float radius,
	                                       const Color& color,
	                                       float thickness)
	{
	    if (radius <= 0.0f)
	        return;

	    Vector3 tangent;
	    Vector3 bitangent;
	    BuildPlaneBasis(normal.NormalizeOrZero(), tangent, bitangent);

        DrawWireEllipse(center, tangent * radius, bitangent * radius, color, thickness);
	}

void GizmoRenderContext::DrawWireEllipse(const Vector3& center,
                                        const Vector3& radiusA,
                                        const Vector3& radiusB,
                                        const Color& color,
                                        float thickness)
{
    constexpr int kSegments = 64;
    GizmoLineBatch batch(*this, color, thickness);
    DrawCircleInto(batch, center, radiusA, 1.0f, radiusB, 1.0f, kSegments);
}

void GizmoRenderContext::DrawSolidTorus(const Vector3& center,
                                        const Vector3& normal,
                                        float majorRadius,
                                        float tubeRadius,
                                        const Color& color,
                                        int ringSegments,
                                        int tubeSegments,
                                        const Vector3* cullTowardCameraPos)
{
    if (majorRadius <= 0.0f || tubeRadius <= 0.0f)
        return;

    const Vector3 n = normal.NormalizeOrZero();
    Vector3 tangent;
    Vector3 bitangent;
    BuildPlaneBasis(n, tangent, bitangent);

    // Front-facing cull: project the camera position onto the ring plane to get
    // the in-plane direction pointing toward the viewer. Ring segments whose
    // outward radial direction faces away from that direction are the occluded
    // back arc and are skipped. When the ring is viewed nearly edge-on the
    // in-plane projection degenerates, so culling is disabled and the full ring
    // is drawn.
    bool cullEnabled = false;
    Vector3 cullDir;
    if (cullTowardCameraPos)
    {
        const Vector3 toCam = *cullTowardCameraPos - center;
        cullDir = toCam - n * Vector3::Dot(toCam, n);
        const float cullLen = cullDir.Length();
        if (cullLen > 1.0e-4f)
        {
            cullDir = cullDir / cullLen;
            cullEnabled = true;
        }
    }
    // Keep slightly more than the exact front half so the two ends of the arc
    // land just behind the silhouette rather than cutting exactly on it.
    constexpr float kCullThreshold = -0.12f;

    std::vector<Vector3> verts;
    verts.reserve(static_cast<std::size_t>(ringSegments * tubeSegments * 6u));

    // Point on the tube around ring centre `ringCenter` with outward ring
    // direction `ringDir`, at tube angle `tubeAngle`.
    auto tubePoint = [&](const Vector3& ringCenter, const Vector3& ringDir, float tubeAngle)
    {
        return ringCenter + (ringDir * std::cos(tubeAngle) + n * std::sin(tubeAngle)) * tubeRadius;
    };

    for (int i = 0; i < ringSegments; ++i)
    {
        float t0 = static_cast<float>(i) / static_cast<float>(ringSegments);
        float t1 = static_cast<float>(i + 1) / static_cast<float>(ringSegments);
        float ringAngle0 = t0 * 2.0f * kPi;
        float ringAngle1 = t1 * 2.0f * kPi;

        const Vector3 ringDir0 = tangent * std::cos(ringAngle0) + bitangent * std::sin(ringAngle0);
        const Vector3 ringDir1 = tangent * std::cos(ringAngle1) + bitangent * std::sin(ringAngle1);

        if (cullEnabled && Vector3::Dot(ringDir0 + ringDir1, cullDir) < kCullThreshold * 2.0f)
            continue;

        const Vector3 ringCenter0 = center + ringDir0 * majorRadius;
        const Vector3 ringCenter1 = center + ringDir1 * majorRadius;

        for (int j = 0; j < tubeSegments; ++j)
        {
            float u0 = static_cast<float>(j) / static_cast<float>(tubeSegments);
            float u1 = static_cast<float>(j + 1) / static_cast<float>(tubeSegments);
            float tubeAngle0 = u0 * 2.0f * kPi;
            float tubeAngle1 = u1 * 2.0f * kPi;

            const Vector3 p00 = tubePoint(ringCenter0, ringDir0, tubeAngle0);
            const Vector3 p01 = tubePoint(ringCenter0, ringDir0, tubeAngle1);
            const Vector3 p10 = tubePoint(ringCenter1, ringDir1, tubeAngle0);
            const Vector3 p11 = tubePoint(ringCenter1, ringDir1, tubeAngle1);

            AppendTriangle(verts, p00, p10, p11);
            AppendTriangle(verts, p00, p11, p01);
        }
    }

    DrawTriangles(verts.data(), verts.size(), color);
}

void GizmoRenderContext::DrawTransformedWireBox(const Vector3& localMin,
                                                const Vector3& localMax,
                                                const Mathematics::Matrix4x4& transform,
                                                const Color& color,
                                                float thickness)
{
    std::array<Vector3, 8> corners = Mathematics::AABB{localMin, localMax}.Corners();
    for (Vector3& corner : corners)
        corner = transform.TransformPoint(corner);
    DrawBoxEdges(*this, corners, color, thickness);
}

void DrawCircleInto(GizmoLineBatch& batch,
                    const Mathematics::Vector3& center,
                    const Mathematics::Vector3& axisA, float rA,
                    const Mathematics::Vector3& axisB, float rB,
                    int segments)
{
    Mathematics::Vector3 prev;
    for (int i = 0; i <= segments; ++i)
    {
        const float a = static_cast<float>(i) / static_cast<float>(segments) * 2.0f * kPi;
        const float ca = std::cos(a) * rA;
        const float sa = std::sin(a) * rB;
        const Mathematics::Vector3 p = center + axisA * ca + axisB * sa;
        if (i > 0)
            batch.AddLine(prev, p);
        prev = p;
    }
}

void DrawWireEllipsoid(GizmoRenderContext& context,
                       const Mathematics::Vector3& center,
                       const Mathematics::Vector3& axisX,
                       const Mathematics::Vector3& axisY,
                       const Mathematics::Vector3& axisZ,
                       const Mathematics::Vector3& radii,
                       const Color& color,
                       float thickness,
                       int segments)
{
    GizmoLineBatch batch(context, color, thickness);
    DrawCircleInto(batch, center, axisX, radii.x, axisY, radii.y, segments);
    DrawCircleInto(batch, center, axisY, radii.y, axisZ, radii.z, segments);
    DrawCircleInto(batch, center, axisZ, radii.z, axisX, radii.x, segments);
}

void BuildBillboardBasis(const Mathematics::Vector3& pos,
                         const Mathematics::Vector3& camPos,
                         Mathematics::Vector3& right,
                         Mathematics::Vector3& up)
{
    using Mathematics::Vector3;
    constexpr float kNearVerticalDot = 0.95f;

    const Vector3 toCam = (camPos - pos).NormalizeOrZero();
    Vector3 worldUp(0.0f, 1.0f, 0.0f);
    if (std::fabs(Vector3::Dot(toCam, worldUp)) > kNearVerticalDot)
        worldUp = Vector3(1.0f, 0.0f, 0.0f);

    right = Vector3::Cross(worldUp, toCam).NormalizeOrZero();
    up = Vector3::Cross(toCam, right).NormalizeOrZero();
}

float ComputeGizmoIconWorldRadius(const Mathematics::Vector3& worldPos,
                                  const Mathematics::Vector3& camPos,
                                  float orthoHeight)
{
    // Tuned by eye for typical Scene View FOVs.
    constexpr float kIconWorldFactor = 0.022f;
    constexpr float kIconMinWorld    = 0.05f;
    constexpr float kIconMaxWorld    = 1.5f;

    if (orthoHeight > 0.0f)
        return orthoHeight * kIconWorldFactor * 0.5f;

    const float dist = (worldPos - camPos).Length();
    return std::clamp(dist * kIconWorldFactor, kIconMinWorld, kIconMaxWorld);
}

std::optional<std::size_t> PickNearestGizmoIcon(const GizmoRay& ray,
                                                std::span<const GizmoIconPickSphere> icons)
{
    std::optional<std::size_t> nearest;
    float nearestT = 0.0f;
    for (std::size_t i = 0; i < icons.size(); ++i)
    {
        float t = 0.0f;
        if (Mathematics::IntersectRaySphere(ray, icons[i].Center, icons[i].PickRadius, t) &&
            (!nearest || t < nearestT))
        {
            nearest = i;
            nearestT = t;
        }
    }
    return nearest;
}

} // namespace SceneTools
} // namespace Editor
} // namespace GameEngine
