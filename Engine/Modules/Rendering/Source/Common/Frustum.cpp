#include "Rendering/Common/Frustum.h"

#include <cmath>
#include <glm/gtc/matrix_access.hpp>

namespace GameEngine { namespace Rendering {

	void ExtractFrustumPlanes(const Matrix4x4& viewProj, Vector4* planes)
	{
	    // Matrix4x4 is column-major (GLM-style). Extract rows first, then
	    // build planes from row combinations (row3 ± row{0,1,2}).
	    const glm::mat4& m = viewProj.GetGLM();
	    const glm::vec4 row0 = glm::row(m, 0);
	    const glm::vec4 row1 = glm::row(m, 1);
	    const glm::vec4 row2 = glm::row(m, 2);
	    const glm::vec4 row3 = glm::row(m, 3);

	    auto makePlane = [](const glm::vec4& v) {
	        return Vector4(v.x, v.y, v.z, v.w);
	    };

	    // Left plane
	    planes[0] = makePlane(row3 + row0);

	    // Right plane
	    planes[1] = makePlane(row3 - row0);

	    // Bottom plane
	    planes[2] = makePlane(row3 + row1);

	    // Top plane
	    planes[3] = makePlane(row3 - row1);

		    // Reverse-Z (depth [1,0]): near at z=1 -> row3 - row2; far at z=0 -> row2.
		    planes[4] = makePlane(row3 - row2);

		    // Far plane (z=0 in reverse-Z).
		    planes[5] = makePlane(row2);

	    // Normalize planes
	    for (int i = 0; i < 6; ++i)
	    {
	        Vector3 normal(planes[i].x, planes[i].y, planes[i].z);
	        const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
	        if (length > 0.0f)
	        {
	            const float invLength = 1.0f / length;
	            planes[i].x *= invLength;
	            planes[i].y *= invLength;
	            planes[i].z *= invLength;
	            planes[i].w *= invLength;
	        }
	    }
}

bool TestSphereFrustum(const Vector3& center, float radius, const Vector4* planes)
{
    // Conservative test: if the sphere is completely outside any plane,
    // consider it culled. We allow a small inflation factor to avoid
    // flickering at the frustum edges.
    const float kInflation = 1.5f;
    const float inflatedRadius = radius * kInflation;

    for (int i = 0; i < 6; ++i)
    {
        const Vector4& p = planes[i];
        const float dist = center.x * p.x + center.y * p.y + center.z * p.z + p.w;
        if (dist < -inflatedRadius)
        {
            return false;
        }
    }

    return true;
}

bool TestAabbFrustum(const Vector3& boxMin, const Vector3& boxMax, const Vector4* planes)
{
    for (int i = 0; i < 6; ++i)
    {
        const Vector4& p = planes[i];
        // The corner furthest along the plane normal: if even it is outside,
        // the whole box is.
        const float x = p.x >= 0.0f ? boxMax.x : boxMin.x;
        const float y = p.y >= 0.0f ? boxMax.y : boxMin.y;
        const float z = p.z >= 0.0f ? boxMax.z : boxMin.z;
        if (x * p.x + y * p.y + z * p.z + p.w < 0.0f)
            return false;
    }
    return true;
}

} } // namespace GameEngine::Rendering

