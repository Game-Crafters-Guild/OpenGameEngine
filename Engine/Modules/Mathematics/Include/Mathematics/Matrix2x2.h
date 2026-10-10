
#pragma once

#include <glm/glm.hpp>

namespace GameEngine {
namespace Mathematics {

// Lightweight 2x2 matrix wrapper over glm::mat2 (column-major).
struct Matrix2x2
{
private:
    glm::mat2 m_Matrix;

public:
    Matrix2x2() : m_Matrix(1.0f) {}
    explicit Matrix2x2(const glm::mat2& m) : m_Matrix(m) {}

    static Matrix2x2 Identity() { return Matrix2x2(glm::mat2(1.0f)); }

    glm::mat2& GetGLM() { return m_Matrix; }
    const glm::mat2& GetGLM() const { return m_Matrix; }

    operator glm::mat2&() { return m_Matrix; }
    operator const glm::mat2&() const { return m_Matrix; }

    float* Data() { return &m_Matrix[0][0]; }
    const float* Data() const { return &m_Matrix[0][0]; }

    glm::vec2& operator[](int column) { return m_Matrix[column]; }
    const glm::vec2& operator[](int column) const { return m_Matrix[column]; }
};

} // namespace Mathematics
} // namespace GameEngine

