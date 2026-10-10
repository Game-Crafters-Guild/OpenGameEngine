
#pragma once

#include <glm/glm.hpp>

namespace GameEngine {
namespace Mathematics {

// Lightweight 3x3 matrix wrapper over glm::mat3 (column-major).
struct Matrix3x3
{
private:
    glm::mat3 m_Matrix;

public:
    Matrix3x3() : m_Matrix(1.0f) {}
    explicit Matrix3x3(const glm::mat3& m) : m_Matrix(m) {}

    static Matrix3x3 Identity() { return Matrix3x3(glm::mat3(1.0f)); }

    glm::mat3& GetGLM() { return m_Matrix; }
    const glm::mat3& GetGLM() const { return m_Matrix; }

    operator glm::mat3&() { return m_Matrix; }
    operator const glm::mat3&() const { return m_Matrix; }

    float* Data() { return &m_Matrix[0][0]; }
    const float* Data() const { return &m_Matrix[0][0]; }

    glm::vec3& operator[](int column) { return m_Matrix[column]; }
    const glm::vec3& operator[](int column) const { return m_Matrix[column]; }
};

} // namespace Mathematics
} // namespace GameEngine

