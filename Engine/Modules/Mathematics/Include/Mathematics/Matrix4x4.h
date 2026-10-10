
#pragma once

#include <cmath>
#include <cstring>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"

namespace GameEngine {
namespace Mathematics {

// Lightweight 4x4 matrix wrapper over glm::mat4 (column-major, column vectors).
struct Matrix4x4
{
private:
    glm::mat4 m_Matrix;

public:
	    Matrix4x4() : m_Matrix(1.0f) {}
	    Matrix4x4(const glm::mat4& m) : m_Matrix(m) {}
	
	    static Matrix4x4 Identity() { return Matrix4x4(glm::mat4(1.0f)); }

	    // From 16 floats in column-major order (the layout of the ECS transform
	    // components' matrix fields).
	    static Matrix4x4 FromColumnMajor(const float columnMajor[16])
	    {
	        Matrix4x4 out;
	        std::memcpy(out.Data(), columnMajor, sizeof(float) * 16u);
	        return out;
	    }
	
	    // Convenience constructors for common camera and transform matrices so
	    // higher-level systems and tests can avoid depending directly on GLM.
	    static Matrix4x4 LookAt(const Vector3& eye,
	                           const Vector3& target,
	                           const Vector3& up)
	    {
	        glm::vec3 e(eye.x, eye.y, eye.z);
	        glm::vec3 c(target.x, target.y, target.z);
	        glm::vec3 u(up.x, up.y, up.z);
	        return Matrix4x4(glm::lookAtLH(e, c, u));
	    }
	
	    [[deprecated("Engine defaults to reverse-Z; use PerspectiveReverseZ.")]]
	    static Matrix4x4 Perspective(float fovYRadians,
	                                 float aspect,
	                                 float zNear,
	                                 float zFar)
	    {
	        return Matrix4x4(glm::perspectiveLH_ZO(fovYRadians, aspect, zNear, zFar));
	    }

	    // Reverse-Z perspective: zNear -> NDC depth 1.0, zFar -> 0.0.
	    static Matrix4x4 PerspectiveReverseZ(float fovYRadians,
	                                         float aspect,
	                                         float zNear,
	                                         float zFar)
	    {
	        const float f = 1.0f / std::tan(fovYRadians * 0.5f);
	        const float invRange = 1.0f / (zFar - zNear);
	        glm::mat4 m(0.0f);
	        m[0][0] = f / aspect;
	        m[1][1] = f;
	        m[2][2] = -zNear * invRange;
	        m[2][3] = 1.0f;
	        m[3][2] = (zNear * zFar) * invRange;
	        return Matrix4x4(m);
	    }
	
	    static Matrix4x4 Translation(const Vector3& translation)
	    {
	        glm::vec3 t(translation.x, translation.y, translation.z);
	        glm::mat4 m(1.0f);
	        m = glm::translate(m, t);
	        return Matrix4x4(m);
	    }

    glm::mat4& GetGLM() { return m_Matrix; }
    const glm::mat4& GetGLM() const { return m_Matrix; }

    operator glm::mat4&() { return m_Matrix; }
    operator const glm::mat4&() const { return m_Matrix; }

    float* Data() { return &m_Matrix[0][0]; }
    const float* Data() const { return &m_Matrix[0][0]; }

    glm::vec4& operator[](int column) { return m_Matrix[column]; }
    const glm::vec4& operator[](int column) const { return m_Matrix[column]; }

    Matrix4x4 operator*(const Matrix4x4& rhs) const
    {
        return Matrix4x4(m_Matrix * rhs.m_Matrix);
    }

    Vector4 Transform(const Vector4& v) const
    {
        glm::vec4 gv(v.x, v.y, v.z, v.w);
        glm::vec4 r = m_Matrix * gv;
        return Vector4(r.x, r.y, r.z, r.w);
    }

	    Vector3 TransformPoint(const Vector3& v) const
	    {
	        glm::vec4 gv(v.x, v.y, v.z, 1.0f);
	        glm::vec4 r = m_Matrix * gv;
	        return Vector3(r.x, r.y, r.z);
	    }

	    // When targeting Vulkan/GLSL, our canonical matrices are already
	    // column-major and have the correct handedness, so no additional
	    // conversion is required. This helper exists so higher-level code can
	    // remain explicit about the intended GPU layout.
	    Matrix4x4 ToVulkan() const
	    {
	        return *this;
	    }
};

} // namespace Mathematics
} // namespace GameEngine

